#!/bin/sh
if [ -e "libssl.a" ] && [ -e "libcrypto.a" ]; then
    echo "exist, skip"
else
    ./config -fPIC no-shared && make
fi
