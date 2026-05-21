#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OPENSSL_DIR="${ROOT_DIR}/third_party/openssl-3.4.0"
LIBSSL_A="${OPENSSL_DIR}/libssl.a"
LIBCRYPTO_A="${OPENSSL_DIR}/libcrypto.a"
JOBS="${JOBS:-8}"

if [[ -f "${LIBSSL_A}" && -f "${LIBCRYPTO_A}" ]]; then
    echo "openssl static libraries already exist, skip"
    exit 0
fi

if [[ ! -d "${OPENSSL_DIR}" ]]; then
    echo "error: openssl directory not found: ${OPENSSL_DIR}" >&2
    exit 1
fi

cmake -E chdir "${OPENSSL_DIR}" ./config -fPIC no-shared
cmake -E chdir "${OPENSSL_DIR}" make -j "${JOBS}"
