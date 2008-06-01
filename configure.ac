#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_PRERE(2.61)
AC_INIT(scubed3, m4_esyscmd([./version.sh | tr -d '\n']))
AC_CONFIG_SRCDIR([src/scubed3.c])
AC_CONFIG_HEADER([config.h])
AM_INIT_AUTOMAKE

# Checks for programs.
AC_PROG_CC

# Checks for libraries.

# Checks for header files.
AC_HEADER_STDC
AC_CHECK_HEADERS([fcntl.h inttypes.h stdint.h stdlib.h string.h sys/ioctl.h sys/mount.h unistd.h])

# Checks for typedefs, structures, and compiler characteristics.
AC_C_CONST
AC_C_INLINE
AC_TYPE_OFF_T
AC_TYPE_SIZE_T
AC_TYPE_UINT16_T
AC_TYPE_UINT32_T
AC_TYPE_UINT64_T
AC_TYPE_UINT8_T
AC_C_VOLATILE

# Checks for library functions.
AC_FUNC_FSEEKO
AC_PROG_GCC_TRADITIONAL
AC_FUNC_MEMCMP
AC_FUNC_STAT
AC_CHECK_FUNCS([memset strchr strdup strerror strrchr])

AC_CONFIG_FILES([Makefile src/Makefile])
AC_OUTPUT
