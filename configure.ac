AC_INIT([alexaagent], [201607])
AM_INIT_AUTOMAKE([foreign nostdinc dist-bzip2 no-dist-gzip])
AC_CONFIG_SRCDIR([main.cc])

PKG_CHECK_MODULES([OPENAL], [openal >= 1.1])
PKG_CHECK_MODULES([LIBAVCODEC], [libavcodec])
PKG_CHECK_MODULES([LIBAVFORMAT], [libavformat])
PKG_CHECK_MODULES([LIBAVUTIL], [libavutil])
PKG_CHECK_MODULES([OPENSSL], [openssl >= 1.0])
PKG_CHECK_MODULES([LIBNGHTTP2_ASIO], [libnghttp2_asio >= 1.10])

if test "${CXXFLAGS+set}" != set; then
	CXXFLAGS=
fi
AC_PROG_CXX

AC_C_INLINE
AC_TYPE_PID_T
AC_TYPE_SIZE_T

AC_ARG_ENABLE(
	[debug],
	AS_HELP_STRING([--enable-debug], [turn on debugging]),
	[case "${enableval}" in
	  yes) debug=true ;;
	  no) debug=false ;;
	  *) AC_MSG_ERROR([bad value ${enableval} for --enable-debug]) ;;
	 esac],
	[debug=false]
)
AM_CONDITIONAL([DEBUG], [test x$debug = xtrue])
AC_ARG_ENABLE(
	[profile],
	AS_HELP_STRING([--enable-profile], [turn on profiling]),
	[case "${enableval}" in
	  yes) profile=true ;;
	  no) profile=false ;;
	  *) AC_MSG_ERROR([bad value ${enableval} for --enable-profile]) ;;
	 esac],
	[profile=false]
)
AM_CONDITIONAL([PROFILE], [test x$profile = xtrue])

AM_CONFIG_HEADER([configure.h])
AC_CONFIG_FILES([
	Makefile
])
AC_OUTPUT
