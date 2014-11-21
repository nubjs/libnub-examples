#!/bin/bash

# Run:
#  git clone https://github.com/nubjs/libnub.git nub
#  cd nub
#  git submodule init
#  ./build.sh
#  cd out
#  make -j8
#  BUILDTYPE=Debug make -j8

# This project builds Debug by default, so to build release just run:
#  BUILDTYPE=Release ./build.sh

CC=$(which clang)

BUILDTYPE=${BUILDTYPE:-Debug}

NUB_DIR=nub
NUB_BUILD=${NUB_DIR}/out/${BUILDTYPE}
NUB_IDIR=${NUB_DIR}/include

UV_DIR=${NUB_DIR}/deps/uv
UV_BUILD=${NUB_BUILD}
UV_IDIR=${UV_DIR}/include

CFLAGS="-pthread -fno-omit-frame-pointer -Wall -g -fstrict-aliasing"

if [ ${BUILDTYPE} == Release ]
then
  CFLAGS+=" -O3"
  OUTFILE="tcp-echo"
else
  CFLAGS+=" -O0"
  OUTFILE="tcp-echo_g"
fi

DEPS=${NUB_BUILD}/libnub.a
DEPS+=\ ${UV_BUILD}/libuv.a

BUILDFILES=tcp-echo.c
BUILDFILES+=\ ${DEPS}

INCLUDES=-I${NUB_IDIR}\ -I${UV_IDIR}\ -I${NUB_DIR}/deps/fuq

${CC} ${CFLAGS} -o ${OUTFILE} ${BUILDFILES} ${INCLUDES}
