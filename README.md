# llm-deploy

Strata / LLM 服务的实验与运维记录仓库。**当前只有文档和脚本，没有源码树** —— 提交 `d03f0a0` 移除了原先跟踪的 `src-tree/fusion-engine-src/`、`deploy/webui/`、`docs/*.md` 等 ninfer 项目文件，只保留 git 历史。运行中的服务与引擎源码在远程设备 B（`ssh ai`，`/home/ET/chenran/strata-lab-20261017/`），不在本仓库。

## 目录

| 路径 | 内容 |
|---|---|
| `docs/strata-cache-optimization.md` | 主文档：prompt cache 命中率、多会话缓存池、显存/内存预算的实验记录与结论，含 2026-10-19 状态快照 |
| `scripts/strata_probe.py` | 只读探测：`MODE=mech` 验证多会话是否各自保留缓存（交错到达）；`MODE=size` 标定单个 park 快照的宿主内存成本。跑在设备 B，指向 `127.0.0.1:18080` |
| `scripts/strata_monitor.py` | 只读监控：读 `/metrics` + `/proc/PID/{status,limits}` + swap，逐项输出告警；exit 0 无告警，exit 1 有告警。`--selftest` 证明告警能失败（8 项全红） |
| `.claude/goals/global-kv-ring-roadmap.md` | 另一条工作线的路线图（全局 KV 复用与多并发 ring）。其引用的源文件属于已移除的 `src-tree/` |
| `.omc/handoffs/` | 会话交接记录（2026-10-19 量化选择研究） |

## 运行脚本

```bash
# 设备 B 上，只读，不写引擎或磁盘
python3 scripts/strata_monitor.py
python3 scripts/strata_monitor.py --selftest
MODE=mech python3 scripts/strata_probe.py     # 需要 Strata 服务在 :18080 上运行
```

监控没有定时任务：目前只在需要时手动执行。

## 分支

`main` 是唯一分支（2026-10-19）。原先的工作分支 `exp/max-lrfu-g3g4-20261015` 重命名为 `main`；其余分支（`b06-per-lane-index`、`iso-cache-release-20261014`）都是 `main` 的祖先、没有独有提交，已删除。远端 `origin` 仍指向 `ChanceAroundYou/ninfer-deploy`。

## 证据边界

文档中的数值来自 2026-10-18 在设备 B 上的实测，原始数据当时仅暂存于远程 `/tmp` 且多数已删除，仓库内不含可复算的原始数据。源码行号引用指向上游工作树，可能已漂移。质量方面：现有夹具只验证缓存指标与固定输出标记，**不构成任务正确性验收** —— 这一限制在主文档的「未决项」中列明。
