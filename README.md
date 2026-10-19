# llm-deploy

Strata（GPU 推理引擎）部署的实验与运维记录仓库。**本仓库只有文档和脚本，不含源码** —— 引擎源码在设备 B（`ssh ai`）的 `/home/ET/chenran/strata-lab-20261017/`，那是一个独立的 git worktree。本仓库工作树里查不到任何 `.cpp/.h`：这是设计如此，不是丢失。

AI agent 的行为规则与项目事实见 [`AGENTS.md`](AGENTS.md)。

## 代码与运行环境在哪

| 位置 | 内容 |
|---|---|
| 设备 A（本机，`/home/nnb/projects/llm-deploy`） | 文档 + 脚本。无源码、无模型、无构建目录、无原始测试数据 |
| 设备 B `~/strata-lab-20261017/` | 引擎源码与测试（git worktree）：`src/ include/ tests/ docs/`，以及实验用的 lab 配置与 bench 脚本 |
| 设备 B `~/llm-deploy/` | 生产运行时（`runtime/build-current/`）、模型、WebUI、`rollback-20261018/`、受保护密钥 `qdl.env` |

实验在设备 B 上跑；本机只编辑文档、审阅脱敏指标。

## 目录

```
llm-deploy/
├── README.md                        # 本文件
├── AGENTS.md                        # AI agent 规则（自动加载）
├── docs/
│   └── strata-cache-optimization.md # 主文档：实验记录与结论（真相源）
├── scripts/
│   ├── strata_probe.py              # 只读探测：跨会话缓存保留 / 快照内存单价
│   └── strata_monitor.py            # 只读监控：/metrics + /proc + swap，exit 1 = 有告警
└── .claude/ .omc/                   # agent 本地状态，不入 Git
```

## 脚本

两个脚本都设计为在**设备 B** 上运行，只读，不写引擎也不写盘。

```bash
python3 scripts/strata_monitor.py              # 逐项告警，exit 0 无告警 / exit 1 有告警
python3 scripts/strata_monitor.py --selftest   # 8 项全红，证明告警确实能失败
MODE=mech python3 scripts/strata_probe.py      # 跨会话是否各自保留缓存（交错到达）
MODE=size python3 scripts/strata_probe.py      # 单个 park 快照的宿主内存成本（engine 自报 snapshot_bytes）
```

探测脚本指向设备 B 的 `127.0.0.1:18080`，需要 Strata 服务在跑；它把引擎的 `conversation cache:` 行导向一个 per-run 日志文件再读回（这些行写到 config 的 `log` 字段所指文件，不在 server stdout）。监控没有定时任务，只在需要时手动执行。

## 关键结论速览

详见主文档（含每条结论的证据与边界），速览：

- **短 system prompt 默认不建 root checkpoint**（`--prompt-cache-root` 默认 2048）。1993-token 的 system prompt 下暖请求全量重读；降到 512 后 read 从 1993 降到 22（−98.9%），TTFT 1.41 s → 0.23 s。
- **决定改写代价的是保留的 checkpoint 数（`--prompt-cache`），不是 interval**。2×2 四格在同一 slot 数下逐位相同：6 slots 时整段重读，24 slots 时按距离复用 3315 / 6390 / 9894。
- **多会话缓存池解决跨会话互相淘汰**：池开命中率 64.6% vs 池关 0.0%，prefill 少 2.83×，wall 少 2.45×，全程 `evictions=0`；会话与槽位无亲和性。
- **池预算是上限不是预留**，且不必要为模型预留：`--conversation-cache-mib 0` 与 `131072` 的引擎 RSS 逐 MiB 相同。缓存净成本按实际 park 的量增长（10 会话 20K–100K ≈ 17.5 GiB）。
- **显存目标已由静态 reserve 达成**：`--vram-reserve-mib 9216` → 空闲 4990 → 8994 MiB，decode 代价约 0.4%/GiB。`--kv-grow` 与 `--batch` 互斥，对本负载不可用。
- **质量验收仍缺失**：现有夹具只测缓存指标和固定输出标记，没有任务正确性评估（needle 探针除外）。

## 状态

`main` 是唯一分支。文档中的数值来自 2026-10-18 在设备 B 的实测；原始数据当时仅暂存于远程 `/tmp` 且多数已删除，仓库内不含可复算的原始数据。任何远端状态（GPU、端口、服务、配置）都要实时查，不照抄文档。
