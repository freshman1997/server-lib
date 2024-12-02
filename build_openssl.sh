#!/bin/sh
if [ -e "./third_party/openssl-3.4.0/libssl.a" ] && [ -e "./third_party/openssl-3.4.0/libcrypto.a" ]; then
    echo "exist, skip"
else
    cd third_party/openssl-3.4.0 && git checkout origin/openssl-3.4
    branch=$(git branch | grep openssl-3.4)
    if [ -z "$branch" ]; then
        echo "error, openssl-3.4 has not been cloned!"
    else
        ./config -fPIC no-shared && make
    fi
fi
