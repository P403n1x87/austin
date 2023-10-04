#!/bin/bash -eu

set -e
set -u

# Install build dependencies
apt-get update
apt-get -y install \
    autoconf \
    build-essential \
    libunwind-dev \
    binutils-dev \
    libiberty-dev \
    musl-tools \
    zlib1g-dev

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
