# AGENTS.md — ninfer-deploy 项目规则（AI 自动读取）

> 本文件由 Claude Code 自动加载。所有 agent 行为规则、项目事实、设备信息、运行/测试/部署流程、禁令均在此。
> 人读文档见 `README.md`。许可署名见 `README.md` 底部。

---

## 0. 项目身份

- **NInfer** 是基于上游开源项目的下游衍生（downstream derivative），**不是从零手写**。
  - 基座：`Neroued/ninfer`（原始）→ `ashalliants/ninfer-3090`（RTX 3090 基线，v0.11.0）→ 本 fork
  - KVMem 环：并入 `tancau/ninfer-kvmem-ring`（Apache-2.0）
  - 策略层：改造 `kvmem/kvmem-qw3`（Di Chai，Apache-2.0，8 件 PORTED 文件在 `src/ops/kvmem/qw3/`）
  - 定池语义：参照 CraneBW/laamaafung（仅语义，未并入代码）
  - 本 fork 目标：**sm_86 / NVIDIA GeForce RTX 3090 (24 GB)**，CUDA 12.8，版本 `0.11.0-rtx3090`
  - 上游目标 sm_120a/RTX 5090 的调参常量**不是**本机权威，必须在本卡实测
- **本仓库** `ninfer-deploy`：部署源码树的版本化真相源。含 NInfer/KVMem 源码、WebUI、部署定义、文档。
  - **不含**：模型、构建目录、venv、下载暂存、日志、凭证、benchmark 输出
- **许可证**：Apache-2.0（引擎 + vendored 三方库各自 MIT/Apache/BSD，见 README.md 底部署名）

---

## 1. 本机与远程

### 本机（无 GPU）
- 本机是开发/编排侧，**没有 GPU**，不能本地编译 CUDA 或运行推理。
- 通过 `ssh ai` 连接远程 AI 服务器（别名 `ai`，SSH config 已配置）。
- 所有构建/测试/运行都在远程执行。

### 远程 AI 服务器（`ssh ai`）
- 用户：`chenran`，主目录 `/home/ET/chenran/`
- **主目录**：`~/ninfer-deploy/`（源码 + 构建 + 脚本 + 模型 + 运行时）
  - 旧名 `kvmem-g3g4` 已重命名为 `ninfer-deploy`（两侧同名）
- **生产运行时**：`~/llm-deploy/`（独立于源码目录）
  - `~/llm-deploy/runtime/build-current/` — 已验证的生产构建
  - `~/llm-deploy/models/` — 生产模型（不入 Git）
  - `~/llm-deploy/webui/` — 已部署 WebUI
  - `~/llm-deploy/qdl.env` — **受保护密钥，勿迁移/删除/读取**
- **测试构建**：`~/ninfer-deploy/build/`（ninja 构建目录）
- **源码**：`~/ninfer-deploy/fusion-engine-src/`
- **GPU**：多卡，生产通常占用 GPU 0/2，GPU 4-7 常空闲可用于测试（**以 `nvidia-smi` 实时为准，不写死**）
- **端口**：生产 `:18471`，测试隔离 `:18492`（GPU 4）— **以 `ss -ltn` 实时为准**

### 访问方式
```bash
ssh ai                                    # 进入远程
# 远程上：
cd ~/ninfer-deploy                        # 源码/构建/脚本主目录
nvidia-smi                                # 查 GPU 占用
ss -ltn | grep 18492                      # 查测试端口
```

---

## 2. 目录结构

```
ninfer-deploy/                          # 本机仓库 = 远程主目录（两侧同名）
├── AGENTS.md                           # 本文件（AI 自动读取）
├── README.md                           # 人读简版 + 许可署名
├── src-tree/fusion-engine-src/         # NInfer/KVMem 源码与测试
│   ├── src/                            # C++/CUDA 源码
│   ├── tests/                          # 测试（models/qwen3_5/tests.cmake 注册）
│   ├── docs/                           # 引擎文档（serving.md, maintainer/*）
│   └── include/                        # 公共头文件
├── deploy/                             # 部署定义、systemd 模板、脚本
│   ├── webui/                          # WebUI 源码与非密配置模板
│   └── env/                            # 环境定义
├── docs/                               # 部署/运维/模型/测试文档
│   ├── operations.md                   # 运维流程
│   ├── deployment.md                   # 部署与重建
│   ├── testing.md                      # 测试验收
│   └── model-artifacts.md
├── scripts/                            # 标准化脚本（build/test/deploy）
├── patches/                            # 上游改动说明
├── NOTICE.md                           # 已合并进 README.md（Apache-2.0 §4）
└── LICENSE                             # Apache-2.0
```

远程 `~/ninfer-deploy/` 额外包含（不入 Git）：`build/`、`models/`、`runtime/`、`staging/`、`logs/`、`.env`、`qdl.env`。

---

## 3. 构建 / 测试 / 部署

### 构建
```bash
# 本机修改后同步到远程
rsync -a --exclude='build*/' --exclude='.git/' src-tree/fusion-engine-src/ ai:/home/ET/chenran/ninfer-deploy/fusion-engine-src/
# 远程构建
ssh ai "cd ~/ninfer-deploy/build && /home/ET/zhanghao/anaconda3/envs/nus/bin/ninja <target>"
```
- **ninja**：`/home/ET/zhanghao/anaconda3/envs/nus/bin/ninja`
- **编译器**：conda env `ninfer-build`（`x86_64-conda-linux-gnu-c++`）
- **CUDA**：`/usr/local/cuda-12.4`（或 12.9）
- **注意**：rsync -a 保留 mtime，会 defeats ninja 增量追踪 → 修改后需 `touch` 源文件或删 .a 重建

### 测试
- **单元测试**：`tests/models/qwen3_5/tests.cmake` 注册，`ctest`（需 BUILD_TESTING=ON 的 build tree）
- **隔离集成**：`~/ninfer-deploy/run-arm.sh <arm名>` → 冷启动 → 等 listening → 真实生成请求
  - 配置通过环境变量：`KV_TEST_GPU`、`KV_TEST_PORT`、`KV_CTX`、`KV_CAP`、`KV_CONC`、`NINFER_KV_RETRIEVE` 等
  - `NINFER_KV_REUSE_HOSTBACKED=1` 是环运行的必需项
  - 每臂必须冷启动（warm pool 会撞 429）
- **停实例**：精确 PID `kill -TERM`（13-17s 退出），确认 `ss -ltn | grep <port>` 空
- **生产 untouched**：`:18471` 用 `curl /health` 复核

### 部署
见 `docs/operations.md` 和 `docs/deployment.md`：
1. 记录活跃服务/可执行/模型/依赖/配置
2. 准备替代品，不改活跃路径
3. 维护窗口内切换 service unit（精确 unit，不用 broad matching）
4. 验证：service state → health → WebUI status → 真实生成 → controlled restart → 重复
5. 通过后才标记旧二进制/环境为删除候选

### 标准化脚本（scripts/）
- `scripts/build.sh` — 同步 + 构建
- `scripts/test.sh` — 单元 + 隔离集成
- `scripts/deploy.sh` — 部署验证流程
（待创建）

---

## 4. 禁令（不可违反）

### 生产安全
- 生产只读，除非经批准的维护操作
- 动生产前确认精确的 service/process/model/binary/runtime-lib
- **绝不** `pkill -f`；先查精确 PID 或 cgroup
- 替换验证前**不删**生产 binary/runtime/model
- build 目录与源码目录分离；部署必须可从版本化源码 + 非密定义复现

### 密钥与产物
- **绝不**读/打/拷/提交 `.env` 值、cookie、下载 URL、token、凭证
- 模型/构建输出/venv/下载暂存/日志/缓存/bytecode 不入 Git
- `qdl.env` 是受保护密钥，无单独授权不得迁移或删除
- 绝不读 `/proc/PID/environ`

### 合同
- Management API 变更需配对测试 + 文档
- 保持 cache-clear 合同：`POST /v1/admin/cache/clear` → `202 {clear_id}`；状态经 `GET …/{id}`

### 工程
- 提交用 Conventional Commits
- 测试覆盖率 80%+，新代码不得拉低
- 先测量再优化；浮点比较用 `np.isclose`；防除零
- `eval`/`exec`/`pickle` 禁用不可信数据；subprocess 用列表参数禁 shell=True；路径穿越拒绝 `..`

---

## 5. KVMem 环关键事实

- 环只在 `--max-concurrency 1` + `NINFER_KV_REUSE_HOSTBACKED=1` 时激活
- `startup.cpp` 在 `ring_requested() && max_concurrency > 1` 时直接抛异常
- `NINFER_KV_RETRIEVE=3072`（48 页）是唯一 8/8 召回点（配合 `NINFER_KV_NEIGHBOR_PAGES=0`），**不是单调杠杆，别往大调**
- 生产 `:18471` 用 `--max-concurrency 4`，**根本不跑环**
- 环的 KV 页循环：VRAM ↔ pinned host RAM（`HostKVArena` → `PinnedHostBuffer` = `cudaHostAlloc`），**不涉及磁盘**
- 磁盘 tier（`--disk-kv-path`）是正交的持久化层，仅 `--disk-kv-restore` 时查询

---

## 6. 当前会话进展

```
108e4cb docs(serving): clarify KVMem ring uses only VRAM + pinned host RAM
bf4de8d fix(kvmem): runtime over-subscription for ring lanes, clean prefix-adoption refusal
aedb890 feat(kvmem): per-lane retrieval mask, lane-scoped ring budget, activation pin
```

**已完成**：砍掉启动期硬限制、动态 ceiling、prefix adoption 干净 429、提取 ring_lane_budget.h、磁盘 boundary 文档+代码、过订 behavioral test 通过。

**剩余**：Stage 1.4（commit_prefix_fork 非常驻源页拒绝行为测试，需 KV harness）。

---

## 7. 文件分工

| 文件 | 受众 | 内容 | 自动读取？ |
|---|---|---|---|
| `AGENTS.md`（本文件） | AI agent | 全部规则、事实、流程、禁令 | **是**（Claude Code 自动加载） |
| `README.md` | 人 | 简版介绍 + 许可署名 | 否（需显式 Read） |
| `NOTICE.md` | — | 已合并进 README.md，原文件删除 | — |
file_path">
/home/nnb/projects/ninfer-deploy/AGENTS.md