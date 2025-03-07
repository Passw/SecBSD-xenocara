dnl  Copyright 2005 Red Hat, Inc.
dnl
dnl  Permission to use, copy, modify, distribute, and sell this software and its
dnl  documentation for any purpose is hereby granted without fee, provided that
dnl  the above copyright notice appear in all copies and that both that
dnl  copyright notice and this permission notice appear in supporting
dnl  documentation, and that the name of Red Hat not be used in
dnl  advertising or publicity pertaining to distribution of the software without
dnl  specific, written prior permission.  Red Hat makes no
dnl  representations about the suitability of this software for any purpose.  It
dnl  is provided "as is" without express or implied warranty.
dnl
dnl  RED HAT DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
dnl  INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
dnl  EVENT SHALL RED HAT BE LIABLE FOR ANY SPECIAL, INDIRECT OR
dnl  CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
dnl  DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
dnl  TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
dnl  PERFORMANCE OF THIS SOFTWARE.
dnl
dnl Process this file with autoconf to create configure.

# Initialize Autoconf
AC_PREREQ([2.60])
AC_INIT([x11perf],
	[1.7.0],
	[https://gitlab.freedesktop.org/xorg/test/x11perf/-/issues],
	[x11perf])
AC_CONFIG_SRCDIR([Makefile.am])
AC_CONFIG_HEADERS([config.h])

# Initialize Automake
AM_INIT_AUTOMAKE([foreign dist-xz])

# Require X.Org macros 1.8 or later for MAN_SUBSTS set by XORG_MANPAGE_SECTIONS
m4_ifndef([XORG_MACROS_VERSION],
	  [m4_fatal([must install xorg-macros 1.8 or later before running autoconf/autogen])])
XORG_MACROS_VERSION(1.8)
XORG_DEFAULT_OPTIONS

# Define the installation directory for the x11 performance compare scripts
AC_SUBST([x11perfcompdir], [$datadir/X11/x11perfcomp])

# Checks for programs run by the scripts we install
AC_PATH_PROG(MKTEMP, mktemp)

# Checks for pkg-config packages
PKG_CHECK_MODULES(X11PERF, [x11 xmuu xproto >= 7.0.25])

# Check for Xrender library
PKG_CHECK_MODULES(XRENDER, xrender, [xrender_found=yes], [xrender_found=no])
case "$xrender_found" in
yes)
	AC_DEFINE(XRENDER,1,[Xrender library available])
	;;
esac

# Check for Xft library
PKG_CHECK_MODULES(XFT, xft fontconfig, [xft_found=yes], [xft_found=no])
case "$xft_found" in
yes)
	AC_DEFINE(XFT,1,[Xft library available])
	;;
esac

# Check for MIT-SHM extension
PKG_CHECK_MODULES(XEXT, xext, [xext_found=yes], [xext_found=no])
case "$xext_found" in
yes)
	save_LIBS="$LIBS"
	LIBS="$XEXT_LIBS $LIBS"
	AC_CHECK_FUNCS([XShmQueryExtension],
		       AC_DEFINE(MITSHM, 1, [MIT-SHM extension available]))
	LIBS="$save_LIBS"
	;;
esac

AC_CHECK_FUNCS([srand_deterministic])

# Substitute config variables in man pages
AC_CONFIG_FILES([man/x11perf.1:man/x11perf.man])
AC_CONFIG_FILES([man/x11perfcomp.1:man/x11perfcomp.man])
AC_CONFIG_FILES([man/Xmark.1:man/Xmark.man])

AC_CONFIG_FILES([
	Makefile
	man/Makefile])
AC_OUTPUT
