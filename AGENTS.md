# AGENTS.md — llm-deploy 项目规则（AI 自动读取）

> 本文件由 Claude Code 自动加载：agent 行为规则、项目事实、工作流与禁令。
> 人读的索引与关键结论见 `README.md`。

---

## 0. 项目身份

- **本仓库是记录仓库**：Strata 部署的实验与运维真相源。只含文档与脚本。
- **代码权威在设备 B**：`/home/ET/chenran/strata-lab-20261017/` 是 `Niko1221/Strata` 的 git worktree（`src/ include/ tests/ docs/`）。本仓库的 `src-tree/strata-src/` 是它的**镜像快照**，用途是备份与离线编辑（ssh 编辑不便时）。镜像不承载开发：代码改动最终在设备 B 上 commit，本仓库只记录刷新后的快照。
- **唯一真相源是 `docs/strata-cache-optimization.md`**。`README.md` 只做索引和速览，不复制数据；两者冲突时以主文档为准，并修正 README 而不是往 README 里加数字。

---

## 1. 两台设备

### 设备 A（开发机，本机）
文档编辑、脚本维护、结果审阅。只处理脱敏指标，不跑模型、不占 GPU。

### 设备 B（AI 服务器，`ssh ai`）
- 用户 `chenran`，主目录 `/home/ET/chenran/`
- `~/strata-lab-20261017/` — 引擎源码与测试（git worktree）、lab 配置、bench 与探针脚本
- `~/llm-deploy/` — 生产运行时（`runtime/build-current/`）、模型、WebUI、`rollback-20261018/`、**受保护密钥 `qdl.env`**
- 文档里提到的 `~/ninfer-deploy`（NInfer 时代的目录）在设备 B 上已不存在

**GPU 与端口一律实时查**（`nvidia-smi`、`ss -ltn`），不照抄文档里的快照。测试只在测试卡与测试端口上跑（当前约定：GPU 3 / `127.0.0.1:18080`，该 loopback 指设备 B，不是本机）；生产实例只读，不得停止、重启、修改或压测。

### 代码镜像（`src-tree/strata-src/`）

设备 B worktree 的快照，用于备份与离线编辑。刷新（单向，B → 本地）：

```bash
R=ai:/home/ET/chenran/strata-lab-20261017
D=src-tree/strata-src
rsync -a -e ssh $R/src/    $D/src/
rsync -a -e ssh $R/include/ $D/include/
rsync -a -e ssh $R/tests/   $D/tests/
rsync -a -e ssh --include='*/' --include='*.md' --exclude='*' $R/docs/ $D/docs/
```

- 只跟踪 `src/`、`include/`、`tests/` 和 `docs/` 的 markdown。`build*/`、`data/`、`bench/` 结果、PDF/media、模型、venv、日志、`tools/`、`cmake/`、`.git/` 不入库。
- 在本地镜像里改的文件要 rsync 回设备 B 并在 B 上 commit，然后刷新本地镜像；**不要只在本地 commit 就当作改动已生效**。
- 当前快照 `477ac13`（2026-10-19）。构建与测试只能在设备 B 上跑（本地没有 CUDA 工具链与 build tree）。

---

## 2. 实验与验证纪律

- **每条结论必须有可复核的证据**：样本量、重复周期、配置差异（单变量）、缓存状态、测量方法，并写明边界。
- 客户端 usage 与服务端 summary 要**逐请求配对**；一条缺失、重复或不一致即判该周期无效，并定位采集器缺陷，不猜测。服务端 `conversation cache:` 行写到 config 的 `log` 字段所指文件，不在 server stdout。
- A/B 顺序平衡、每 arm 独立冷启动、每模式至少两个独立周期；不能用不同缓存状态或不同 prompt token 数的样本互比。
- **同一采集路径连续两次失败即换方法**，不重复盲跑；失败先保留诊断证据、定位根因再重跑。
- 机制解释不等于实测：源码读出来的行为要标「源码核实」，跑出来的才标「实测」。被取代的旧结论在原地标注「已被取代」，不删原始记录。
- **声称完成前重跑复现路径**，不用推断。质量类断言需要任务正确性证据；固定输出标记只证明响应通路，不是质量验收。

---

## 3. 文档写作

- 用绝对日期（`2026-10-18`），不用相对日期。历史章节按日期保留，不在原处改写数字。
- 未验证项显式列在「未决项 / 未做」，不用「应该」「可能」混入结论。
- 引用源码时写相对 worktree 路径（例：`src/program/generate.cpp:2989-2992`），引用上游文档同理（`docs/DETAILS.md`，不是 `DETAILS.md`）。行号以本地镜像 `src-tree/strata-src/` 为准，引用前先核对，不能凭记忆写。
- 不把远端状态写成当前状态：服务是否运行、GPU 归属、配置文件内容都要实时查；查不到就写「未核实」。

---

## 4. 禁令（不可违反）

- **生产只读**，除非经批准的维护操作；动生产前确认精确的 service / process / model / binary / runtime-lib，不用 broad matching。
- **绝不** `pkill -f`；只用本任务记录的精确 PID `kill -TERM`。
- **绝不**读、打印、拷贝或提交：`.env`、`qdl.env`、凭证、cookie、token、下载 URL、原始日志、数据库记录、请求正文、`/proc/PID/environ`。`qdl.env` 无单独授权不得迁移或删除。
- 实验数据只用非敏感合成 fixture；任何可能含真实用户信息的数据立即停止读取/传播并按敏感数据处理。
- 模型、build 输出、venv、下载暂存、日志、缓存、原始测试输出不入 Git。`.claude/` 与 `.omc/` 是 agent 本地状态，同样不入 Git。
- 不 mask/禁用 service unit 而不说明对回滚路径的影响。

---

## 5. Git

- Conventional Commits：`docs:` / `chore:` / `fix:` / `refactor:`，可带正文。
- `main` 是唯一分支；需要新分支时先说明理由并在使用后清理。
- 提交前 `git status` 确认没有把 agent 状态、原始测试数据或构建产物带进去。

---

## 6. 脚本

两个脚本只读，跑在设备 B：

| 脚本 | 用途 |
|---|---|
| `scripts/strata_monitor.py` | `/metrics` + `/proc/PID/{status,limits}` + swap 的告警检查；exit 0 无告警、exit 1 有告警；`--selftest` 证明告警能失败 |
| `scripts/strata_probe.py` | `MODE=mech` 跨会话缓存保留（交错到达），`MODE=size` 快照内存单价（engine 自报 `snapshot_bytes`） |

新增检查项必须同时证明它**能失败**（跑 `--selftest` 或等价注入），否则该检查等于没建。脚本没有定时任务：只在需要时手动执行；要加定时任务先取得确认。
