#!/bin/sh

cd ./third_party/openssl-3.4.0 && ./build.sh
cd ../../build && cmake .. && make
