# /goal-prompt — KVMem Max → LRFU → G3 → G4 今晚攻坚（Agent A / Agent B 并行）

> 用法：把下面 `---` 之间的整段分别粘给两个 agent（A 做①②，B 做③④）。
> 前置阅读（按顺序）：`docs/superpowers/specs/2026-10-15-kvmem-max-lrfu-g3g4-design.md`
> → memory `kvmem-actkv-lrfu-verdict-2026-10` → `kvmem-actkv-paper-mode-2026-10`。
> 上游原文在远端 `~/projects/kvmem-llamacpp/kvmem/`（`lrfu.hpp`、`mean_key_retrieval.cpp`），
> 审查报告 `~/projects/kvmem-actkv/docs/kvmem-actkv-review-2026-10-10.md`（P0-3/P0-4/D1/D3 必读）。

---

## Agent A — Max 聚合 + LRFU（`src/ops/kvmem/`，顺序 Max→LRFU）

分支 `exp/max-lrfu-g3g4-20261015`（基 `239c28c`）。本地源码在
`src-tree/fusion-engine-src/` 下对照，改动经 SSH 落到 AI 主机测试构建，
GPU4 + `:18492` 验证。生产（GPU2 / `:18471` / `:18790` / `build-current` /
模型 / unit / config / `qdl.env`）零接触；`ninja -j32` 上限；与 Agent B 串行整编。

① Max：`kvmem_retrieve_launch.cu` phase1/phase2 的 per-query-head 统计改为
per-KV-head 组内 max；`s_stats` 缩到 `2*n_kv_heads`；`head_w=1/(L_total*n_kv_heads)`；
`sum_b==n_query` 不变量保持。`KvMemRetrieveConfig` 加 `score_agg`（Mean=0默认/
Max=1，与上游同名）；开关 `NINFER_TERNARY_KVMEM_SCORE_AGG`（默认mean）；
`kvmem_score.h accumulate` 布线。验证：组内分歧 fixture（oracle已证GQA12下
mean≈0.29/max=1）Max选中、Mean保持原行为；一致场景两臂kept逐位同；oracle全绿。

② LRFU：新头 `kvmem_lrfu.h`，诚实移植上游 `LrfuHistory`
（`preview`/`commit`，λ=0.5、top_fraction=0.9），必须带 D1（`KvMemScoreProbe`
加独立 `ks` committed + `ks_candidate` + `ks_round`，绝不复用 `score_host`）
和 D3（`finish()` 内先commit上轮再preview本轮；没打分的chunk直接丢弃）。
开关 `NINFER_TERNARY_KVMEM_LRFU`（默认关）、`_LAMBDA`、`_TOP_FRAC`。
`select_retrieval_pages` 内容路：LRFU开时按 `lrfu_score` 重排（kept仍由当轮
selector产生，ks绝不写回 `set_retrieval_scores` 信号）。
验证：30–40轮对话针（针埋6轮前），Mean/Max/Max+LRFU三臂，判据=对话记忆针8/8
且换问后旧块衰减（KS轨迹 0.9→0.45量级可抽查）。

文件边界：独占 `kvmem_retrieve*` + 新 `kvmem_lrfu.h` + `kvmem_score.h` 的
`ks_*` 段；不动 `action_*` 段、 `frontend.cpp`、`prefill.cpp`、`context.cpp`。
开关默认关=负控；收工前改动记清单、可回退。

---

## Agent B — G3 动作query + G4 自适应预算（`program/` + `frontend/`）

分支、铁律、构建纪律同上（与 Agent A 串行整编，先到先编）。

③ G3：query span 加一档——上一轮 assistant 输出尾部。
优先级 action span（新鲜、一次性、`≤MAXQ`）> D-12绝对user span > QUERY_TAIL；
`max(0,·)` 底线（有符号cosine的KS永不自愈）。
`KvMemScoreProbe` 加 `action_begin/action_end/action_fresh`（消费即焚）；
写 `kvmem_publish_action_span(begin,end)` 并定位发布点
（decode尾或下轮prefill头，能拿到上一轮assistant输出token区间处；
找不到精确区间就按 `_ACTION_TAIL_N` 从ledger尾截，日志明确标注用的是哪档）。
开关 `NINFER_TERNARY_KVMEM_ACTION_SPAN`（默认关）、`_ACTION_TAIL_N`（默认128）。
验证：tool-call轨迹召回对照，问句与历史tool结果词面不相交fixture；
SELECT行 `span_mode=action` 可审计。

④ G4：`context.cpp` ring预算处加门控：`B_upper` + 每轮门控 + 冷却。
门控信号=kept边界margin（末位kept分−首位dropped分），不用上游死代码
`observe_confidence`。margin薄⇒`min(budget*scaleup,B_upper)`，冷却N轮；
只增不减，每次重选检查一次。
`NINFER_KV_RETRIEVE_MAX`（未设=门控禁用，旧行为逐位不变）、
`_SCALEUP`（默认2）、`_MARGIN_MIN`、`_RETRIEVE_COOLDOWN`（默认3）。
状态进 `SequenceState`（`budget_cur`+`cooldown_left`，与ring_keep同处）；
`[ring] budgets` 行加 `gate=` 后缀。
验证：今晚代码+单轮margin读数；多轮爬升曲线可溢出明天（明确记为溢出项）。

文件边界：独占 `frontend.cpp` + `prefill.cpp` + `context.cpp` +
`kvmem_score.h` 的 `action_*` 段；不动 A 的 `kvmem_retrieve*`/`ks_*` 段。
开关默认关=负控；收工前改动记清单、可回退。

---

## 合并与验收（谁先做完谁先合，A 先于 B 合）

顺序：A合（Max是LRFU排序前提）→ B合（G3改输入、G4改预算，与A正交）。
合完跑三臂全量：Mean / Max / Max+LRFU × action开/关；G4读数只记录不做门槛。
验收沿生产五项口径转测试端口（GPU4 `:18492`）：
health → metrics → 真实生成 → controlled restart → SELECT/KEPT/oracle三行齐。
不管项：recent数值、SSD tiering、删 `build-selfcontained/`（稳定后再说）、
`build-ninfer-sm86-clean`（永不删）。
