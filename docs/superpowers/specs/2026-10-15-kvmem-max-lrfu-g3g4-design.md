# KVMem Max → LRFU → G3 → G4 四项攻坚：设计、接口契约与并行方案

> 日期：2026-10-15 晚。分支：`exp/max-lrfu-g3g4-20261015`（自 `239c28c` 切出）。
> 铁律：生产二进制 / 模型 / unit / config 不碰；测试只用 GPU4 + `:18492`；动生产文件前先备份；
> `qdl.env` 只看键名永不迁移值；ninja 限 32 并行；`Text file busy` = 引擎还活着，先停面板。
> 全文默认关（negative control 可达），每项 GPU4 验证通过才算完。

背景依据（不再重议）：`kvmem-actkv-paper-mode-2026-10`
（论文 Alg.1 / §6 / §7 + G1–G7）、`kvmem-actkv-lrfu-verdict-2026-10`
（Max 单轮项 + LRFU 跨轮项，绑定移植，D1/D3 为硬性前提）、
`kvmem-actkv-scoring-gap-2026-10`（Max 数学已验证、结构可行）。
上游原文：`~/projects/kvmem-llamacpp/kvmem/{include/kvmem/lrfu.hpp,src/host/mean_key_retrieval.cpp,...}`，
审查报告 `~/projects/kvmem-actkv/docs/kvmem-actkv-review-2026-10-10.md`（P0-3/P0-4/D1/D3 必读）。

本地现状（改点接缝，均已读透）：

- 内核 `src/ops/kvmem/kvmem_retrieve_launch.cu`：softmax-over-blocks（Eq.10），
  phase1/phase2 都是 `for qh < n_heads`，`s_stats` 占 `2*n_heads` floats，
  `head_w = 1/(L_total*n_heads)`，契约 `sum_b score[b] == n_query_tokens`（`kvmem_retrieve.h:117`）。
- 探针 `src/ops/kvmem/kvmem_score.h`：per-lane `KvMemScoreProbe`（B06），
  `begin`（不清上轮 kept，继承承重）→ `accumulate`（D-12 绝对 span ∩ chunk，
  回落 QUERY_TAIL=64；B05 长 span 切段求和，oracle 口径=真正打分的 token 数）
  → `finish`（D2H + `kvmem_select_blocks` + 发布 `kept` + 内置 oracle 自检）。
  开关唯一读取点纪律（与 `kvmem_shadow.h` 同形）；数值旋钮 `env_i32`
  （0/负=未设），布尔开关必须走 `env_flag`（B25 教训）。
- 消费 `src/models/qwen3_5/program/storage/context.cpp:1714 select_retrieval_pages`：
  内容路（`kept` + `score_host` 重排截断到 budget）优先，词法 IDF 回落（负控）。
- query 起源 `frontend.cpp:1068-1103`（最后一轮 user，绝对下标，逐请求无条件发布，
  含 -1 清陈旧）→ `prefill.cpp:1212` 逐 chunk 重发布 → `kvmem_round_end`
  （`text.cpp:1850`）`finish`。
- 预算 `context.cpp:2428-2463`：`budget = min(NINFER_KV_RETRIEVE, pool*SHARE%)`，
  B32 auto-enable（未设 + 超池才给，显式值含 0 不动），`S+E+R+F==P` 自检行。

---

## 任务 1 — Max 聚合进内核（单轮项，无状态）

**改什么**：`kvmem_retrieve_launch.cu` phase1/phase2 的 per-query-head 统计改为
per-KV-head 组内 max。`s_stats` 从 `2*n_heads` 降到 `2*n_kv_heads`；
`head_w = 1/(L_total*n_kv_heads)`；`sum_b == n_query` 不变量保持
（每 `(l,m,kvh)` 仍是 block 上的 softmax），finish 的 oracle 自检原样有效。

**接口**（Agent A 拥有，后续任务只读）：

- `KvMemRetrieveConfig` 加 `score_agg` 字段（`Mean=0` 默认 / `Max=1`，
  与上游 `GqaScoreAggregation` 同名同值，便于对照）。
- 开关 `NINFER_TERNARY_KVMEM_SCORE_AGG`（`max`/`mean`，默认 `mean`；
  读法走 `env_flag` 形——字符串比较，`=0` 不适用，`mean` 显式=负控）。
- 布线：`kvmem_score.h accumulate` 读一次 env，填 `cfg.score_agg`；
  `launch_kvmem_retrieve_scores` 按 `config.score_agg` 分派（同一 kernel 模板加 `bool MAX`
  参数，或两套统计写路径——Agent A 定，oracle 对齐为准）。

**验证**：稀释 fixture（组内分歧：1 head 强命中 + 余 head 相消；oracle 已证
mean≈0.29/max=1 @GQA12）→ Max 臂选中针块、Mean 臂按原行为；
一致场景两臂 kept 逐位相同；`sum_score==n_query` oracle 全绿；GPU4 针检 A/B。

## 任务 2 — LRFU 跨轮（跨轮项，D1+D3 是前提）

**改什么**：新头 `src/ops/kvmem/kvmem_lrfu.h`，向上游 `lrfu.hpp`
（`preview`/`commit`/`checkpoint`/`score`，λ=0.5、top_fraction=0.9）的诚实移植，
修掉审查 P0-3/P0-4 的两个 bug：

- **D1 独立 ks 字段**：`KvMemScoreProbe` 加 `std::vector<float> ks`
 （committed）+ `ks_candidate`（preview）+ `ks_round`；
  绝不复用 `score_host`（当轮分数）做跨轮累积。
- **D3 提交时推进轮次**：`finish()` 内先做上轮的 commit（若有 candidate），
  再用本轮 D2H 分数 preview；decode 不推进（`begin` 不碰 ks）；
  丢弃的 preview（本 chunk 没打分）直接丢弃、永不 commit。

**接口**（Agent A 拥有）：

- `kvmem_lrfu_preview(lane, scores, n)` / `kvmem_lrfu_commit(lane)` /
  `kvmem_lrfu_score(lane, block)`（candidate 优先，否则 committed，缺省 0）。
- 开关 `NINFER_TERNARY_KVMEM_LRFU=1`（默认关；`env_flag` 读法）、
  `..._LRFU_LAMBDA`（默认 0.5）、`..._LRFU_TOP_FRAC`（默认 0.9)。
- 消费点：`select_retrieval_pages` 内容路——LRFU 开时按 `lrfu_score` 而非当轮分重排
  （kept 仍由当轮 `kvmem_select_blocks` 产生，LRFU 只改重排键；这是 D1 的另一半：
  keep-score 写回绝不进 `set_retrieval_scores` 信号）。

**验证**：30–40 轮对话针（本地 turn2 6/6 的延长线；Agent A 新造 fixture，
针埋 6 轮前的对话记忆）；Mean / Max / Max+LRFU 三臂对照——判据照抄源头：
对话记忆针 8/8，且换问后旧块衰减（日志抽查 KS 轨迹 0.9→0.45 量级）。

## 任务 3 — G3 动作区 query（改 query 输入）

**改什么**：query span 来源加一档——上一轮 assistant 输出尾部。
优先级：action span（新鲜、一次性、`≤MAXQ`）> D-12 绝对 user span > QUERY_TAIL。
`max(0,·)` 底线：动作区注意力只取非负（审查 P0-6，有符号 cosine 的 KS 永不自愈）。

**接口**（Agent B 拥有）：

- `KvMemScoreProbe` 加 `action_begin/action_end/action_fresh`（bool，一次性，
  用后即清；陈旧动作 query 是静默错答案类故障，必须消费即焚）。
- `kvmem_publish_action_span(begin, end)`（Agent B 定位发布点：
  decode 尾或下轮 prefill 头，能拿到"上一轮 assistant 输出 token 区间"处；
  找不到精确区间就按 `_ACTION_TAIL_N` 从 ledger 尾截—— Punto 精确优先、尾截兜底，
  明确日志标注用的是哪档）。
- 开关 `NINFER_TERNARY_KVMEM_ACTION_SPAN=1`（默认关）、
  `..._ACTION_TAIL_N`（默认 128）。

**验证**：tool-call 轨迹召回对照（G3 的主场）；action 开/关两臂，
问句与历史 tool 结果词面不相交的 fixture；SELECT 行 `span_mode=action` 可审计。

## 任务 4 — G7 自适应预算（最后调预算，代码今晚、完整验证可溢出）

**改什么**：`context.cpp` ring 预算处加门控：`B_upper` + 每轮门控 + 冷却。
门控信号不用上游 `observe_confidence`（死代码，零调用）——用本地可观测的量：
**kept 边界 margin** =（末位 kept 分 − 首位 dropped 分）/ 尺度；
margin 薄 ⇒ 加预算 `min(budget*scaleup, B_upper)`，冷却 N 轮内不再加；
只增不减（论文 §6 同形），每压缩步（每次重选）检查一次。

**接口**（Agent B 拥有）：

- `NINFER_KV_RETRIEVE_MAX`（B_upper；未设=门控禁用，旧行为逐位不变）、
  `..._RETRIEVE_SCALEUP`（默认 2）、`..._MARGIN_MIN`（默认待 Agent B 按量级定）、
  `..._RETRIEVE_COOLDOWN`（默认 3 轮）。
- 状态进 `SequenceState`（与 `ring_keep`/`ring_scored_mapped` 同处）：
  `budget_cur` + `cooldown_left`；日志沿用 `[ring] budgets` 行，加 `gate=+N(cooldown)` 后缀。

**验证**：代码 + 单轮 margin 读数今晚；多轮 budget 爬升曲线验证可溢出到明天
（明确记为溢出项，不硬凑）。

---

## 并行方案（Agent A × Agent B，今晚可并行）

- **Agent A**：任务 1 + 任务 2（都在 `src/ops/kvmem/`，依赖顺序 Max→LRFU；
  交付测试二进制，不进 `build-current`）。
- **Agent B**：任务 3 + 任务 4（都在 `program/` + `frontend/`，先定位发布点/状态位再写；
  交付同上）。
- **文件边界**：A 独占 `kvmem_retrieve*` + 新 `kvmem_lrfu.h` + `kvmem_score.h` 的
  `ks_*` 段；B 独占 `frontend.cpp` + `prefill.cpp` + `context.cpp` +
  `kvmem_score.h` 的 `action_*` 段。`kvmem_score.h` 同文件追加、段名不重叠，
  合并时按段落 rebase，不抢同一行。
- **构建纪律**：远端共用一个增量构建目录（`build-selfcontained` 系），
  整编串行（谁先就位谁先编，另一方等；`ninja -j32` 上限）；单文件 oracle 探针
  各自在 `/tmp` 玩，不进树。
- **合并顺序**：A 先合（Max 是 LRFU 的排序前提）→ B 合（G3 改输入、G4 改预算，
  与 A 正交）；合完跑一次三臂全量（Mean / Max / Max+LRFU）×（action 开/关），
  G4 读数只记录不做门槛。
- **每步可回退**：源码改动记清单；各任务开关默认关即负控；收工统一 commit 或 revert。
- **不管项**：recent 数值、SSD tiering、`build-selfcontained/` 删除（稳定运行后再说）、
  `build-ninfer-sm86-clean` 回滚目录（永不删）。

## 验收序列（沿用生产五项口径，转测试端口）

GPU4 `:18492`：health → metrics → `2+2` 真实生成 → controlled restart →
SELECT/KEPT/oracle 日志三行齐。每任务独立过一遍；最终三臂对照过一遍。
