#!/bin/bash
BUILD_PREFIX=${1:-$PWD}
cd $BUILD_PREFIX
protoc -I=. --cpp_out=. mlx90640_buffer.proto
protoc -I=. --python_out=. mlx90640_buffer.proto
PBSRC=mlx90640_buffer.pb.cc
PBOBJ=mlx90640_buffer.pb.o
OBJ=mlx90640_capture.o
SRC=mlx90640_capture.cpp
BIN=mlx90640_capture
MLXLIB=/usr/local/lib/libMLX90640_API.so
CXX_STANDARD=c++17

g++ -O2 -I. -std=$CXX_STANDARD -Wall -W -Werror -c -o $PBOBJ $PBSRC
g++ -O2 -I. -I/usr/local/include/MLX90640 -Wall -W -Werror -std=$CXX_STANDARD -c -o $OBJ $SRC
g++  $PBOBJ $OBJ $MLXLIB -pthread -lprotobuf  -o $BIN
