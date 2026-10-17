#!/usr/bin/env bash
# 标准化部署验证脚本（生产只读检查，不修改生产）
# 用法: ./scripts/deploy-check.sh
set -euo pipefail

REMOTE_HOST="${REMOTE_HOST:-ai}"
PROD_PORT="${PROD_PORT:-18471}"

echo "=== 生产服务状态 ==="
ssh "${REMOTE_HOST}" "curl -s http://127.0.0.1:${PROD_PORT}/health && echo"
echo "=== GPU 占用 ==="
ssh "${REMOTE_HOST}" "nvidia-smi --query-gpu=index,memory.used,memory.total --format=csv,noheader"
echo "=== 端口监听 ==="
ssh "${REMOTE_HOST}" "ss -ltn | grep -E ':18471|:18492' || echo '无监听'"
echo "=== 测试端口检查 ==="
ssh "${REMOTE_HOST}" "ss -ltn | grep 18492 && echo '测试端口占用中' || echo '测试端口空闲'"