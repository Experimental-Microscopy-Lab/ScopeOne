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
  GREEN=$'\033[32m'
  YELLOW=$'\033[33m'
  RED=$'\033[31m'
  RESET=$'\033[0m'
else
  BOLD_BLUE=""
  GREEN=""
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

ensure_build_tools() {
  local missing_commands=()

  # Prints one line per tool and records the missing ones. Any of the listed commands satisfies the tool
  check_tool() {
    local label="$1" purpose="$2" command_name
    shift 2
    for command_name in "$@"; do
      if command -v "$command_name" >/dev/null 2>&1; then
        printf '  %s✓%s %-12s %s\n' "$GREEN" "$RESET" "$label" "$purpose"
        return
      fi
    done
    printf '  %s✗%s %-12s %s\n' "$RED" "$RESET" "$label" "$purpose"
    missing_commands+=("$label")
  }

  step "Checking build tools"
  check_tool git "fetches the submodules and Micro-Manager" git
  check_tool cmake "configures ScopeOne and ScopeOneCore" cmake
  check_tool make "builds Micro-Manager" make
  check_tool autoreconf "generates Micro-Manager's configure script" autoreconf
  check_tool automake "generates Micro-Manager's makefiles" automake
  # Micro-Manager is built with autotools, which Homebrew installs as glibtoolize
  check_tool libtool "builds Micro-Manager's shared libraries" libtoolize glibtoolize
  check_tool pkg-config "locates libraries while configuring" pkg-config
  # The compiler line shows the first one found, which is the one the build tools pick up
  local compiler_info="compiles everything (g++ or clang++)" compiler_command
  for compiler_command in c++ g++ clang++; do
    if command -v "$compiler_command" >/dev/null 2>&1; then
      compiler_info="$("$compiler_command" --version | sed -n 1p)"
      break
    fi
  done
  check_tool "C++ compiler" "$compiler_info" c++ g++ clang++
  if (( ${#missing_commands[@]} == 0 )); then
    return
  fi
  error "Missing build tools: ${missing_commands[*]}"
  error "Install them with your package manager and run this script again. For example:"
  cat >&2 <<'EOF_TOOLS'

  Debian, Ubuntu:  sudo apt install git cmake build-essential autoconf automake libtool pkg-config
  Fedora:          sudo dnf install git cmake gcc-c++ make autoconf automake libtool pkgconf-pkg-config
  macOS:           brew install git cmake autoconf automake libtool pkg-config

Package names differ between distributions. Step 1 of the Linux and macOS build steps in the README lists everything the build needs.
EOF_TOOLS
  exit 1
}

ensure_vcpkg() {
  if [[ -z "${VCPKG_ROOT:-}" || ! -f "$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" ]]; then
    local vcpkg_root="$HOME/vcpkg"
    if [[ ! -f "$vcpkg_root/scripts/buildsystems/vcpkg.cmake" ]]; then
      if [[ "$ASSUME_YES" != true ]]; then
        if [[ ! -t 0 ]]; then
          error "Set VCPKG_ROOT to a bootstrapped vcpkg checkout, or run this script in a terminal."
          exit 1
        fi
        local answer
        echo "vcpkg is a C++ package manager. ScopeOne uses it to download and build Qt, OpenCV and its other libraries, and nothing is installed system wide."
        read -r -p "${YELLOW}vcpkg was not found. Download it into $vcpkg_root? Press Enter to accept, type another folder, or n to cancel:$RESET " answer
        case "$answer" in
          "") ;;
          n|N|no|No) error "Set VCPKG_ROOT to a bootstrapped vcpkg checkout and run this script again."; exit 1 ;;
          *) vcpkg_root="${answer/#\~/$HOME}" ;;
        esac
      fi
      if [[ ! -f "$vcpkg_root/scripts/buildsystems/vcpkg.cmake" ]]; then
        run git clone https://github.com/microsoft/vcpkg.git "$vcpkg_root"
      fi
    fi
    export VCPKG_ROOT="$vcpkg_root"
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
if [[ ! -d "$ROOT_DIR/vcpkg_installed/$VCPKG_TRIPLET" ]]; then
  echo "The first build compiles Qt and OpenCV with vcpkg and can take a long time. Later builds reuse the result."
fi
run cmake -S "$CORE_DIR" -B "$CORE_DIR/build" -DCMAKE_BUILD_TYPE="$BUILD_TYPE" "${VCPKG_ARGS[@]}"
run cmake --build "$CORE_DIR/build" --parallel "$JOBS"
run cmake --install "$CORE_DIR/build"

step "Building and installing ScopeOne drivers"
run cmake -S "$ROOT_DIR/drivers" -B "$ROOT_DIR/drivers/build" -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
  -DScopeOneCore_ROOT="$CORE_DIR/install" -DCMAKE_PREFIX_PATH="$CORE_DIR/install" \
  -DCMAKE_INSTALL_PREFIX="$CORE_DIR/install/bin" "${VCPKG_ARGS[@]}"
run cmake --build "$ROOT_DIR/drivers/build" --parallel "$JOBS"
run cmake --install "$ROOT_DIR/drivers/build"

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

# Core builds the CUDA and ONNX modules only when it finds their toolkits, so a missing one
# leaves them out without failing the build
core_module() {
  local value
  value="$(sed -n "s/^$1:INTERNAL=//p" "$CORE_DIR/build/CMakeCache.txt")"
  if [[ "$value" == ON ]]; then
    printf '%son%s' "$GREEN" "$RESET"
  else
    printf '%soff%s' "$YELLOW" "$RESET"
  fi
}

step "Done"
echo "GUI executable: $ROOT_DIR/build/ScopeOne"
echo "Demo configuration: $ROOT_DIR/config/MMConfig_demo.cfg"
echo "Core modules: CUDA: $(core_module SCOPEONE_BUILT_WITH_CUDA), ONNX: $(core_module SCOPEONE_BUILT_WITH_INFERENCE)"
