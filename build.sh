#!/bin/sh

mkdir -p build
./build_openssl.sh && cd ./build && cmake .. && make
