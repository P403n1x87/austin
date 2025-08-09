#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_PREREQ([2.69])
# [[[cog
# from scripts.utils import get_current_version_from_changelog as version
# print(f"AC_INIT([austin], [{version()}], [https://github.com/p403n1x87/austin/issues])")
# ]]]
AC_INIT([austin], [4.0.0], [https://github.com/p403n1x87/austin/issues])
# [[[end]]]
AC_CONFIG_SRCDIR([config.h.in])
AC_CONFIG_HEADERS([config.h])
AM_INIT_AUTOMAKE
: ${CFLAGS=""}
# Checks for programs.
AC_PROG_CC_C99
AC_PROG_CPP

# Host conditionals
AC_CANONICAL_HOST
AC_CANONICAL_TARGET

# Use the C language and compiler for the following checks
AC_LANG([C])

# Checks for libraries.
AC_CHECK_HEADER(libunwind-ptrace.h, [
    AM_CONDITIONAL(BUILD_AUSTINP, true)
    AUSTINP_CFLAGS="-DAUSTINP -fPIC"
    AUSTINP_LDADD="-l:libunwind-ptrace.a -l:liblzma.a -l:libunwind-generic.a -l:libunwind.a"
    echo "including build of austinp"
], [
    AM_CONDITIONAL(BUILD_AUSTINP, false)
    echo "not building austinp: missing libunwind"
])
AC_CHECK_LIB(bfd, bfd_openr, [
    AC_DEFINE([HAVE_BFD], [1], ["Compile with BFD support"])
    AUSTINP_CFLAGS+=" -DHAVE_BFD"
    AUSTINP_LDADD+=" -l:libbfd.a -l:libz.a"
    AC_CHECK_LIB(sframe, sframe_encode, [
        AUSTINP_LDADD+=" -l:libsframe.a"
    ])
    AC_CHECK_LIB(zstd, ZSTD_compress, [
        AUSTINP_LDADD+=" -l:libzstd.a"
    ])
    echo "enabling symbol resolution support for austinp"
], [
    echo "austinp will be built without symbol resolution support: missing libbfd"
])
AC_CHECK_LIB(iberty, bfd_demangle, [
    AC_DEFINE([HAVE_LIBERTY], [1], ["Compile with C++ name demangling support"])
    AUSTINP_CFLAGS+=" -DHAVE_LIBERTY"
    AUSTINP_LDADD+=" -l:libiberty.a"
    echo "enabling C++ name demangling support for austinp"
], [
    echo "austinp will be built without C++ names demangling support: missing libiberty"
], [
    -lbfd
])
AUSTINP_LDADD+=" -ldl"

case $target_cpu in
    powerpc64le )
        AUSTINP_CFLAGS+=" -Wno-error=stringop-overflow"
        ;;
esac

AC_SUBST(AUSTINP_CFLAGS, [$AUSTINP_CFLAGS])
AC_SUBST(AUSTINP_LDADD, [$AUSTINP_LDADD])

# Checks for header files.
AC_HEADER_STDC
AC_CHECK_HEADERS([stddef.h stdlib.h string.h syslog.h unistd.h stdio.h])

# Checks for typedefs, structures, and compiler characteristics.
AC_TYPE_PID_T
AC_TYPE_SSIZE_T

# Checks for library functions.
AC_FUNC_FORK
AC_FUNC_MALLOC
AC_FUNC_REALLOC
AC_CHECK_FUNCS([strstr])

# Coverage
AC_ARG_ENABLE([coverage], [
  --enable-coverage    Turn on coverage], [
case "${enableval}" in
    yes) coverage=true ;;
    no)  coverage=false ;;
    *) AC_MSG_ERROR([bad value ${enableval} for --enable-coverage]) ;;
esac], [coverage=false])

AM_CONDITIONAL([COVERAGE], [test x$coverage = xtrue])

# Debug
AC_ARG_ENABLE([debug], [
  --enable-debug    Enable debug build. To include only debug symbols use "symbols"
], [
case "${enableval}" in
    symbols)
        debugsymbols=true
        debug=false
        ;;
    yes)
        debugsymbols=true
        debug=true
        AUSTINP_LDADD+=" -lm"
        ;;
    no)
        debugsymbols=false
        debug=false
        ;;
    *) AC_MSG_ERROR([bad value ${enableval} for --enable-debug]) ;;
esac], [
    debugsymbols=false
    debug=false
])

AM_CONDITIONAL([DEBUG_SYMBOLS], [test x$debugsymbols = xtrue])
AM_CONDITIONAL([DEBUG], [test x$debug = xtrue])

AC_CONFIG_FILES([Makefile
                 src/Makefile])
AC_OUTPUT
