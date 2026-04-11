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
