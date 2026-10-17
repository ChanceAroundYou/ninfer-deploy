# NInfer 部署（ninfer-deploy）

本仓库是 NInfer 部署的版本化真相源：含 NInfer/KVMem 源码、WebUI、部署定义与文档。
**不含**模型、构建目录、虚拟环境、下载暂存、日志、凭证、benchmark 输出。

## 项目简介

- **NInfer**：基于上游开源（`Neroued/ninfer` → `ashalliants/ninfer-3090`）的下游衍生推理引擎，本 fork 目标 **sm_86 / RTX 3090**。
- **KVMem 环**：本 fork 核心特性，KV 页在 VRAM ↔ pinned host RAM 间循环（生产默认不跑环）。
- **许可证**：Apache-2.0（详见底部署名）。

## 目录

- `src-tree/fusion-engine-src/` — NInfer/KVMem 源码与测试
- `deploy/` — 部署定义、systemd 模板、WebUI
- `docs/` — 运维、部署、测试、模型文档
- `scripts/` — 标准化构建/测试/部署脚本
- `patches/` — 上游改动说明

## 快速开始

```bash
# 本机（无 GPU）通过 ssh 连接远程 AI 服务器
ssh ai
cd ~/ninfer-deploy

# 构建
/home/ET/zhanghao/anaconda3/envs/nus/bin/ninja -C build <target>

# 测试（隔离，GPU 4, 端口 18492）
KV_TEST_GPU=4 KV_TEST_PORT=18492 ./run-arm.sh <arm名>

# 部署验证见 docs/operations.md
```

详细规则、设备信息、禁令见 `AGENTS.md`（AI 自动读取）。

---

## 许可与署名（Apache-2.0 §4）

本仓按 **Apache License 2.0** 发布（原文见 [LICENSE](LICENSE)）。

### 上游来源

| 仓 | 角色 | 许可 |
|---|---|---|
| `Neroued/ninfer` | NInfer 原始仓 | Apache-2.0 |
| `ashalliants/ninfer-3090` | RTX 3090 基线（v0.11.0） | Apache-2.0 |
| `tancau/ninfer-kvmem-ring` | KVMem 环原始线 | Apache-2.0 |
| `kvmem/kvmem-qw3` | 策略层（Di Chai） | Apache-2.0 |

### Vendored 第三方

| 组件 | 许可 |
|---|---|
| ggml-quants | MIT |
| llama-jinja | MIT + Unicode V3 |
| nlohmann/json | MIT |
| spdlog | MIT |
| utf8proc | MIT (expat) |
| xgrammar | Apache-2.0 |
| cpp-httplib | MIT |

### 模型权重

本仓**不含**模型权重，版权归各自作者（Qwen/Alibaba Cloud、Prism ML 等）所有，按其各自许可发布。

### 改动声明

本仓是上游的下游衍生，包含 92 件修改 + 44 件新增（2026-10-03 快照口径）。逐文件清单见 `patches/README-改动说明.md`。

本仓不含构建产物，"照本仓能编出可用二进制"未在他人的工具链上验过。

---

*NOTICE.md 内容已合并至此。*