#!/bin/bash -eu

set -e
set -u

# Install build dependencies
apt-get update
apt-get -y install \
    autoconf \
    build-essential \
    libtool \
    binutils-dev \
    libiberty-dev \
    liblzma-dev \
    musl-tools \
    zlib1g-dev \
    libzstd-dev \
    git

# Compile and install libunwind from sources (use cache if available)
LIBUNWIND_CACHE=/libunwind-cache

if [ -f "$LIBUNWIND_CACHE/lib/libunwind.a" ]; then
    echo "Restoring libunwind from cache"
else
    git clone --depth=1 --branch $LIBUNWIND_VERSION https://github.com/libunwind/libunwind.git
    cd libunwind
    autoreconf -i
    ./configure --prefix=$LIBUNWIND_CACHE --disable-tests CFLAGS="-fPIC"
    make -j$(nproc)
    make install
    cd -
fi

cp -r $LIBUNWIND_CACHE/include/* /usr/local/include/
cp -r $LIBUNWIND_CACHE/lib/*     /usr/local/lib/
ldconfig

# Build Austin
autoreconf --install
./configure
make

export VERSION=$(cat src/austin.h | sed -r -n "s/^#define VERSION[ ]+\"(.+)\"/\1/p")

# The wheel/manylinux tag must reflect the glibc this binary actually links
# against -- detect it inside this (possibly QEMU-emulated) environment
# rather than hardcoding a value in the caller, which drifts silently as the
# base image's glibc changes across Ubuntu releases (see issue #347, where
# the same class of bug shipped an x86_64 wheel claiming manylinux_2_12 while
# actually requiring GLIBC_2.38). Mirrors release.yml's
# release-linux-glibc-modern job.
case "$ARCH" in
    armv7) WHEEL_ARCH=armv7l ;;
    *) WHEEL_ARCH=$ARCH ;;
esac
GLIBC=$(ldd --version | awk '/^ldd/ {print $NF}')
GLIBC_MAJOR=${GLIBC%%.*}
GLIBC_MINOR=${GLIBC#*.}
PLATFORM="manylinux_${GLIBC_MAJOR}_${GLIBC_MINOR}_${WHEEL_ARCH}"
# Per PEP 600, compound with the legacy named alias when one exists so older
# pip (< 20.3) still picks up the wheel.
case "${GLIBC_MAJOR}_${GLIBC_MINOR}" in
    2_5) PLATFORM="${PLATFORM}.manylinux1_${WHEEL_ARCH}" ;;
    2_12) PLATFORM="${PLATFORM}.manylinux2010_${WHEEL_ARCH}" ;;
    2_17) PLATFORM="${PLATFORM}.manylinux2014_${WHEEL_ARCH}" ;;
esac
echo "platform=${PLATFORM}" > /artifacts/.build-outputs
echo "glibc=${GLIBC_MAJOR}.${GLIBC_MINOR}" >> /artifacts/.build-outputs

pushd src
    tar -Jcf austin-$VERSION-gnu-linux-$ARCH.tar.xz austin
    tar -Jcf austinp-$VERSION-gnu-linux-$ARCH.tar.xz austinp

    cp austin  /artifacts/austin
    cp austinp /artifacts/austinp

    musl-gcc -O3 -Os -s -Wall -pthread *.c -o austin -D__MUSL__
    tar -Jcf austin-$VERSION-musl-linux-$ARCH.tar.xz austin

    cp austin /artifacts/austin.musl

    mv austin-$VERSION-gnu-linux-$ARCH.tar.xz /artifacts
    mv austinp-$VERSION-gnu-linux-$ARCH.tar.xz /artifacts
    mv austin-$VERSION-musl-linux-$ARCH.tar.xz /artifacts
popd
