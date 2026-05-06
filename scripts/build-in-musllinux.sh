#!/bin/sh
# Build austin inside a pypa musllinux (Alpine) container.
# Required env: ARCH (e.g. x86_64, aarch64).
# Writes the resolved wheel platform tag and version to .build-outputs in CWD.
# POSIX-sh compatible: Alpine's default shell is busybox ash, no bashisms.

set -eu

apk add --no-cache build-base xz-dev zlib-dev zstd-dev git tar

MUSL=$(ldd 2>&1 | awk '/Version/ {print $NF}')
MUSL_MAJOR=${MUSL%%.*}
MUSL_MINOR=$(echo "$MUSL" | cut -d. -f2)
PLATFORM="musllinux_${MUSL_MAJOR}_${MUSL_MINOR}_${ARCH}"
MUSL_TAG="${MUSL_MAJOR}_${MUSL_MINOR}"

gcc -O3 -Os -s -Wall -pthread src/*.c -o src/austin -D__MUSL__

VERSION=$(sed -r -n 's/^#define VERSION[ ]+"(.+)"/\1/p' src/austin.h)

cd src
tar -Jcf "austin-${VERSION}-musl-${MUSL_TAG}-linux-${ARCH}.tar.xz" austin
cd ..

{
    echo "platform=${PLATFORM}"
    echo "version=${VERSION}"
    echo "musl_tag=${MUSL_TAG}"
} > .build-outputs
