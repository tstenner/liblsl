#!/bin/sh

# This script builds the liblsl.so for linux machines without bigger quirks
# and no recent CMake version, i.e. ARM boards and PCs with old distributions.
# For development, install a recent CMake version, either via pip
# (pip install cmake) or as binary download from cmake.org

set -e -x
# Try to read LSLGITREVISION from git if the variable isn't set
echo ${LSLGITREVISION:="$(git describe --tags HEAD)"}
${CXX:-g++} -fPIC -fvisibility=hidden -O2 ${CFLAGS} -Iexternal \
	-DBOOST_ALL_NO_LIB \
	-DLSL_LIBRARY_INFO_STR=\"${LSLGITREVISION:-"built from standalone build script"}\" \
	src/*.cpp src/pugixml/pugixml.cpp \
	external/src/chrono/src/chrono.cpp \
	external/src/filesystem/src/codecvt_error_category.cpp \
	external/src/filesystem/src/operations.cpp \
	external/src/filesystem/src/path.cpp \
	external/src/filesystem/src/path_traits.cpp \
	external/src/filesystem/src/portability.cpp \
	external/src/filesystem/src/unique_path.cpp \
	external/src/filesystem/src/utf8_codecvt_facet.cpp \
	external/src/serialization/src/*.cpp \
	external/src/system/src/error_code.cpp \
	external/src/thread/src/pthread/once.cpp \
	external/src/thread/src/pthread/thread.cpp \
	external/src/thread/src/tss_null.cpp \
	${LDFLAGS} \
	-Wno-deprecated-declarations \
	-shared -o liblsl.so -lpthread -lrt -ldl
${CC:-gcc} -O2 ${CFLAGS} -Iinclude testing/lslver.c -o lslver -L. -llsl
LD_LIBRARY_PATH=. ./lslver
