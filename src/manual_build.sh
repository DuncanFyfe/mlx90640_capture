#!/bin/bash
BUILD_PREFIX=${1:-$PWD}
cd $BUILD_PREFIX
protoc -I=. --cpp_out=. mlx90640_buffer.proto
protoc -I=. --python_out=. mlx90640_buffer.proto
PBSRC=mlx90640_buffer.pb.cc
PBOBJ=mlx90640_buffer.pb.o
OBJ=mlx90640_reader.o
SRC=mlx90640_reader.cpp
BIN=mlx90640_reader
MLXLIB=/usr/local/lib/libMLX90640_API.a

g++ -O2 -I. -std=c++11 -std=c++11 -c -o $PBOBJ $PBSRC
g++ -O2 -I. -I/usr/local/include/MLX90640 -std=c++11 -std=c++11 -c -o $OBJ $SRC
g++  $PBOBJ $OBJ $MLXLIB -pthread -lprotobuf  -o $BIN 
