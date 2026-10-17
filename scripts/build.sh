#!/usr/bin/env bash
# 标准化构建脚本：同步源码到远程 + 构建
# 用法: ./scripts/build.sh [target] [REMOTE_HOST=ai]
set -euo pipefail

REMOTE_HOST="${REMOTE_HOST:-ai}"
REMOTE_DIR="~/ninfer-deploy"
TARGET="${1:-ninfer-serve}"
NINJA="/home/ET/zhanghao/anaconda3/envs/nus/bin/ninja"

echo "=== 同步源码到 ${REMOTE_HOST}:${REMOTE_DIR} ==="
rsync -a --exclude='build*/' --exclude='.git/' --exclude='.claude/' \
  src-tree/fusion-engine-src/ "${REMOTE_HOST}:${REMOTE_DIR}/fusion-engine-src/"
echo "同步完成"

echo "=== 构建 ${TARGET} ==="
ssh "${REMOTE_HOST}" "cd ${REMOTE_DIR}/build && ${NINJA} ${TARGET}"
echo "构建完成"
