#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_PREREQ([2.64])
dnl the version is not quoted: version.sh runs first, AC_INIT gets a literal
AC_INIT([scubed3], m4_esyscmd_s([./version.sh]))
AC_CONFIG_SRCDIR([src/scubed3.c])
AC_CONFIG_HEADERS([config.h])
AM_INIT_AUTOMAKE

# Checks for programs.
AC_PROG_CC

# Checks for libraries.
PKG_CHECK_MODULES([FUSE], [fuse3])

# readline (line editing in scubed3ctl); --without-readline for the
# debian-installer, it has no readline
AC_ARG_WITH([readline],
	[AS_HELP_STRING([--without-readline],
		[build scubed3ctl without line editing])],
	[], [with_readline=yes])
AS_IF([test "x$with_readline" != xno],
	[AC_CHECK_LIB([readline], [readline], [READLINE_LIBS=-lreadline],
		[AC_MSG_ERROR([readline not found, use --without-readline])])],
	[AC_DEFINE([NO_READLINE], [1], [build scubed3ctl without readline])])
AC_SUBST([READLINE_LIBS])

# Checks for header files.
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
