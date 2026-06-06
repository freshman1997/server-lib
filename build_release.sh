#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build}"
JOBS="${JOBS:-8}"
YUAN_BUILD_SHARED_LIBS="${YUAN_BUILD_SHARED_LIBS:-OFF}"

"${ROOT_DIR}/build_openssl.sh"
cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release -DYUAN_BUILD_SHARED_LIBS="${YUAN_BUILD_SHARED_LIBS}"
cmake --build "${BUILD_DIR}" -j "${JOBS}"
