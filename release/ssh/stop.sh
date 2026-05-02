#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PID_FILE="${PID_FILE:-${SCRIPT_DIR}/release_ssh_server.pid}"

if [[ ! -f "${PID_FILE}" ]]; then
  echo "release_ssh_server pid file not found: ${PID_FILE}"
  exit 0
fi

pid="$(cat "${PID_FILE}")"
if [[ -z "${pid}" ]]; then
  rm -f "${PID_FILE}"
  exit 0
fi

if kill -0 "${pid}" 2>/dev/null; then
  kill "${pid}"
  for _ in {1..50}; do
    if ! kill -0 "${pid}" 2>/dev/null; then
      break
    fi
    sleep 0.1
  done
  if kill -0 "${pid}" 2>/dev/null; then
    kill -KILL "${pid}" 2>/dev/null || true
  fi
fi

rm -f "${PID_FILE}"
echo "release_ssh_server stopped: ${pid}"
帮我在release文件夹下设计、增加一个工具，用于同步文件到远程服务器，2边的文件保持一致。开始分步骤实现：
1、需要一个服务器和一个客户端
2、文件变化了之后触发同步
3、不能用rsync或者其他现成的工具，需要自己实现
4、我目前的需求是客户端跑在Windows，而服务器跑在Linux
5、根据配置文件来指定需要同步的文件路径