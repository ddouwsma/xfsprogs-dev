AC_INIT([xfsprogs],[6.15.0],[linux-xfs@vger.kernel.org])
AC_PREREQ([2.69])
AC_CONFIG_AUX_DIR([.])
AC_CONFIG_MACRO_DIR([m4])
AC_CONFIG_SRCDIR([include/libxfs.h])
AC_PREFIX_DEFAULT(/usr)
AC_SUBST(PACKAGE_BUGREPORT)

# Default CFLAGS if nobody specifies anything else
if test "${CFLAGS+set}" != "set"; then
	CFLAGS="-g -O2 -std=gnu11"
fi
if test "${CXXFLAGS+set}" != "set"; then
	CXXFLAGS="-g -O2 -std=gnu++11"
fi

AC_PROG_INSTALL
LT_INIT

AC_PROG_CC
AC_ARG_VAR(BUILD_CC, [C compiler for build tools])
if test "${BUILD_CC+set}" != "set"; then
  if test $cross_compiling = no; then
    BUILD_CC="$CC"
  else
    AC_CHECK_PROGS(BUILD_CC, gcc cc)
  fi
fi
AC_ARG_VAR(BUILD_CFLAGS, [C compiler flags for build tools])
if test "${BUILD_CFLAGS+set}" != "set"; then
  if test $cross_compiling = no; then
    BUILD_CFLAGS="$CFLAGS"
  else
    BUILD_CFLAGS="-g -O2 -std=gnu11"
  fi
fi

AC_PROG_CXX
# no C++ build tools yet

AC_ARG_ENABLE(shared,
[  --enable-shared=[yes/no]  Enable use of shared libraries [default=yes]],,
	enable_shared=yes)
AC_SUBST(enable_shared)

AC_ARG_ENABLE(gettext,
[  --enable-gettext=[yes/no] Enable alternate language support [default=yes]],,
	enable_gettext=yes)
AC_SUBST(enable_gettext)

AC_ARG_ENABLE(editline,
[  --enable-editline=[yes/no] Enable editline command editing [default=no]],
	test $enable_editline = yes && libeditline="-ledit",
	enable_editline=no)
AC_SUBST(libeditline)
AC_SUBST(enable_editline)

AC_ARG_ENABLE(termcap,
[  --enable-termcap=[yes/no] Enable terminal capabilities library [default=no]],
	test $enable_termcap = yes && libtermcap="-ltermcap",)
AC_SUBST(libtermcap)


AC_ARG_ENABLE(lib64,
[  --enable-lib64=[yes/no]   Enable lib64 support [default=yes]],,
	enable_lib64=yes)
AC_SUBST(enable_lib64)

AC_ARG_WITH([multiarch],
[  --with-multiarch=ARCH   Specify the multiarch triplet],
	multiarch=$withval)
AC_SUBST(multiarch)

AC_ARG_ENABLE(librt,
[  --enable-librt=[yes/no]   Enable librt support [default=yes]],,
	enable_librt=yes)
AC_SUBST(enable_librt)

# Enable UBSAN; set enable_ubsan=probe below to enable autoprobe.
AC_ARG_ENABLE(ubsan,
[  --enable-ubsan=[yes/no]   Enable Undefined Behavior Sanitizer (UBSAN) [default=no]],,
	enable_ubsan=no)
AC_SUBST(enable_ubsan)

# Enable ADDRSAN; set enable_addrsan=probe below to enable autoprobe.
AC_ARG_ENABLE(addrsan,
[  --enable-addrsan=[yes/no] Enable Address Sanitizer (ADDRSAN) [default=no]],,
	enable_addrsan=no)
AC_SUBST(enable_addrsan)

# Enable THREADSAN; set enable_threadsan=probe to enable autoprobe.
AC_ARG_ENABLE(threadsan,
[  --enable-threadsan=[yes/no] Enable Thread Sanitizer (THREADSAN) [default=no]],,
	enable_threadsan=no)
AC_SUBST(enable_threadsan)

AC_ARG_ENABLE(lto,
[  --enable-lto=[yes/no]     Enable link time optimization (LTO) [default=no]],,
	enable_lto=no)
AC_SUBST(enable_lto)

# Enable xfs_scrub build
AC_ARG_ENABLE(scrub,
[  --enable-scrub=[yes/no]   Enable build of xfs_scrub utility [default=yes]],,
	enable_scrub=yes)
AC_SUBST(enable_scrub)

# Enable libicu for xfs_scrubbing of malicious unicode sequences in names
AC_ARG_ENABLE(libicu,
[  --enable-libicu=[yes/no]  Enable Unicode name scanning in xfs_scrub (libicu) [default=probe]],,
	enable_libicu=probe)

#
# If the user specified a libdir ending in lib64 do not append another
# 64 to the library names.
#
base_libdir=`basename "$libdir"`
case $base_libdir in
lib64)
  enable_lib64=no
esac

#
# If the user specified a multiarch path then disable lib64
#
test -n "$multiarch" && enable_lib64=no

# Find localized files.  Don't descend into any "dot directories"
# (like .git or .pc from quilt).  Strangely, the "-print" argument
# to "find" is required, to avoid including such directories in the
# list.
LOCALIZED_FILES=""
for lfile in `find ${srcdir} -path './.??*' -prune -o -name '*.c' -print -o -name '*.py.in' -print || exit 1`; do
    LOCALIZED_FILES="$LOCALIZED_FILES \$(TOPDIR)/$lfile"
done
AC_SUBST(LOCALIZED_FILES)

AC_PACKAGE_GLOBALS(xfsprogs)
AC_PACKAGE_UTILITIES(xfsprogs)
AC_MULTILIB($enable_lib64)
AC_MULTIARCH($multiarch)
AC_RT($enable_librt)

AC_PACKAGE_NEED_INI_H
AC_PACKAGE_NEED_LIBINIH

AC_PACKAGE_NEED_UUID_H
AC_PACKAGE_NEED_UUIDCOMPARE

AC_PACKAGE_NEED_PTHREAD_H
AC_PACKAGE_NEED_PTHREADMUTEXINIT

AC_PACKAGE_NEED_URCU_H
AC_PACKAGE_NEED_RCU_INIT

AC_HAVE_PWRITEV2
AC_HAVE_COPY_FILE_RANGE
AC_HAVE_CACHESTAT
AC_NEED_INTERNAL_FSXATTR
AC_NEED_INTERNAL_FSCRYPT_ADD_KEY_ARG
AC_NEED_INTERNAL_FSCRYPT_POLICY_V2
AC_NEED_INTERNAL_STATX
AC_HAVE_GETFSMAP
AC_HAVE_MAP_SYNC
AC_HAVE_DEVMAPPER
AC_HAVE_MALLINFO
AC_HAVE_MALLINFO2
AC_HAVE_MEMFD_CREATE
AC_HAVE_GETRANDOM_NONBLOCK
if test "$enable_scrub" = "yes"; then
        if test "$enable_libicu" = "yes" || test "$enable_libicu" = "probe"; then
                AC_HAVE_LIBICU
        fi
        if test "$enable_libicu" = "yes" && test "$have_libicu" != "yes"; then
                AC_MSG_ERROR([libicu not found.])
        fi
fi
AC_CONFIG_SYSTEMD_SYSTEM_UNIT_DIR
AC_CONFIG_CROND_DIR
AC_CONFIG_UDEV_RULE_DIR
AC_HAVE_BLKID_TOPO
AC_HAVE_TRIVIAL_AUTO_VAR_INIT

if test "$enable_ubsan" = "yes" || test "$enable_ubsan" = "probe"; then
        AC_PACKAGE_CHECK_UBSAN
fi
if test "$enable_ubsan" = "yes" && test "$have_ubsan" != "yes"; then
        AC_MSG_ERROR([UBSAN not supported by compiler.])
fi

if test "$enable_addrsan" = "yes" || test "$enable_addrsan" = "probe"; then
        AC_PACKAGE_CHECK_ADDRSAN
fi
if test "$enable_addrsan" = "yes" && test "$have_addrsan" != "yes"; then
        AC_MSG_ERROR([ADDRSAN not supported by compiler.])
fi

if test "$enable_threadsan" = "yes" || test "$enable_threadsan" = "probe"; then
        AC_PACKAGE_CHECK_THREADSAN
fi
if test "$enable_threadsan" = "yes" && test "$have_threadsan" != "yes"; then
        AC_MSG_ERROR([THREADSAN not supported by compiler.])
fi

if test "$have_threadsan" = "yes" && test "$have_addrsan" = "yes"; then
        AC_MSG_WARN([ADDRSAN and THREADSAN are not known to work together.])
fi

if test "$enable_lto" = "yes" || test "$enable_lto" = "probe"; then
	AC_PACKAGE_CHECK_LTO
fi
if test "$enable_lto" = "yes" && test "$have_lto" != "yes"; then
	AC_MSG_ERROR([LTO not supported by compiler.])
fi

AC_MANUAL_FORMAT
AC_HAVE_LIBURCU_ATOMIC64

AC_CONFIG_FILES([include/builddefs])
AC_OUTPUT
