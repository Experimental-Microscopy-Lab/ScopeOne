#!/usr/bin/env bash
set -Eeuo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CORE_DIR="$ROOT_DIR/ScopeOneCore"
EXTERNAL_DIR="$CORE_DIR/external"
MM_REPO_DIR="$EXTERNAL_DIR/micro-manager"
MMCORE_LINK="$EXTERNAL_DIR/mmCoreAndDevices"
BUILD_TYPE="${BUILD_TYPE:-Release}"
JOBS="${JOBS:-}"

if [[ "$(uname -s)" != Linux* ]]; then
  echo "This script is intended for Linux builds." >&2
  exit 1
fi

if [[ -z "$JOBS" ]]; then
  if command -v nproc >/dev/null 2>&1; then
    JOBS="$(nproc)"
  else
    JOBS="1"
  fi
fi

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

step "Preparing Micro-Manager checkout"
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

step "Building Micro-Manager"
(
  cd "$MM_REPO_DIR"
  run git submodule update --init --recursive
  run ./autogen.sh
  run ./configure --without-java --enable-static
  run make fetchdeps
  run make -j "$JOBS"
)

step "Building and installing ScopeOneCore"
run cmake -S "$CORE_DIR" -B "$CORE_DIR/build" -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
run cmake --build "$CORE_DIR/build" --parallel "$JOBS"
run cmake --install "$CORE_DIR/build"

step "Building ScopeOne GUI"
run cmake -S "$ROOT_DIR" -B "$ROOT_DIR/build" -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
run cmake --build "$ROOT_DIR/build" --parallel "$JOBS"

step "Creating Linux runtime symlinks"
agent_src="$CORE_DIR/install/bin/ScopeOne_Agent"
agent_dst="$ROOT_DIR/build/ScopeOne_Agent.exe"
if [[ -f "$agent_src" ]]; then
  run ln -sfn "$agent_src" "$agent_dst"
else
  echo "Skipping agent symlink; missing: $agent_src" >&2
fi

demo_adapter_src="$MMCORE_LINK/DeviceAdapters/DemoCamera/.libs/libmmgr_dal_DemoCamera.so"
demo_adapter_dst="$ROOT_DIR/build/libmmgr_dal_DemoCamera.so"
if [[ -f "$demo_adapter_src" ]]; then
  run ln -sfn "$demo_adapter_src" "$demo_adapter_dst"
else
  echo "Skipping DemoCamera adapter symlink; missing: $demo_adapter_src" >&2
fi

step "Done"
echo "GUI executable: $ROOT_DIR/build/ScopeOne"
