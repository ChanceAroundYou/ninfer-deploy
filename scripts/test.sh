#!/usr/bin/env bash
# 标准化测试脚本：单元测试 + 隔离集成测试
# 用法: ./scripts/test.sh [arm_name] [ENV=VAL ...]
set -euo pipefail

REMOTE_HOST="${REMOTE_HOST:-ai}"
REMOTE_DIR="~/ninfer-deploy"

echo "=== 单元测试 ==="
ssh "${REMOTE_HOST}" "cd ${REMOTE_DIR}/build && ctest --output-on-failure 2>&1 | tail -20" || echo "ctest 不可用或失败"

echo "=== 隔离集成测试 ==="
ARM="${1:-smoke}"
shift 2>/dev/null || true
ssh "${REMOTE_HOST}" "cd ${REMOTE_DIR} && KV_TEST_GPU=\${KV_TEST_GPU:-4} KV_TEST_PORT=\${KV_TEST_PORT:-18492} $* ./run-arm.sh ${ARM}"