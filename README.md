# llm-deploy

Strata（GPU 推理引擎）部署的实验与运维记录仓库。主体是文档和脚本；`src-tree/` 是引擎源码的**镜像快照** —— 权威源在设备 B（`ssh ai`）的 `/home/ET/chenran/strata-lab-20261017/`，那是 `Niko1221/Strata` 的 git worktree。改动落地在设备 B：可以在本地镜像里编辑，但要 rsync 回 B 并在 B 上 commit，然后刷新镜像。

AI agent 的行为规则与项目事实见 [`AGENTS.md`](AGENTS.md)。

## 代码与运行环境在哪

| 位置 | 内容 |
|---|---|
| 设备 B `~/strata-lab-20261017/` | **引擎源码与测试（权威源）**：`src/ include/ tests/ docs/`，以及 lab 配置与 bench 脚本。git worktree |
| 本仓库 `src-tree/strata-src/` | 上述的镜像快照（源文件 + markdown）；`tests/data/` 的 fixture 只留本地，不入库 |
| 本仓库 `docs/` `scripts/` | 实验记录（真相源）与只读脚本 |
| 设备 A（本机） | 编辑与审阅。本机有一块 RTX 4070 SUPER 但按规则不用于推理，也没有 CUDA 工具链（无 `nvcc`）和 build tree，构建与测试只能到设备 B 上跑 |
| 设备 B `~/llm-deploy/` | 生产运行时（`runtime/build-current/`）、模型、WebUI、`rollback-20261018/`、受保护密钥 `qdl.env` |

## 目录

```
llm-deploy/
├── README.md                        # 本文件
├── AGENTS.md                        # AI agent 规则（自动加载）
├── .gitignore                       # agent 状态与测试 fixture 不入库
├── docs/
│   └── strata-cache-optimization.md # 主文档：实验记录与结论（真相源）
├── scripts/
│   ├── strata_probe.py              # 只读探测：跨会话缓存保留 / 快照内存单价（自启隔离实例）
│   └── strata_monitor.py            # 只读监控：/metrics + /proc + swap，exit 1 = 有告警
├── src-tree/
│   └── strata-src/                  # 设备 B worktree 的快照（src/include/tests + docs 的 md）
└── .claude/ .omc/                   # agent 本地状态，不入 Git
```

## 脚本

两个脚本都只读、不写引擎也不写盘，但**必须在设备 B 上跑**（它们硬编码了 lab 路径）。本仓库维护脚本，设备 B 上原本没有，跑之前先拷过去：

```bash
scp scripts/strata_monitor.py ai:~/strata-lab-20261017/

python3 strata_monitor.py              # 默认打生产实例 127.0.0.1:18790（STRATA_BASE 可改）
python3 strata_monitor.py --selftest   # 8 项全红、exit 1，证明告警确实能失败（不需要 PID）
MODE=mech python3 strata_probe.py      # 跨会话是否各自保留缓存（交错到达）
MODE=size python3 strata_probe.py      # 单个 park 快照的宿主内存成本（engine 自报 snapshot_bytes）
```

- 监控：PID 优先取 `STRATA_PID`，没给就 `pgrep -f serve.server` 找引擎子进程；两者都没有则报错退出 2。没有定时任务，只在需要时手动执行。
- 探测：**自己启动一个隔离实例**（`serve.server`，`127.0.0.1:18080`），不是连现成服务；依赖 `strata-iq2_xs.json` 与 `.venv/bin/python`（2026-10-19 核实都在）。它把引擎的 `conversation cache:` 行导向一个 per-run 日志文件再读回（这些行写到 config 的 `log` 字段所指文件，不在 server stdout）。

## 关键结论速览

详见主文档（含每条结论的证据与边界）。本节不重复数据。

## 状态

`main` 是唯一分支。文档中的数值来自 2026-10-18 在设备 B 的实测；原始数据当时仅暂存于远程 `/tmp` 且多数已删除，仓库内不含可复算的原始数据。任何远端状态（GPU、端口、服务、配置）都要实时查，不照抄文档。

