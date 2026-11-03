#!/usr/bin/env bash
set -Eeuo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CORE_DIR="$ROOT_DIR/ScopeOneCore"
EXTERNAL_DIR="$CORE_DIR/external"
MM_REPO_DIR="$EXTERNAL_DIR/micro-manager"
MMCORE_LINK="$EXTERNAL_DIR/mmCoreAndDevices"
BUILD_TYPE="${BUILD_TYPE:-Release}"
CLEAN=false
ASSUME_YES=false

# Colors are used only on a terminal, and NO_COLOR turns them off
if [[ -t 1 && -t 2 && -z "${NO_COLOR:-}" ]]; then
  BOLD_BLUE=$'\033[1;34m'
  YELLOW=$'\033[33m'
  RED=$'\033[31m'
  RESET=$'\033[0m'
else
  BOLD_BLUE=""
  YELLOW=""
  RED=""
  RESET=""
fi

error() {
  printf '%s%s%s\n' "$RED" "$1" "$RESET" >&2
}

for argument in "$@"; do
  case "$argument" in
    --clean) CLEAN=true ;;
    --yes|-y) ASSUME_YES=true ;;
    *) error "Unknown option: $argument"; exit 1 ;;
  esac
done

case "$(uname -s)" in
  Linux*)
    PLATFORM_NAME="Linux"
    DEMO_ADAPTER_NAME="libmmgr_dal_DemoCamera.so.0"
    UTILITIES_ADAPTER_NAME="libmmgr_dal_Utilities.so.0"
    DEFAULT_JOBS="$(nproc)"
    ;;
  Darwin*)
    PLATFORM_NAME="macOS"
    DEMO_ADAPTER_NAME="libmmgr_dal_DemoCamera"
    UTILITIES_ADAPTER_NAME="libmmgr_dal_Utilities"
    DEFAULT_JOBS="$(sysctl -n hw.logicalcpu)"
    ;;
  *)
    error "This script supports only Linux and macOS."
    exit 1
    ;;
esac

step() {
  printf '\n%s==> %s%s\n' "$BOLD_BLUE" "$1" "$RESET"
}

run() {
  printf '+'
  printf ' %q' "$@"
  printf '\n'
  "$@"
}

# Asks a yes or no question. --yes answers yes, and without a terminal the answer is no.
confirm() {
  if [[ "$ASSUME_YES" == true ]]; then
    return 0
  fi
  if [[ ! -t 0 ]]; then
    return 1
  fi
  local answer
  read -r -p "$YELLOW$1 [Y/n]$RESET " answer
  [[ -z "$answer" || "$answer" =~ ^[Yy] ]]
}

install_build_tools() {
  if [[ "$PLATFORM_NAME" == "macOS" ]]; then
    if ! xcode-select -p >/dev/null 2>&1; then
      run xcode-select --install
      error "Finish installing the command line tools in the window that opened, then run this script again."
      exit 1
    fi
    if ! command -v brew >/dev/null 2>&1; then
      error "Homebrew installs the build tools. Get it from https://brew.sh, then run this script again."
      exit 1
    fi
    run brew install git subversion cmake autoconf automake libtool autoconf-archive pkg-config ninja boost
  elif command -v apt-get >/dev/null 2>&1; then
    local sudo=()
    if [[ $EUID -ne 0 ]]; then
      sudo=(sudo)
    fi
    run "${sudo[@]}" apt-get update
    run "${sudo[@]}" apt-get install -y \
      git subversion build-essential cmake autoconf automake libtool autoconf-archive \
      pkg-config ninja-build curl zip unzip libboost-all-dev
    run "${sudo[@]}" apt-get install -y --no-upgrade \
      libgl-dev libegl-dev libopengl-dev libx11-dev libx11-xcb-dev libxrender-dev \
      libxcb1-dev libxcb-cursor-dev libxcb-icccm4-dev libxcb-util-dev libxcb-image0-dev \
      libxcb-keysyms1-dev libxcb-randr0-dev libxcb-render-util0-dev libxcb-shape0-dev \
      libxcb-shm0-dev libxcb-sync-dev libxcb-xfixes0-dev libxcb-xkb-dev libxcb-xinput-dev \
      libxcb-glx0-dev libxkbcommon-dev libxkbcommon-x11-dev
  else
    error "Install the build tools listed in the README with your package manager, then run this script again."
    exit 1
  fi
}

ensure_build_tools() {
  # Micro-Manager is built with autotools, which Homebrew installs as glibtoolize
  local missing_commands=()
  local command_name
  for command_name in git cmake make autoreconf automake pkg-config; do
    command -v "$command_name" >/dev/null 2>&1 || missing_commands+=("$command_name")
  done
  command -v libtoolize >/dev/null 2>&1 || command -v glibtoolize >/dev/null 2>&1 || missing_commands+=("libtool")
  if (( ${#missing_commands[@]} == 0 )); then
    return
  fi
  echo "Missing build tools: ${missing_commands[*]}"
  if ! confirm "Install the build tools with the system package manager?"; then
    error "Install the build tools listed in the README, or run this script in a terminal and answer yes."
    exit 1
  fi
  install_build_tools
}

ensure_vcpkg() {
  if [[ -z "${VCPKG_ROOT:-}" || ! -f "$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" ]]; then
    local default_root="$HOME/vcpkg"
    if [[ ! -f "$default_root/scripts/buildsystems/vcpkg.cmake" ]]; then
      if ! confirm "vcpkg was not found. Download it into $default_root?"; then
        error "Set VCPKG_ROOT to a bootstrapped vcpkg checkout, or run this script in a terminal and answer yes."
        exit 1
      fi
      run git clone https://github.com/microsoft/vcpkg.git "$default_root"
    fi
    export VCPKG_ROOT="$default_root"
    echo "Using vcpkg in $VCPKG_ROOT. Add export VCPKG_ROOT=\"$VCPKG_ROOT\" to your shell profile to skip this next time."
  fi
  if [[ ! -x "$VCPKG_ROOT/vcpkg" ]]; then
    run "$VCPKG_ROOT/bootstrap-vcpkg.sh" -disableMetrics
  fi
}

ensure_build_tools
ensure_vcpkg

case "$(uname -m)" in
  x86_64|amd64)
    VCPKG_TRIPLET="$([[ "$PLATFORM_NAME" == "Linux" ]] && echo x64-linux-dynamic || echo x64-osx-dynamic)"
    ;;
  aarch64|arm64)
    VCPKG_TRIPLET="$([[ "$PLATFORM_NAME" == "Linux" ]] && echo arm64-linux-dynamic || echo arm64-osx-dynamic)"
    ;;
  *)
    error "Unsupported host architecture: $(uname -m)"
    exit 1
    ;;
esac

VCPKG_ARGS=(
  "-DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
  "-DVCPKG_MANIFEST_DIR=$ROOT_DIR"
  "-DVCPKG_INSTALLED_DIR=$ROOT_DIR/vcpkg_installed"
  "-DVCPKG_TARGET_TRIPLET=$VCPKG_TRIPLET"
  "-DVCPKG_HOST_TRIPLET=$VCPKG_TRIPLET"
)

JOBS="${JOBS:-$DEFAULT_JOBS}"
DEMO_ADAPTER="$MMCORE_LINK/DeviceAdapters/DemoCamera/.libs/$DEMO_ADAPTER_NAME"
UTILITIES_ADAPTER="$MMCORE_LINK/DeviceAdapters/Utilities/.libs/$UTILITIES_ADAPTER_NAME"

if [[ "$CLEAN" == true ]]; then
  rm -rf "$CORE_DIR/build" "$ROOT_DIR/build"
fi

step "Initializing ScopeOne submodules"
run git -C "$ROOT_DIR" submodule update --init --recursive

step "Preparing Micro-Manager checkout for $PLATFORM_NAME"
run mkdir -p "$EXTERNAL_DIR"

if [[ -d "$MM_REPO_DIR/.git" ]]; then
  echo "Using existing Micro-Manager checkout: $MM_REPO_DIR"
elif [[ -e "$MM_REPO_DIR" ]]; then
  error "Path exists but is not a git checkout: $MM_REPO_DIR"
  exit 1
else
  run git clone --recurse-submodules https://github.com/micro-manager/micro-manager.git "$MM_REPO_DIR"
fi

if [[ -e "$MMCORE_LINK" && ! -L "$MMCORE_LINK" ]]; then
  error "Path exists and is not a symlink: $MMCORE_LINK"
  exit 1
fi
run ln -sfn micro-manager/mmCoreAndDevices "$MMCORE_LINK"

step "Building required Micro-Manager components"
(
  cd "$MM_REPO_DIR"
  run git submodule update --init --recursive
  run ./autogen.sh
  run ./configure --without-java --enable-static
  run make -C "$MMCORE_LINK/MMDevice" -j "$JOBS"
  run make -C "$MMCORE_LINK/MMCore" -j "$JOBS"
  run make -C "$MMCORE_LINK/DeviceAdapters/DemoCamera" -j "$JOBS"
  run make -C "$MMCORE_LINK/DeviceAdapters/Utilities" -j "$JOBS"
)

step "Building and installing ScopeOneCore"
run cmake -S "$CORE_DIR" -B "$CORE_DIR/build" -DCMAKE_BUILD_TYPE="$BUILD_TYPE" "${VCPKG_ARGS[@]}"
run cmake --build "$CORE_DIR/build" --parallel "$JOBS"
run cmake --install "$CORE_DIR/build"

step "Building and installing ScopeOne plugins"
run cmake -S "$ROOT_DIR/plugins" -B "$ROOT_DIR/build/plugins" -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
  -DScopeOneCore_ROOT="$CORE_DIR/install" -DCMAKE_PREFIX_PATH="$CORE_DIR/install" \
  -DCMAKE_INSTALL_PREFIX="$CORE_DIR/install/bin" "${VCPKG_ARGS[@]}"
run cmake --build "$ROOT_DIR/build/plugins" --parallel "$JOBS"
run cmake --install "$ROOT_DIR/build/plugins"

step "Building ScopeOne GUI"
run cmake -S "$ROOT_DIR" -B "$ROOT_DIR/build" -DCMAKE_BUILD_TYPE="$BUILD_TYPE" "${VCPKG_ARGS[@]}"
run cmake --build "$ROOT_DIR/build" --parallel "$JOBS"

step "Copying demo adapters"
if [[ ! -f "$DEMO_ADAPTER" ]]; then
  error "DemoCamera adapter was not found: $DEMO_ADAPTER"
  exit 1
fi
if [[ ! -f "$UTILITIES_ADAPTER" ]]; then
  error "Utilities adapter was not found: $UTILITIES_ADAPTER"
  exit 1
fi
run cp -L "$DEMO_ADAPTER" "$UTILITIES_ADAPTER" "$ROOT_DIR/build/"

step "Done"
echo "GUI executable: $ROOT_DIR/build/ScopeOne"
echo "Demo configuration: $ROOT_DIR/config/MMConfig_demo.cfg"
