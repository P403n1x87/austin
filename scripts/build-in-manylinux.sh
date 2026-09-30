#!/bin/bash
# Build austin/austinp inside a pypa manylinux container.
# Required env: ARCH (e.g. x86_64, aarch64).
# Writes the resolved wheel platform tag and version to .build-outputs in CWD.
#
# manylinux images are RHEL-family (dnf/yum) for every architecture except
# armv7l, which -- per https://github.com/pypa/manylinux -- only has an
# Ubuntu-based image (the glibc 2.31 baseline predates any armv7l RHEL-based
# build). Branch on the package manager rather than the architecture so this
# keeps working if that ever changes.

set -euo pipefail

if command -v apt-get >/dev/null 2>&1; then
    # Debian/Ubuntu-based image (currently: armv7l only). No devtoolset
    # concept, no demangle.h/lib64-sysroot quirks -- system gcc and the -dev
    # packages already resolve correctly, matching what scripts/build_arch.sh
    # already relied on for the plain ubuntu22.04 QEMU path.
    apt-get update -q -y
    apt-get -y install --no-install-recommends \
        autoconf automake build-essential libtool \
        binutils-dev libiberty-dev liblzma-dev \
        zlib1g-dev libzstd-dev git
else
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
fi

# /opt/libunwind may already be populated by a caller-mounted cache volume
# (see build_release_arch.yml, which caches it across QEMU-emulated runs --
# these are slow enough that rebuilding libunwind/xz/zstd from scratch every
# time is wasteful). Skip straight to using it if so.
if [ -f /opt/libunwind/lib/libunwind.a ]; then
    echo "Using cached libunwind/xz/zstd from /opt/libunwind"
else
    git clone --depth=1 --branch "${LIBUNWIND_VERSION:-v1.8.3}" https://github.com/libunwind/libunwind.git /tmp/libunwind
    pushd /tmp/libunwind
    autoreconf -i
    # --disable-shared: on ppc64le, the freshly autoreconf'd libtool mis-handles
    # the -WCClinker flag when linking the *shared* libunwind.la ("gcc: error:
    # unrecognized command-line option '-WCClinker'"), reproduced identically on
    # both CentOS7 (manylinux2014) and AlmaLinux8 (manylinux_2_28) base images --
    # a libtool/ppc64 bug, not an image quirk. Austin only ever links libunwind
    # statically, so skip the shared build everywhere.
    ./configure --prefix=/opt/libunwind --disable-tests --disable-shared CFLAGS="-fPIC"
    make -j"$(nproc)"
    make install
    popd

    if ! command -v apt-get >/dev/null 2>&1; then
        # xz-devel and libzstd-devel on RHEL-family only ship shared libraries; no
        # -static package exists in the base or EPEL repos for all distro versions.
        # Build static archives from source into the same prefix as libunwind so
        # the linker finds them via the LIBRARY_PATH we export below. Debian/
        # Ubuntu's liblzma-dev/libzstd-dev already ship .a files, so this is
        # RHEL-only.
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
    fi
fi
echo "/opt/libunwind/lib" > /etc/ld.so.conf.d/libunwind.conf
ldconfig 2>/dev/null || true

export CPATH=/opt/libunwind/include
export LIBRARY_PATH="/opt/libunwind/lib:${LIBRARY_PATH:-}"
if ! command -v apt-get >/dev/null 2>&1; then
    export PKG_CONFIG_PATH=/opt/libunwind/lib/pkgconfig
fi

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
