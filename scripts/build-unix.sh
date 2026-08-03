#!/usr/bin/env bash
set -Eeuo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CORE_DIR="$ROOT_DIR/ScopeOneCore"
EXTERNAL_DIR="$CORE_DIR/external"
MM_REPO_DIR="$EXTERNAL_DIR/micro-manager"
MMCORE_LINK="$EXTERNAL_DIR/mmCoreAndDevices"
BUILD_TYPE="${BUILD_TYPE:-Release}"

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
    echo "This script supports only Linux and macOS." >&2
    exit 1
    ;;
esac

JOBS="${JOBS:-$DEFAULT_JOBS}"
DEMO_ADAPTER="$MMCORE_LINK/DeviceAdapters/DemoCamera/.libs/$DEMO_ADAPTER_NAME"
UTILITIES_ADAPTER="$MMCORE_LINK/DeviceAdapters/Utilities/.libs/$UTILITIES_ADAPTER_NAME"

step() {
  printf '\n==> %s\n' "$1"
}

run() {
  printf '+'
  printf ' %q' "$@"
  printf '\n'
  "$@"
}

require_command() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "Missing required command: $1" >&2
    exit 1
  fi
}

for command_name in git cmake make; do
  require_command "$command_name"
done

step "Initializing ScopeOne submodules"
run git -C "$ROOT_DIR" submodule update --init --recursive

step "Preparing Micro-Manager checkout for $PLATFORM_NAME"
run mkdir -p "$EXTERNAL_DIR"

if [[ -d "$MM_REPO_DIR/.git" ]]; then
  echo "Using existing Micro-Manager checkout: $MM_REPO_DIR"
elif [[ -e "$MM_REPO_DIR" ]]; then
  echo "Path exists but is not a git checkout: $MM_REPO_DIR" >&2
  exit 1
else
  run git clone --recurse-submodules https://github.com/micro-manager/micro-manager.git "$MM_REPO_DIR"
fi

if [[ -e "$MMCORE_LINK" && ! -L "$MMCORE_LINK" ]]; then
  echo "Path exists and is not a symlink: $MMCORE_LINK" >&2
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
run cmake -S "$CORE_DIR" -B "$CORE_DIR/build" -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
run cmake --build "$CORE_DIR/build" --parallel "$JOBS"
run cmake --install "$CORE_DIR/build"

step "Building ScopeOne GUI"
run cmake -S "$ROOT_DIR" -B "$ROOT_DIR/build" -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
run cmake --build "$ROOT_DIR/build" --parallel "$JOBS"

step "Copying demo adapters"
if [[ ! -f "$DEMO_ADAPTER" ]]; then
  echo "DemoCamera adapter was not found: $DEMO_ADAPTER" >&2
  exit 1
fi
if [[ ! -f "$UTILITIES_ADAPTER" ]]; then
  echo "Utilities adapter was not found: $UTILITIES_ADAPTER" >&2
  exit 1
fi
run cp -L "$DEMO_ADAPTER" "$UTILITIES_ADAPTER" "$ROOT_DIR/build/"

step "Done"
echo "GUI executable: $ROOT_DIR/build/ScopeOne"
echo "Demo configuration: $ROOT_DIR/config/MMConfig_demo.cfg"
