#!/bin/sh

./build_openssl.sh
cd ./build && cmake .. && make
