#!/bin/bash
# Build austin/austinp inside a pypa manylinux container.
# Required env: ARCH (e.g. x86_64, aarch64).
# Writes the resolved wheel platform tag and version to .build-outputs in CWD.

set -euo pipefail

# Detect the toolset that the pypa image pre-activated in PATH (gcc or
# devtoolset; the exact version changes as images are updated).
GCC_BIN=$(which gcc 2>/dev/null || true)
if [[ "$GCC_BIN" == /opt/rh/*/root/usr/bin/gcc ]]; then
    TOOLSET=$(echo "$GCC_BIN" | sed 's|/opt/rh/\([^/]*\)/.*|\1|')
else
    # Fall back to the latest available
    TOOLSET=$(ls /opt/rh | grep -E '^(gcc-toolset|devtoolset)-[0-9]+$' | sort -V | tail -1)
fi
echo "Using toolset: ${TOOLSET}"

# Austin's source uses the post-2.34 binutils API (bfd_section_vma as a
# 1-arg function). System binutils on CentOS 7 (2.27) and AlmaLinux 8 (2.30)
# are too old; the toolset ships binutils 2.35+ with the modern API.
if command -v dnf >/dev/null 2>&1; then
    dnf install -y "${TOOLSET}-binutils-devel" xz-devel zlib-devel libzstd-devel
    dnf install -y zlib-static   || true
    dnf install -y libzstd-static || true
else
    yum install -y epel-release
    yum install -y "${TOOLSET}-binutils-devel" xz-devel zlib-devel libzstd-devel
    yum install -y zlib-static   || true
    yum install -y libzstd-static || true
fi

# The toolset linker may use a sysroot under /opt/rh/<toolset>/root, so
# -L/usr/lib64 resolves inside that sysroot rather than the real /usr/lib64.
# Copy the static archives there so the linker finds them unconditionally.
TOOLSET_LIB=/opt/rh/${TOOLSET}/root/usr/lib64
for lib in liblzma.a libz.a libzstd.a; do
    [ -f "/usr/lib64/${lib}" ] && cp "/usr/lib64/${lib}" "${TOOLSET_LIB}/${lib}" || true
done

# The enable script references MANPATH which may not be set; pre-initialise to
# avoid an unbound-variable error under set -u.
export MANPATH=${MANPATH:-}
# shellcheck disable=SC1090
source /opt/rh/${TOOLSET}/enable

# The toolset enable script prepends toolset library paths to LIBRARY_PATH but
# the toolset linker may not search system paths by default, causing it to miss
# liblzma.a (from xz-devel in /usr/lib64). Add them explicitly.
export LIBRARY_PATH="${LIBRARY_PATH:+${LIBRARY_PATH}:}/usr/lib64:/usr/lib"

# RHEL ships demangle.h without the libiberty/ subdir; austin's source uses
# the Debian path <libiberty/demangle.h>. Place the symlink inside the
# toolset's include tree so it resolves before the (older) /usr/include.
TOOLSET_INC=/opt/rh/${TOOLSET}/root/usr/include
mkdir -p "${TOOLSET_INC}/libiberty"
ln -sf "${TOOLSET_INC}/demangle.h" "${TOOLSET_INC}/libiberty/demangle.h"

git clone --depth=1 --branch "${LIBUNWIND_VERSION:-v1.8.3}" https://github.com/libunwind/libunwind.git /tmp/libunwind
pushd /tmp/libunwind
autoreconf -i
./configure --prefix=/opt/libunwind --disable-tests CFLAGS="-fPIC"
make -j"$(nproc)"
make install
popd
echo "/opt/libunwind/lib" > /etc/ld.so.conf.d/libunwind.conf
ldconfig

# xz-devel and libzstd-devel on RHEL-family only ship shared libraries; no
# -static package exists in the base or EPEL repos for all distro versions.
# Build static archives from source into the same prefix as libunwind so the
# linker finds them via the LIBRARY_PATH we export below.
curl -fL https://github.com/tukaani-project/xz/releases/download/v5.6.3/xz-5.6.3.tar.gz \
    | tar xz -C /tmp
pushd /tmp/xz-5.6.3
./configure --prefix=/opt/libunwind --disable-shared --enable-static --quiet
make -j"$(nproc)"
make install
popd

curl -fL https://github.com/facebook/zstd/releases/download/v1.5.6/zstd-1.5.6.tar.gz \
    | tar xz -C /tmp
make -C /tmp/zstd-1.5.6 -j"$(nproc)" install PREFIX=/opt/libunwind

GLIBC=$(ldd --version | awk '/^ldd/ {print $NF}')
GLIBC_MAJOR=${GLIBC%%.*}
GLIBC_MINOR=${GLIBC#*.}
PLATFORM="manylinux_${GLIBC_MAJOR}_${GLIBC_MINOR}_${ARCH}"
# Per PEP 600, compound with the legacy named alias when one exists
# so older pip (< 20.3) still picks up the wheel.
case "${GLIBC_MAJOR}_${GLIBC_MINOR}" in
    2_5)  PLATFORM="${PLATFORM}.manylinux1_${ARCH}"    ;;
    2_12) PLATFORM="${PLATFORM}.manylinux2010_${ARCH}" ;;
    2_17) PLATFORM="${PLATFORM}.manylinux2014_${ARCH}" ;;
esac
GLIBC_TAG="${GLIBC_MAJOR}_${GLIBC_MINOR}"

export CPATH=/opt/libunwind/include
export LIBRARY_PATH="/opt/libunwind/lib:${LIBRARY_PATH}"
export PKG_CONFIG_PATH=/opt/libunwind/lib/pkgconfig

autoreconf --install
./configure
make

VERSION=$(sed -r -n 's/^#define VERSION[ ]+"(.+)"/\1/p' src/austin.h)

pushd src
tar -Jcf "austin-${VERSION}-gnu-${GLIBC_TAG}-linux-${ARCH}.tar.xz" austin
tar -Jcf "austinp-${VERSION}-gnu-${GLIBC_TAG}-linux-${ARCH}.tar.xz" austinp
popd

{
    echo "platform=${PLATFORM}"
    echo "version=${VERSION}"
    echo "glibc_tag=${GLIBC_TAG}"
} > .build-outputs
