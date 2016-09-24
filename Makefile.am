AM_CPPFLAGS = $(OPENAL_CFLAGS) $(LIBAVCODEC_CFLAGS) $(LIBAVFORMAT_CFLAGS) $(LIBAVUTIL_CFLAGS) $(OPENSSL_CFLAGS) $(LIBNGHTTP2_ASIO_CFLAGS) -I.
AM_CXXFLAGS = -std=c++1y
AM_LDFLAGS =
if DEBUG
AM_CXXFLAGS += -g
else
AM_CPPFLAGS += -DNDEBUG
AM_CXXFLAGS += -O3
endif
if PROFILE
AM_CXXFLAGS += -pg
AM_LDFLAGS += -pg
endif
bin_PROGRAMS = alexaagent play_file play_url tiny_http
alexaagent_LDADD = $(OPENAL_LIBS) $(LIBAVCODEC_LIBS) $(LIBAVFORMAT_LIBS) $(LIBAVUTIL_LIBS) $(OPENSSL_LIBS) $(LIBNGHTTP2_ASIO_LIBS) -lboost_system -lboost_coroutine -lboost_regex -lpthread
alexaagent_SOURCES = \
	json.h \
	multipart.h \
	audio.h \
	scheduler.h \
	session.h \
	tiny_http.h \
	main.cc
play_file_LDADD = $(OPENAL_LIBS) $(LIBAVCODEC_LIBS) $(LIBAVFORMAT_LIBS) $(LIBAVUTIL_LIBS)
play_file_SOURCES = \
	audio.h \
	play_file.cc
play_url_LDADD = $(OPENAL_LIBS) $(LIBAVCODEC_LIBS) $(LIBAVFORMAT_LIBS) $(LIBAVUTIL_LIBS) $(OPENSSL_LIBS) -lboost_system -lpthread
play_url_SOURCES = \
	audio.h \
	play_url.cc
tiny_http_LDADD = $(OPENSSL_LIBS) -lboost_system -lpthread
tiny_http_SOURCES = \
	tiny_http.h \
	tiny_http.cc
check_PROGRAMS = test_multipart test_tiny_http
TESTS = test_multipart test_tiny_http
test_multipart_SOURCES = \
	multipart.h \
	test_multipart.cc
test_tiny_http_LDADD = $(OPENSSL_LIBS) -lboost_system
test_tiny_http_SOURCES = \
	tiny_http.h \
	test_tiny_http.cc
