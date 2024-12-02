#!/bin/sh
if [ -e "./third_party/openssl-3.4.0/libssl.a" ] && [ -e "./third_party/openssl-3.4.0/libcrypto.a" ]; then
    echo "exist, skip"
else
    cd third_party/openssl-3.4.0 && ./config -fPIC no-shared && make
fi
