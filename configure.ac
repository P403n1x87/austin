#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_PREREQ([2.69])
AC_INIT([austin], [3.3.0], [https://github.com/p403n1x87/austin/issues])
AC_CONFIG_SRCDIR([config.h.in])
AC_CONFIG_HEADERS([config.h])
AM_INIT_AUTOMAKE
: ${CFLAGS=""}
# Checks for programs.
AC_PROG_CC_C99
AC_PROG_CPP

# Use the C language and compiler for the following checks
AC_LANG([C])

# Checks for libraries.
AC_CHECK_HEADER(libunwind-ptrace.h, [
    AM_CONDITIONAL(BUILD_AUSTINP, true)
    AUSTINP_CFLAGS="-DAUSTINP"
    AUSTINP_LDADD="-l:libunwind-ptrace.a -l:liblzma.a -l:libunwind-generic.a -l:libunwind.a"
    echo "including build of austinp"
], [
    AM_CONDITIONAL(BUILD_AUSTINP, false)
    echo "not building austinp: missing libunwind"
])
AC_CHECK_LIB(bfd, bfd_openr, [
    AC_DEFINE([HAVE_BFD], [1], ["Compile with BFD support"])
    AUSTINP_CFLAGS+=" -DHAVE_BFD"
    AUSTINP_LDADD+=" -lbfd"
    echo "enabling symbol resolution support for austinp"
], [
    echo "austinp will be built without symbol resolution support: missing libbfd"
])
AC_CHECK_LIB(iberty, bfd_demangle, [
    AC_DEFINE([HAVE_LIBERTY], [1], ["Compile with C++ name demangling support"])
    AUSTINP_CFLAGS+=" -DHAVE_LIBERTY"
    echo "enabling C++ name demangling support for austinp"
], [
    echo "austinp will be built without C++ names demangling support: missing libiberty"
], [
    -lbfd
])

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

# Debug symbols
AC_ARG_ENABLE([debug-symbols], [
  --enable-debug-symbols    Include debug symbols], [
case "${enableval}" in
    yes) debugsymbols=true ;;
    no)  debugsymbols=false ;;
    *) AC_MSG_ERROR([bad value ${enableval} for --enable-debug-symbols]) ;;
esac], [debugsymbols=false])

AM_CONDITIONAL([DEBUG_SYMBOLS], [test x$debugsymbols = xtrue])

AC_CONFIG_FILES([Makefile
                 src/Makefile])
AC_OUTPUT
