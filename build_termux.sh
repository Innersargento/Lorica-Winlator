#!/data/data/com.termux/files/usr/bin/env bash
# ==============================================================================
# Lorica Build Script for Termux (Native Android ARM64)
# ==============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

XSYS_DIR="$SCRIPT_DIR/.termux_xsys"
PREFIX_DIR="${PREFIX:-/data/data/com.termux/files/usr}"
BUILD_DIR="$SCRIPT_DIR/build"
DIST_DIR="$SCRIPT_DIR/dist"

# Display colors
GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m' # No Color

info()  { echo -e "${BLUE}[*]${NC} $*"; }
ok()    { echo -e "${GREEN}[+]${NC} $*"; }
warn()  { echo -e "${YELLOW}[!]${NC} $*"; }
err()   { echo -e "${RED}[-]${NC} $*"; }

# ------------------------------------------------------------------------------
# 1. Check and install Termux dependencies
# ------------------------------------------------------------------------------
MISSING_PKGS=()

check_cmd() {
    command -v "$1" >/dev/null 2>&1
}

info "Checking Termux dependencies..."

check_cmd clang      || MISSING_PKGS+=("clang")
check_cmd cmake      || MISSING_PKGS+=("cmake")
check_cmd ninja      || MISSING_PKGS+=("ninja")
check_cmd llvm-strip || MISSING_PKGS+=("llvm")
check_cmd zstd       || MISSING_PKGS+=("zstd")

# Required headers
if [[ ! -f "$PREFIX_DIR/include/android/log.h" ]]; then
    MISSING_PKGS+=("ndk-sysroot")
fi
if [[ ! -f "$PREFIX_DIR/include/X11/Xlib.h" ]]; then
    MISSING_PKGS+=("libx11")
fi
if [[ ! -f "$PREFIX_DIR/include/xcb/xcb.h" ]]; then
    MISSING_PKGS+=("libxcb")
fi
if [[ ! -f "$PREFIX_DIR/include/X11/Xproto.h" && ! -f "$PREFIX_DIR/include/X11/X.h" ]]; then
    MISSING_PKGS+=("xorgproto")
fi

if [[ ${#MISSING_PKGS[@]} -gt 0 ]]; then
    warn "Missing packages detected: ${MISSING_PKGS[*]}"
    info "Installing dependencies via apt..."
    apt-get update -y
    apt-get install -y "${MISSING_PKGS[@]}"
    ok "Dependencies installed successfully."
else
    ok "All Termux dependencies are already installed."
fi

# ------------------------------------------------------------------------------
# 2. Check and generate X11 / XCB stubs for Winlator
# ------------------------------------------------------------------------------
STUBS_VALID=true
REQUIRED_STUBS=(
    "libX11.so"
    "libxcb.so"
    "libX11-xcb.so"
    "libxcb-dri3.so"
    "libxcb-present.so"
    "libpthread.a"
    "liblog.so"
)

if [[ ! -d "$XSYS_DIR/include/X11" || ! -d "$XSYS_DIR/include/xcb" ]]; then
    STUBS_VALID=false
else
    for stub in "${REQUIRED_STUBS[@]}"; do
        if [[ ! -e "$XSYS_DIR/lib/$stub" ]]; then
            STUBS_VALID=false
            break
        fi
    done
fi

if [[ "$STUBS_VALID" == false ]]; then
    info "Creating X11/XCB stubs environment in $XSYS_DIR..."
    rm -rf "$XSYS_DIR"
    mkdir -p "$XSYS_DIR/include" "$XSYS_DIR/lib"

    # Copy X11 and XCB headers
    cp -r "$PREFIX_DIR/include/X11" "$PREFIX_DIR/include/xcb" "$XSYS_DIR/include/"

    # Create stubs with the exact SONAMEs expected by Winlator
    EMPTY_C="$XSYS_DIR/empty.c"
    touch "$EMPTY_C"

    mk_stub() {
        clang -shared -o "$XSYS_DIR/lib/$1" -Wl,-soname,"$2" "$EMPTY_C"
    }

    mk_stub libX11.so         libX11.so.6
    mk_stub libxcb.so         libxcb.so.1
    mk_stub libX11-xcb.so     libX11-xcb.so.1
    mk_stub libxcb-dri3.so    libxcb-dri3.so.0
    mk_stub libxcb-present.so libxcb-present.so.0
    llvm-ar rc "$XSYS_DIR/lib/libpthread.a"
    ln -sf /system/lib64/liblog.so "$XSYS_DIR/lib/liblog.so"

    ok "X11/XCB stubs created successfully."
else
    ok "X11/XCB stubs already exist and are ready."
fi

# ------------------------------------------------------------------------------
# 3. Configure CMake
# ------------------------------------------------------------------------------
if [[ "${1:-}" == "--clean" ]]; then
    info "Cleaning previous build directory (--clean)..."
    rm -rf "$BUILD_DIR" "$DIST_DIR"
fi

if [[ ! -f "$BUILD_DIR/build.ninja" ]]; then
    info "Configuring CMake for Android ARM64 (API 29)..."
    cmake -S "$SCRIPT_DIR" -B "$BUILD_DIR" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DANDROID=ON \
        -DNOX11=OFF \
        -DNOEGL=OFF \
        -DGBM=OFF \
        -DUSE_ANDROID_LOG=ON \
        -Dlog-lib="$XSYS_DIR/lib/liblog.so" \
        "-DCMAKE_C_FLAGS=--target=aarch64-linux-android29 -D__ANDROID_API__=29 -I$XSYS_DIR/include -fno-math-errno" \
        "-DCMAKE_SHARED_LINKER_FLAGS=--target=aarch64-linux-android29 -L$XSYS_DIR/lib -Wl,--allow-shlib-undefined -Wl,-z,undefs"
    ok "Configuration complete."
else
    info "Existing build detected in $BUILD_DIR. Skipping configuration step."
fi

# ------------------------------------------------------------------------------
# 4. Compile with Ninja
# ------------------------------------------------------------------------------
NPROC="$(nproc 2>/dev/null || echo 4)"
info "Starting compilation using $NPROC threads..."
cmake --build "$BUILD_DIR" -j"$NPROC"
ok "Compilation complete!"

# ------------------------------------------------------------------------------
# 5. Package Driver (opengl_lorica.tzst)
# ------------------------------------------------------------------------------
info "Packaging driver..."
mkdir -p "$DIST_DIR/usr/lib"
cp "$SCRIPT_DIR/lib/libGL.so.1" "$DIST_DIR/usr/lib/libGL.so.1"
llvm-strip "$DIST_DIR/usr/lib/libGL.so.1"

(
    cd "$DIST_DIR"
    tar --zstd -cvf opengl_lorica.tzst usr >/dev/null
)

ok "Driver packaged successfully at:"
echo -e "${GREEN}  -> $DIST_DIR/opengl_lorica.tzst${NC}"

# If Android Download directory exists, suggest the copy command
if [[ -d "/storage/emulated/0/Download" ]]; then
    echo ""
    info "To copy to phone's Download directory (Drive D:\\ in Winlator):"
    echo -e "  cp \"$DIST_DIR/opengl_lorica.tzst\" /storage/emulated/0/Download/"
fi
