#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROJECT_NAME="NeoBIF"
PROJECT_PREFIX="NEOBIF"
CMAKE_BIN="${CMAKE:-cmake}"
BUILD_DIR="$ROOT_DIR/build"
BUILD_TYPE="Release"
BUILD_WX="ON"
REQUIRE_WX="OFF"
BUILD_CLI="ON"
MINIMAL_RELEASE="ON"
JOBS="${JOBS:-}"
TARGET=""
GENERATOR=""
NEOSHARED_ROOT_VALUE="${NEOSHARED_ROOT:-}"
VCPKG_ROOT_VALUE="${VCPKG_ROOT:-}"
VCPKG_TRIPLET="x64-windows-static"
NO_VCPKG=0
CLEAN=0
EXTRA=()

usage() {
  cat <<USAGE
usage: ./scripts/build.sh [options] [-- extra-cmake-args...]

Options:
  --build-dir DIR          Build directory [default: ./build]
  --build-type TYPE        Debug, Release, RelWithDebInfo, or MinSizeRel
  --wx ON|OFF              Build the wxWidgets GUI target when available
  --require-wx ON|OFF      Fail configure if wxWidgets is unavailable
  --cli ON|OFF             Build the command-line target when supported
  --minimal-release ON|OFF Enable release minimization flags
  --jobs N                 Parallel build jobs
  --target NAME            Build a specific CMake target
  --generator NAME         CMake generator name
  --neoshared-root DIR     Path to the separate neoshared repository
  --vcpkg-root DIR         vcpkg root
  --vcpkg-triplet NAME     vcpkg target triplet
  --no-vcpkg               Do not add the vcpkg toolchain file
  --clean                  Delete the build directory before configuring
  -h, --help               Show this help
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --build-dir) BUILD_DIR="$2"; shift 2;;
    --build-type) BUILD_TYPE="$2"; shift 2;;
    --wx) BUILD_WX="$2"; shift 2;;
    --require-wx) REQUIRE_WX="$2"; shift 2;;
    --cli) BUILD_CLI="$2"; shift 2;;
    --minimal-release) MINIMAL_RELEASE="$2"; shift 2;;
    --jobs|--parallel) JOBS="$2"; shift 2;;
    --target) TARGET="$2"; shift 2;;
    --generator) GENERATOR="$2"; shift 2;;
    --neoshared-root) NEOSHARED_ROOT_VALUE="$2"; shift 2;;
    --vcpkg-root) VCPKG_ROOT_VALUE="$2"; shift 2;;
    --vcpkg-triplet) VCPKG_TRIPLET="$2"; shift 2;;
    --no-vcpkg) NO_VCPKG=1; shift;;
    --clean) CLEAN=1; shift;;
    -h|--help) usage; exit 0;;
    --) shift; EXTRA+=("$@"); break;;
    *) EXTRA+=("$1"); shift;;
  esac
done

if [[ "$BUILD_DIR" != /* ]]; then
  BUILD_DIR="$ROOT_DIR/$BUILD_DIR"
fi
if [[ -n "$NEOSHARED_ROOT_VALUE" && "$NEOSHARED_ROOT_VALUE" != /* ]]; then
  NEOSHARED_ROOT_VALUE="$ROOT_DIR/$NEOSHARED_ROOT_VALUE"
fi

if [[ "$CLEAN" == 1 && -e "$BUILD_DIR" ]]; then
  case "$BUILD_DIR" in
    "$ROOT_DIR"/*) rm -rf -- "$BUILD_DIR";;
    *) echo "Refusing to clean a build directory outside the repository: $BUILD_DIR" >&2; exit 2;;
  esac
fi
mkdir -p "$BUILD_DIR"

CONFIG_ARGS=(-S "$ROOT_DIR" -B "$BUILD_DIR" "-DCMAKE_BUILD_TYPE=$BUILD_TYPE" "-DNEO_MINIMAL_RELEASE=$MINIMAL_RELEASE")
if [[ -n "$GENERATOR" ]]; then
  CONFIG_ARGS=(-G "$GENERATOR" "${CONFIG_ARGS[@]}")
fi

WX_OPTION="${PROJECT_PREFIX}_BUILD_WX_GUI"
REQUIRE_WX_OPTION="${PROJECT_PREFIX}_REQUIRE_WX_GUI"
CLI_OPTION="${PROJECT_PREFIX}_BUILD_CLI"
if grep -q "$WX_OPTION" "$ROOT_DIR/CMakeLists.txt"; then
  CONFIG_ARGS+=("-D$WX_OPTION=$BUILD_WX")
fi
if grep -q "$REQUIRE_WX_OPTION" "$ROOT_DIR/CMakeLists.txt"; then
  CONFIG_ARGS+=("-D$REQUIRE_WX_OPTION=$REQUIRE_WX")
fi
if grep -q "$CLI_OPTION" "$ROOT_DIR/CMakeLists.txt"; then
  CONFIG_ARGS+=("-D$CLI_OPTION=$BUILD_CLI")
fi

if [[ -n "$NEOSHARED_ROOT_VALUE" ]]; then
  [[ -f "$NEOSHARED_ROOT_VALUE/CMakeLists.txt" ]] || {
    echo "neoshared CMakeLists.txt not found under: $NEOSHARED_ROOT_VALUE" >&2
    exit 2
  }
  CONFIG_ARGS+=("-DNEOSHARED_ROOT=$NEOSHARED_ROOT_VALUE")
fi

if [[ "$NO_VCPKG" == 0 && -n "$VCPKG_ROOT_VALUE" && -f "$VCPKG_ROOT_VALUE/scripts/buildsystems/vcpkg.cmake" ]]; then
  CONFIG_ARGS+=("-DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT_VALUE/scripts/buildsystems/vcpkg.cmake")
  [[ -z "$VCPKG_TRIPLET" ]] || CONFIG_ARGS+=("-DVCPKG_TARGET_TRIPLET=$VCPKG_TRIPLET")
fi
CONFIG_ARGS+=("${EXTRA[@]+"${EXTRA[@]}"}")

printf 'Configuring %s in %s\n' "$PROJECT_NAME" "$BUILD_DIR"
"$CMAKE_BIN" "${CONFIG_ARGS[@]}"

BUILD_ARGS=(--build "$BUILD_DIR" --config "$BUILD_TYPE")
[[ -z "$JOBS" || "$JOBS" == 0 ]] || BUILD_ARGS+=(--parallel "$JOBS")
[[ -z "$TARGET" ]] || BUILD_ARGS+=(--target "$TARGET")

printf 'Building %s\n' "$PROJECT_NAME"
"$CMAKE_BIN" "${BUILD_ARGS[@]}"
