AC_INIT([jaim], [0.3-dev],
   [https://github.com/jpresley/jaim/issues],,
   [https://github.com/jpresley/jaim/])
AM_INIT_AUTOMAKE([foreign -Wall -Werror serial-tests])
AC_CONFIG_HEADERS([config.h])

: ${CXXLANG=-std=gnu++23}

AC_PROG_CXX

# Require C++23
CXXFLAGS="$CXXFLAGS $CXXLANG"
AC_LANG_PUSH([C++])
AC_MSG_CHECKING(for a working C++23 library)
AC_COMPILE_IFELSE(
  [AC_LANG_PROGRAM([#include <print>], [])],
  [AC_MSG_RESULT(yes)],
  [AC_MSG_ERROR([a C++23 compiler is required])])
AC_LANG_POP([C++])

AC_PATH_PROG([PATH_BASH], [bash], [/bin/bash])
AC_DEFINE_UNQUOTED([PATH_BASH], ["$PATH_BASH"], [Path to bash])

# Check for macOS sandbox.h (required for sandbox profile support)
AC_LANG_PUSH([C++])
AC_CHECK_HEADER([sandbox.h], [],
  [AC_MSG_ERROR([sandbox.h not found -- macOS sandbox support is required])])
AC_LANG_POP([C++])

TEST_ABS_TOP_BUILDDIR=`pwd -P`
TEST_ABS_TOP_SRCDIR=`cd "$srcdir" && pwd -P`
AC_SUBST([TEST_ABS_TOP_BUILDDIR])
AC_SUBST([TEST_ABS_TOP_SRCDIR])

AC_CONFIG_FILES([Makefile tests/Makefile tests/common.sh tests/setup-setuid.sh])
AC_CONFIG_COMMANDS([test-scripts], [chmod +x tests/common.sh tests/setup-setuid.sh])
AC_OUTPUT
