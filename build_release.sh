#!/bin/sh

mkdir -p build
./build_openssl.sh && cd ./build && cmake .. -DCMAKE_BUILD_TYPE=Release && make -j 8
