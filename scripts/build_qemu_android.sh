#!/bin/bash
# Build QEMU statically for ARM64 Android (no KVM, TCG only)
# Dependencies: glib, pixman, libslirp, libffi, zlib — all built from source
set -euo pipefail

NDK="$HOME/Android/Sdk/ndk/27.3.13750724"
TOOLCHAIN="$NDK/toolchains/llvm/prebuilt/linux-x86_64"
SYSROOT="$TOOLCHAIN/sysroot"
API=26
CC="$TOOLCHAIN/bin/aarch64-linux-android${API}-clang"
CXX="$TOOLCHAIN/bin/aarch64-linux-android${API}-clang++"
AR="$TOOLCHAIN/bin/llvm-ar"
RANLIB="$TOOLCHAIN/bin/llvm-ranlib"
STRIP="$TOOLCHAIN/bin/llvm-strip"

BUILD_DIR="/tmp/qemu-android-build"
PREFIX="$BUILD_DIR/prefix"
JOBS=$(nproc)

export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:$PREFIX/lib64/pkgconfig:$PREFIX/share/pkgconfig"
export PKG_CONFIG_LIBDIR="$PKG_CONFIG_PATH"
export PKG_CONFIG_SYSROOT_DIR=""

mkdir -p "$BUILD_DIR/src" "$PREFIX"/{lib,include}

echo "=== Building QEMU for ARM64 Android (static, TCG) ==="
echo "NDK: $NDK"
echo "CC: $CC"
echo "PREFIX: $PREFIX"
echo "JOBS: $JOBS"
echo ""

# --- 1. zlib ---
build_zlib() {
    echo "=== [1/5] Building zlib ==="
    cd "$BUILD_DIR/src"
    if [ ! -d zlib-1.3.1 ]; then
        curl -sL -o zlib.tar.gz "https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz" && tar xzf zlib.tar.gz && rm zlib.tar.gz
    fi
    cd zlib-1.3.1
    CC="$CC" AR="$AR" RANLIB="$RANLIB" \
        ./configure --prefix="$PREFIX" --static
    make -j$JOBS
    make install
    echo "zlib: OK"
}

# --- 2. libffi (needed by glib) ---
build_libffi() {
    echo "=== [2/5] Building libffi ==="
    cd "$BUILD_DIR/src"
    if [ ! -d libffi-3.4.6 ]; then
        curl -sL -o libffi.tar.gz "https://github.com/libffi/libffi/releases/download/v3.4.6/libffi-3.4.6.tar.gz" && tar xzf libffi.tar.gz && rm libffi.tar.gz
    fi
    cd libffi-3.4.6
    ./configure \
        --host=aarch64-linux-android \
        --prefix="$PREFIX" \
        --enable-static --disable-shared \
        CC="$CC" CXX="$CXX" AR="$AR" RANLIB="$RANLIB"
    make -j$JOBS
    make install
    echo "libffi: OK"
}

# --- 2b. libiconv (needed by glib on Android) ---
build_libiconv() {
    echo "=== [2b/5] Building libiconv ==="
    cd "$BUILD_DIR/src"
    if [ ! -d libiconv-1.17 ]; then
        curl -sL -o libiconv.tar.gz "https://ftp.gnu.org/pub/gnu/libiconv/libiconv-1.17.tar.gz" && tar xzf libiconv.tar.gz && rm libiconv.tar.gz
    fi
    cd libiconv-1.17
    ./configure \
        --host=aarch64-linux-android \
        --prefix="$PREFIX" \
        --enable-static --disable-shared \
        CC="$CC" CXX="$CXX" AR="$AR" RANLIB="$RANLIB"
    make -j$JOBS
    make install
    # Create pkg-config file (libiconv doesn't ship one)
    cat > "$PREFIX/lib/pkgconfig/iconv.pc" << PCEOF
prefix=$PREFIX
exec_prefix=\${prefix}
libdir=\${exec_prefix}/lib
includedir=\${prefix}/include

Name: iconv
Description: libiconv
Version: 1.17
Libs: -L\${libdir} -liconv
Cflags: -I\${includedir}
PCEOF
    echo "libiconv: OK"
}

# --- 3. glib (QEMU's main dependency) ---
build_glib() {
    echo "=== [3/5] Building glib ==="
    cd "$BUILD_DIR/src"
    GLIB_VER="2.82.0"
    GLIB_MAJ="${GLIB_VER%.*}"
    if [ ! -d "glib-$GLIB_VER" ]; then
        curl -sL -o glib.tar.xz "https://download.gnome.org/sources/glib/$GLIB_MAJ/glib-$GLIB_VER.tar.xz" && tar xJf glib.tar.xz && rm glib.tar.xz
    fi
    cd "glib-$GLIB_VER"

    # Create meson cross file for Android ARM64
    cat > "$BUILD_DIR/android-arm64-cross.txt" << CROSSEOF
[binaries]
c = '$CC'
cpp = '$CXX'
ar = '$AR'
strip = '$STRIP'
ranlib = '$RANLIB'
pkgconfig = 'pkg-config'

[properties]

[built-in options]
c_args = ['-I$PREFIX/include']
c_link_args = ['-L$PREFIX/lib']

[host_machine]
system = 'android'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'
CROSSEOF

    rm -rf build-android
    meson setup build-android \
        --cross-file="$BUILD_DIR/android-arm64-cross.txt" \
        --prefix="$PREFIX" \
        --default-library=static \
        -Dselinux=disabled \
        -Dxattr=false \
        -Dlibmount=disabled \
        -Dnls=disabled \
        -Dtests=false \
        -Dglib_debug=disabled \
        -Dintrospection=disabled \
        -Dgio_module_dir="$PREFIX/lib/gio/modules"
    ninja -C build-android -j$JOBS
    ninja -C build-android install
    echo "glib: OK"
}

# --- 4. pixman ---
build_pixman() {
    echo "=== [4/5] Building pixman ==="
    cd "$BUILD_DIR/src"
    if [ ! -d pixman-0.44.2 ]; then
        curl -sL -o pixman.tar.gz "https://cairographics.org/releases/pixman-0.44.2.tar.gz" && tar xzf pixman.tar.gz && rm pixman.tar.gz
    fi
    cd pixman-0.44.2
    rm -rf build-android
    meson setup build-android \
        --cross-file="$BUILD_DIR/android-arm64-cross.txt" \
        --prefix="$PREFIX" \
        --default-library=static \
        -Dgtk=disabled \
        -Dlibpng=disabled \
        -Dtests=disabled \
        -Ddemos=disabled
    ninja -C build-android -j$JOBS
    ninja -C build-android install
    echo "pixman: OK"
}

# --- 5. libslirp (user-mode networking) ---
build_libslirp() {
    echo "=== [5/5] Building libslirp ==="
    cd "$BUILD_DIR/src"
    if [ ! -d libslirp-4.8.0 ]; then
        curl -sL -o libslirp.tar.gz "https://gitlab.freedesktop.org/slirp/libslirp/-/archive/v4.8.0/libslirp-v4.8.0.tar.gz" && tar xzf libslirp.tar.gz && rm libslirp.tar.gz
        mv libslirp-v4.8.0 libslirp-4.8.0
    fi
    cd libslirp-4.8.0
    rm -rf build-android
    meson setup build-android \
        --cross-file="$BUILD_DIR/android-arm64-cross.txt" \
        --prefix="$PREFIX" \
        --default-library=static
    ninja -C build-android -j$JOBS
    ninja -C build-android install
    echo "libslirp: OK"
}

# --- 6. QEMU ---
build_qemu() {
    echo "=== Building QEMU ==="
    cd "$BUILD_DIR/src"
    QEMU_VER="10.2.1"
    if [ ! -d "qemu-$QEMU_VER" ]; then
        echo "Downloading QEMU $QEMU_VER..."
        curl -sL -o qemu.tar.xz "https://download.qemu.org/qemu-$QEMU_VER.tar.xz" && tar xJf qemu.tar.xz && rm qemu.tar.xz
    fi
    cd "qemu-$QEMU_VER"

    # QEMU uses its own configure (not autoconf)
    ./configure \
        --cc="$CC" \
        --cxx="$CXX" \
        --host-cc="cc" \
        --target-list=aarch64-softmmu \
        --prefix="$PREFIX/qemu" \
        --static \
        --disable-docs \
        --disable-gtk \
        --disable-sdl \
        --disable-opengl \
        --disable-virglrenderer \
        --disable-vte \
        --disable-curses \
        --disable-vnc \
        --disable-spice \
        --disable-smartcard \
        --disable-usb-redir \
        --disable-libusb \
        --disable-libudev \
        --disable-xen \
        --disable-kvm \
        --enable-tcg \
        --disable-tools \
        --disable-guest-agent \
        --disable-numa \
        --disable-capstone \
        --disable-brlapi \
        --disable-jack \
        --disable-alsa \
        --disable-pa \
        --disable-oss \
        --disable-coreaudio \
        --disable-dsound \
        --disable-debug-info \
        --enable-slirp \
        --extra-cflags="-I$PREFIX/include" \
        --extra-ldflags="-L$PREFIX/lib"

    make -j$JOBS
    echo ""
    echo "=== QEMU build complete ==="
    ls -la build/qemu-system-aarch64
    file build/qemu-system-aarch64
    echo ""
    echo "Binary: $BUILD_DIR/src/qemu-$QEMU_VER/build/qemu-system-aarch64"
}

# Build everything
build_zlib
build_libffi
build_libiconv
build_glib
build_pixman
build_libslirp
build_qemu

echo ""
echo "=== ALL DONE ==="
echo "QEMU binary: $BUILD_DIR/src/qemu-10.2.1/build/qemu-system-aarch64"
echo "Copy it to app/src/main/jniLibs/arm64-v8a/libqemu.so for bundling"
