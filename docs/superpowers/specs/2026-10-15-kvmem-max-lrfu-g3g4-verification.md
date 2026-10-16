# G3/G4 改动清单与验证记录（Agent B）

- 分支：`exp/max-lrfu-g3g4-20261015`
- 日期：2026-10-15/16
- 对应设计：`docs/superpowers/specs/2026-10-15-kvmem-max-lrfu-g3g4-design.md`
- 范围：G3 动作 query span（`frontend/` + `prefill.cpp` + `kvmem_score.h` 的 `action_*` 段）、
  G4 自适应预算门控（`context.cpp` + `program_impl.h`）。
  与 Agent A 串行整编；**未触碰** A 的 `kvmem_retrieve*` / `ks_*` 段。

## 1. 改动清单

`git diff --stat`（A 合入 `dafe6ba` 之后实测）：7 文件，+512 / −50。

| 文件 | 增删 | 内容 | 定位锚点 |
|---|---|---|---|
| `frontend/frontend.cpp` | +35 / −6 | 删除 `kvmem_publish_query_span` 调用；反向扫 `message_boundaries` 取最后一条 assistant 消息，裁 `<\|im_start\|>` 帧，填 `action_span_begin/end` | 原 `kvmem_publish_query_span` 调用处 |
| `frontend/prepared_prompt.h` | +12 / −9 | 删除 `kvmem_publish_query_span` 声明；`PreparedPromptData` **末尾**追加 `action_span_begin/end` | 结构体尾部（见下方布局告警） |
| `program/prefill.cpp` | +59 / −21 | 删除 `kvmem_publish_query_span` 定义（连同其 per-lane broadcast）；`start_sequence` 里在 `ledger.swap()` **之前**抓 `action_fallback_begin/end`；`advance_prefill` 每 chunk 在 `kvmem_score_set_query_span` 之后发布 action span | 紧邻既有 query span 发布点 |
| `program/transactions/commit.cpp` | +8 / −1 | 强制 token 后缀路径在 `prefill_text_chunk` 之前把 query/action span 与 chunk origin 发布为中性值，堵住上一请求的 span 泄漏 | `append_forced_tokens` 内 |
| `program/program_impl.h` | +52 / −1 | 新增 `struct RingBudgetMargin`；`SequenceState` 增 `budget_cur`/`cooldown_left`/`ring_margin`；`RequestControl` 增 `action_fallback_begin/end`；`select_retrieval_pages` 改 5 参 | — |
| `program/storage/context.cpp` | +226 / −5 | `ring_measure_margin`（float/double 两版）+ 门控状态机 + `gate=` 日志后缀 + 4 个 env 读取器 | 与 A 共享文件，A 占 `select_retrieval_pages` 内容路径的一个 hunk |
| `ops/kvmem/kvmem_score.h` | +120 / −7 | `action_*` 探针字段、`kvmem_action_span_enabled()`/`kvmem_action_tail_n()`/`kvmem_score_set_action_span()`、`accumulate` 内的 action 档、`finish()` 清 `action_fresh`；SELECT 行加 `min_score=`/`action_src=`/`action_tail_n=` | **追加在 A 的 `ks_*` 段之后** |

### 发布点设计更正（2026-10-16）

本表早先版本记的是另一套设计：frontend 侧定义并调用 `kvmem_publish_action_span`，
与旧的 `kvmem_publish_query_span` 并列。**该设计已废弃并从代码中删除**，原因有二：

1. frontend 跑在**请求线程**、lane 尚未分配（lane 在 commit 时才绑定），因此它无法
   发布到某一个 lane，只能向所有 lane 广播 —— 两个并发请求会互相覆盖 span。
2. 它依赖 thread_local 探针，而提交线程的 TLS 到不了 worker 线程（实测：frontend
   侧发布之后，worker 的打分器读不到该值）。

现行设计是 **worker 侧发布**：frontend 只把 span 填进 `PreparedPromptData`，由
`advance_prefill` 在每 chunk 打分前发布到本 lane。旧 broadcast 本来要覆盖的
「不走 driver 循环」的形状（`append_forced_tokens` 的强制 token 后缀 prefill），
改由 `commit.cpp` 显式发布中性值处理。因此**代码中已不存在任何 `kvmem_publish_*`
函数**；若在本文件别处或旧稿中见到该名字，那是过期描述。

整编后构建验证（2026-10-16，A 合入之后）：GPU4 隔离 scratch tree 上
`ninfer-serve` 与 `ninfer_tests` bundle 均 `cmake --build -j32` 退出码 0、0 error。

### 布局告警

`action_span_begin/end` 追加在 `PreparedPromptData` **末尾**，而非按语义就近放置。
该结构体有以偏移量/聚合初始化依赖字段顺序的下游用法，插在中间会静默错位；
追加在尾部是刻意的，但意味着**后续任何人再往这个结构体加字段都要知道这段历史**。

## 2. 开关表（全部默认关 = 负控）

| 环境变量 | 默认 | 单位 | 作用 |
|---|---|---|---|
| `NINFER_TERNARY_KVMEM_ACTION_SPAN` | `false` | bool | G3 动作 span 档总开关 |
| `NINFER_TERNARY_KVMEM_ACTION_TAIL_N` | `128` | token | 从 assistant 消息尾截取的窗口 |
| `NINFER_KV_RETRIEVE_MAX` | 未设 = **门控禁用** | **token（读入后除 64）** | 预算上界 `B_upper`；例如 16384 → 256 页 |
| `NINFER_KV_RETRIEVE_SCALEUP` | `2` | × | 门控放大倍数 |
| `NINFER_KV_RETRIEVE_COOLDOWN` | `3` | 轮 | 一次增长后的冷却轮数 |
| `NINFER_KV_RETRIEVE_MARGIN_MIN` | `1.0` | 比值 | margin 低于此值才允许增长 |

> 单位更正：`NINFER_KV_WINDOW`/`NINFER_KV_RETRIEVE`/`NINFER_KV_SINK`/
> `NINFER_KV_RETRIEVE_MAX` 经 `proto_env_pages()` 按 **token** 读入再除 64。
> `SELECT budget_blocks/kept/skip` 属于评分选择器，不等于物理池实际驻留页；
> `NINFER_TERNARY_KVMEM_SCORE_BUDGET` 控制该选择器预算，`NINFER_KV_RETRIEVE`
> 控制 ring 恢复预算，不能靠缩小 `NINFER_KV_WINDOW` 推断 SELECT 预算变小。

### G4 门控语义

`margin = (末位 kept 分 − 首位 dropped 分) / 档内相邻名次平均分差`，一律降序排列故恒非负。
`margin.valid == false` 表示这次切分没有决定任何事（budget 为 0 或不构成截断），**不授权增长**。
`ring_measure_margin` 每次调用**无条件写** `margin_out`，`select_retrieval_pages` 入口再清一次，
保证所有早退路径都留下 `valid=false` —— 这堵的是「上一次 reselect 的旧 margin 被当成这一次的读数」。
门控**只增不减**；每次重选检查一次；lane 回收时三个状态字段一起归零。

## 3. 运行验证（Agent B，同一二进制三臂对照）

构建：`ai` 主机 scratch 目录 `~/kvmem-g3g4/build/apps/ninfer-serve`，`ninja` 全绿（667/667）。
本节是早先构建接通验证，非质量验收。远端 launcher 现有默认值为
`--max-context 65536 --kv-capacity 16384`（256 页）；旧稿写 `kv-capacity 131072`
与当时 `pool 256` 读数矛盾，已撤回，不能据旧稿复现当时全部环境。
生产未在本轮维护范围内，:18492 的归属更正见第 5 节。

| 臂 | 变量 | 关键读数 |
|---|---|---|
| A 负控 | 全关 | `gate=off`；`span_mode` 只有 `tail`(41)/`abs`(2)，**零条** ACTION 行（尽管 `action_src=prompt_assistant` 已出现）；`min_score=0.000169134` > 0；`oracle_ok=1`；`restored` 恰好封顶 128 |
| B 门控 | `NINFER_KV_RETRIEVE_MAX=16384` | `gate=hold cur=128 upper=256 margin=1.2857 cooldown=0`（>1.0 不增长）对 `gate=grow cur=256 upper=256 margin=0.2100 cooldown=3`（<1.0 增长）；`evidence` 128→246，`free=0`，`S+E+R+F==pool` 自检仍 `yes`；`cooldown` 3→2→1 递减；`restored` 达 199–242（A 臂封顶 128）→ 增长的预算确实落到了 retriever |
| C 动作档 | `NINFER_TERNARY_KVMEM_ACTION_SPAN=1` | `src=prompt_assistant abs=[34230,34348) tail_n=4 -> local [0,4) of chunk [34230,34234)`；`span_mode=action` ×2（A 臂 0）；契约在 action 档上**逐位成立**：`sum_score=4.000 query_tokens=4`、`sum_score=114.000 query_tokens=114`，两处 `oracle_ok=1` |

A 臂验证了开关关闭时没有启用 action/gate，且评分契约通过；
这些读数不足以证明旧实现与新实现逐位相同，也不足以证明端到端召回收益。
`min_score` 是所有被评分块的最小分，受 query token 数影响，不能作为准确率指标。

### 2026-10-16 真实效果测试更正

旧 `effect.py` 的 `REPORT` 文本包含评分词 `rollback`，答案并非只存在于历史记录；
此前两个长上下文臂均答对不构成召回提升的证据。只输出 `UNKNOWN` 的第一版还缺少明确问句，亦作废。
EA2 调用返回 137，但远端进程存活、引擎日志有正常请求完成记录；137 的来源未定位，
不能归类为模型崩溃或错误答案。

新测试改为六个独立的 12 位合成校验码，答案仅出现在 archive 中，assistant 只列待查 ID；
按 JSON 中每个 ID 对应的完整值做精确匹配，不再用关键词命中评分。
短上下文控制组（654 prompt tokens）已通过 **6/6**。长上下文对照正在重跑：
同一 scratch 二进制、GPU5/:18499，每臂冷启动；基础恢复预算设为 1024 token（16 页），
G3 臂只开 ACTION_SPAN，G4 臂只设 RETRIEVE_MAX=8192（128 页上界）。

第一档实测（24,974 prompt tokens，256 页物理池，所有大请求 `cache_n=0`）：

| 臂 | 六项精确匹配 | 请求总耗时 | 审计读数 |
|---|---:|---:|---|
| 基线 | 6/6 | 30.056 s | evidence=16，gate=off，action SELECT=0 |
| G3 action 开 | 6/6 | 30.579 s | evidence=16，gate=off，action SELECT=2 |
| G4 gate 开 | 6/6 | 30.809 s | evidence 16→32→64→128，最后 hold，upper=128 |

G4 三次增长的 margin 分别为 0.0035、0.0732、0.0392，增长后 cooldown 均置 3。
这些是单个请求内部多次重选的爬升，不是跨 HTTP 请求的预算累积。
基线与 G3 最后物理掩码日志均为 `span=393 resident=246 hidden=147`，
与评分选择器的 kept 数分开记录。此档三臂均满分，**没有证明 G3/G4 带来召回收益**；
单次耗时也不足以支持性能结论。

第二档（76,274 prompt tokens，1192 个逻辑 prompt 页，256 页物理池，约 4.66 倍超池）：

| 臂 | 精确匹配 | 命中项 | 请求总耗时 | 审计读数 |
|---|---:|---|---:|---|
| 基线 | 3/6 | 01、02、04 | 144.180 s | evidence=16，action SELECT=0 |
| G3 action 开 | 3/6 | 01、02、04 | 146.475 s | evidence=16，action SELECT=2 |
| G4 gate 开 | 6/6 | 全部 | 146.810 s | evidence 16→32→64→128，最后 hold |

三臂大请求均 `cache_n=0`，无输出截断，温度 0；使用相同六个校验码与同一 prompt。
G3 与基线的回答 JSON 相同，此 fixture **没有测到 G3 召回收益**。
G4 在首轮补回 03、05、06 三项；增长 margin=0.0035/0.0241/0.1955，
上界 128 页、冷却置 3，未发现 `oracle_ok=0`。
最终物理掩码三臂都为 `span=1194 resident=246 hidden=948`，
但 G4 的隐藏位置分布不同；收益不是“总驻留页更多”的证明，而是预算/页面选择变化下的回答改善。
独立冷启动复验已完成：基线再次 **3/6**（同为 01、02、04），144.729 s；
G4 再次 **6/6**，146.210 s。两次冷启动对照均为 50%→100%，
支持“该压力 fixture 上 G4 改善了端到端精确召回”的限定结论，
不能外推为真实工具轨迹或其他工作负载的通用收益。
独立审查补充：Qwen3_5 同时含全注意力 KV 与线性注意力递归状态；答对校验码
不能单独证明该针所在物理 KV 页曾被恢复。本轮未将针 token 偏移映射到具体页的
恢复/驻留轨迹，因此只报告端到端回答改善，不宣称已证明物理恢复的因果机制。
此外，基线与 G4 各两次冷启动虽结果一致，但同机其他 fixture 曾出现冷启动双稳态，
样本仍不足以排除该类波动；不作统计显著或通用稳定性声明。
脚本：`ai:~/kvmem-g3g4/recall_check.py`，长档命令 `RECALL_LINES=2000 python3 recall_check.py`；
原始结果不纳入 Git，本报告保留解释结果所需的配置与摘要。
门控状态变化已审计为：
`(16,0)→(32,3)→(32,2)→(32,1)→(32,0)→(64,3)→(64,2)→(64,1)→(64,0)→(128,3)→(128,2)→(128,1)→(128,0)`，
括号为 `(budget_cur,cooldown_left)`。所有 84 条 SELECT/臂均无 oracle 失败。
G3 和基线最终 SELECT 的 query 都为 `span_mode=tail query_tokens=4`，
中间 action 打分不代表最终 decode 使用了 action 驱动的 keep 集；
这是一条需要后续专门定位的覆盖路径，不据此宣称已证明 G3 无效。

产物范围：本轮使用 2026-10-16 00:32 构建的 scratch 二进制，而非重编当前工作区。
已核对远端 `context.cpp` 与本地相同；`kvmem_score.h` 与当前本地文件不同
（工作区同时存在 Agent A 后续改动），因此结果只代表该已构建产物，不等同于当前未提交树的验收。
远端源码含 action 开关/消费逻辑，运行时也实际记录 action SELECT。
随后对比表明 `kvmem_score.h` 的当前差异位于 LRFU 的候选提交/轮次逻辑；
本轮三臂均未启用 LRFU。未重编当前工作区的限制仍保留。

**C 臂的一处读数注意**：ACTION 行与 SELECT 行里的 `action_tail_n` 报的是**本 chunk 实际应用**
的条数（先后 4、114，合计 118 = 整条 assistant 消息；该消息不足 128），
**不是** `NINFER_TERNARY_KVMEM_ACTION_TAIL_N` 的回读值。想确认开关取值请查 env 本身，别读日志这个字段。

### 固定上界对照与成本测量约束（2026-10-16）

追加实验：低压力 24,974 token、高压力 76,274 token，各比较固定 16 页、
固定 128 页、自适应 16→128 页。每格两次独立冷启动，第二次反转策略顺序；
G3 与 LRFU 均关闭，仍使用上述既有 GPU5/:18499 scratch 二进制。
脚本 `ai:~/kvmem-g3g4/adaptive_compare.py`；12 个样本全部正常结束（driver exit 0）。
验收问题不是“比固定小预算答得更好”，而是“相对固定上界，能否保持质量并减少可测成本”。

| 压力 / 策略 | 两次精确匹配 | 两次 prompt 时延（s） | 两次请求总耗时（s） |
|---|---|---|---|
| 24,974 token / 固定 16 页 | 6/6、6/6 | 25.533、26.607 | 29.850、30.994 |
| 24,974 token / 固定 128 页 | 6/6、6/6 | 26.034、26.603 | 30.406、31.011 |
| 24,974 token / 自适应 16→128 | 6/6、6/6 | 26.359、26.517 | 30.752、30.918 |
| 76,274 token / 固定 16 页 | 3/6、3/6 | 141.095、141.179 | 145.608、145.687 |
| 76,274 token / 固定 128 页 | 6/6、6/6 | 140.624、140.514 | 146.642、146.546 |
| 76,274 token / 自适应 16→128 | 6/6、6/6 | 140.436、140.554 | 146.455、146.579 |

表内按 replicate 1/2 排列，实际第二轮策略执行顺序反转。全部大请求 `cache_n=0`、
`finish_reason=stop`；低档每样本 34 条、高档 84 条 SELECT 均 `oracle_ok=1`，共 708 条。
固定 16 页高档两次均仅命中 01、02、04；其余策略高档均六项全中。
高档固定 16 页输出 95 token，其余样本 127 token，所以不能把其较短总耗时直接当作
等质量性能优势；表中同时列出不受该输出长度差异直接支配的 prompt 时延。

自适应四次均完整走过 16→32→64→128，**包括固定 16 页已足够满分的低压力任务**；
未体现“简单任务留在小预算”的区分能力。相对固定 128 页，自适应平均 prompt 时延
低档 +0.120 s（约 +0.46%）、高档 −0.074 s（约 −0.053%），
而单轮差值的方向都会反转；每格仅两次、共享主机也非独占，不能声称稳定时延收益。

**结论：本轮没有证明自适应相对固定 128 页有额外价值。**
高压力下 3/6→6/6 的改善可由固定 128 页同样获得，支持“提高预算有用”，
不支持“当前门控有效节约成本”。默认保持关闭，不以此结果推荐启用；
也不将此限定 fixture 的负面结果外推为所有自适应策略都无效。
后续若改进门控，应先能在固定低预算已满分的任务上避免无必要增长，
并补全实际 ring H2D 计数，再用固定上界与固定低预算双对照验证质量/成本。
本轮只做测试与记录，未调整门控参数、未修改或重编引擎。

### 新针位的拆分因果复验（2026-10-16）

为避免把“预算变大”误记成“门控有效”，以另一组更靠后的六个针位（相对行位置
24%、35%、46%、57%、68%、79%）重新做了两个**彼此独立**的实验组。两组均为
650 / 2000 行各两次独立冷启动、第二次反转执行顺序；G3/LRFU 关闭，使用同一既有
scratch 二进制和 GPU5/:18499，`cache_n=0`、`finish_reason=stop`。

| 独立问题 | 任务 | 对照 A（平均 prompt s） | 对照 B（平均 prompt s） | 精确召回 |
|---|---|---|---|---|
| 预算增大本身是否改善？ | 24,974 token | 固定 16：25.999 | 固定 128：26.609 | 都为 6/6、6/6 |
| 预算增大本身是否改善？ | 76,274 token | 固定 16：140.936 | 固定 128：140.276 | 16 为 2/6、2/6；128 为 6/6、6/6 |
| 门控相对固定上界是否额外获益？ | 24,974 token | 自适应：26.720 | 固定 128：26.626 | 都为 6/6、6/6 |
| 门控相对固定上界是否额外获益？ | 76,274 token | 自适应：140.837 | 固定 128：141.094 | 都为 6/6、6/6 |

第一组的唯一变量是恢复预算（16 对 128），在新针位的高压力任务上再次分离出
预算的因果效应：固定 128 两次均比固定 16 多恢复四项（2/6→6/6）。这不能归因于门控。
第二组的唯一策略差异为门控：两种策略在所有四个冷启动高低压力样本中召回相同；
自适应仍每次完整增长 16→32→64→128。其与固定 128 的平均 prompt 差为低压力
+0.094 s、高压力 −0.257 s；这两次观测和共享主机条件不足以建立性能收益，且低压力
仍未避免涨满，故仍未证明门控自身的额外价值。

这 16 个新样本的 944 条 SELECT（低档 34、高档 84 条/样本）均为 `oracle_ok=1`。
执行器正常退出；复核 scratch 精确 PID=[]、:18499 无监听、GPU5=13 MiB，
GPU2/:18471 `/health` 为 `200 {"status":"ok"}`。原始摘要位于
`ai:~/kvmem-g3g4/split-compare-summary.json`，不纳入 Git。

**分离后的结论：**固定大预算在两套针位的高压力 fixture 上都稳定改善精确召回；
当前自适应门控仅复现了相同的最终 128 页预算，尚未显示相对固定 128 页的质量或
可测时延优势。因此不应将前者当成后者成功的证据。

**禁止将 `ring retrieve ... restored=N` 累加当作实际 H2D 页数或字节数**：
`restore_kv_pages_from_host()` 返回恢复后已驻留的请求页集合，包含调用前已经驻留的页；
日志还按集合大小及首尾页去重，所以日志条数也不是恢复调用次数。
试验 JSON 中早先命名的 `restored_pages_sum` / `restore_events` 只反映这些诊断日志，
不用于成本收益判断。已有 `/stats` 的 `context_cache.*_kv_transfers.h2d` 只覆盖
context-cache transaction 观测，ring 的直接 `copy_from_host()` 路径未计入；
不能拿该计数替代完整 ring 搬运量。本轮未修改源码或增加埋点，
只用请求耗时和 `timings.prompt_ms` 比较可观测时延，不宣称 H2D 节省。

## 4. 溢出项（未做，明确记账）

1. **多轮爬升曲线未测**。B 臂观测到的是**单请求内** 128→256 的一次爬升与 3 轮冷却；
   跨请求「多轮逐步爬升到 `B_upper` 后持平」的曲线今晚没有跑。
2. **`record.observe_confidence` 未接入**。按设计，门控信号用 kept 边界 margin，
   上游那条 `observe_confidence` 是死代码，本次刻意不消费它。
3. **tool-call 轨迹召回对照 fixture 未落库**。设计里要求的
   「问句与历史 tool 结果词面不相交」的 fixture 只做了运行时观察，没有固化成回归用例。

## 5. 遗留产物

| 位置 | 内容 | 处置 |
|---|---|---|
| `ai:~/kvmem-g3g4/`（早先约 4.8 GB） | scratch 源码副本、build 与当前复验脚本 | 不在 Git 内；仅用于当前复验，未授权删除 |
| `ai:GPU5 / :18499` | 本轮冷启动测试实例 | 收工实测 scratch PID=[]、:18499 无监听、GPU5=13 MiB；:18471 `/health` 返回 ok |
| `ai:build-selfcontained` / `:18492` | 同机其他会话使用的隔离产物/实例 | 本轮未修改或停止；不再误称为生产 |

`stop-arm.sh` 只匹配 `~/kvmem-g3g4/build/apps/ninfer-serve` 且复核 `/proc/PID/exe`，不匹配则拒绝 ——
这是为了在 18492 与 18499 同时在跑时不误杀生产。

**观察边界**：:18492 的 pid 在早先会话中至少变化两次
（`2434324` → `2541059` → `2561199`）。只能证明该端口对应的进程变了，
不能据此证明生产服务异常重启，也不能仅凭 stop 脚本匹配规则排除所有间接影响。
同机 Agent A 的压力测试文档将 GPU4/:18492 标记为隔离实例，生产标记为 GPU2/:18471；
因此前文把 :18492 称为“生产”的标签未经服务归属核实，不应继续据此报警。
本轮只管理已核实 exe 为 `~/kvmem-g3g4/build/apps/ninfer-serve` 的 GPU5/:18499 实例。

## 合并验收状态（2026-10-16，阻塞）

已重新阅读 goal-prompt 收尾段，合并顺序必须 A→B，再在 GPU4/:18492 跑
Mean / Max / Max+LRFU × action 关/开六组合。G4 仅记录、不作为门槛。
本次只读复核 GPU4=13 MiB、:18492 无监听。

A 的基础实现已有提交 `fe8d5fa`，但后续 LRFU 轮边界修正仍未提交，所属会话正在只读方案阶段，
明确要求不触碰其共享 hunk/index；A 还指出 KS 缺少会话/checkpoint 归属。
本地可见 `kvmem_score_probe` 使用进程静态 per-lane 数组（kvmem_score.h:226-228），
LRFU 历史挂在该探针中，而 query 绝对区间仅识别位置、不识别会话。
因此不同会话复用同 lane 或恢复 checkpoint 时存在使用不对应历史的风险，不能把轮边界修正称为完整修复。
A 补充的定位：`kvmem_score.h:684-687` 将 `p.lrfu` 传给 preview，
`kvmem_lrfu.h:182-194` 仅以绝对坐标判轮，`context.cpp:111-115` 的 lane 失效仅推进 epoch。
这是代码路径确认的风险，**尚无专门双会话数值复现**。
A 已有动态证据仅覆盖轮边界：439 SELECT / 89 spans / 89 轮；关闭控制为 439 SELECT / 0 轮，召回均 4/8。
另有待定位项：同轮最后 chunk 的短 tail-fallback 评分可能覆盖有效 query 证据。
A 会话先更正：当时人工请求仅为“修改建议”，自动审批事件不构成实施授权，
因此暂停了 Program/checkpoint-owned history 与显式 accept/abort 生命周期重构。
随后该会话报告收到新的人工实验授权，恢复其自身的实现与隔离验证；
本会话不据此扩大范围，仍不触碰其源码/index、不替其提交或重编。
目前尚未收到 A 的完整可集成产物与验收证据，因此当前共享源码不作为可集成版本。
以上旧行号只描述此前审阅快照，不保证对应当前未完成树。

当前未执行 A/B 新提交，未重编合并产物，也未把旧构建的结果冒充本轮六组合验收。
先解决 A 的归属/生命周期阻塞并取得明确可合并版本，再按顺序继续；
生产与 build-selfcontained、build-ninfer-sm86-clean 均未修改或删除。

## 6. 回退

运行时关闭本次两个功能：启动隔离测试实例时将 ACTION_SPAN 设为 0，
并移除 RETRIEVE_MAX；其余设置保持不变。生产开关不在本次修改授权范围内。

源码回退必须按归属 hunk 操作：六个文件中存在与 Agent A 共享的修改，
不能整文件 checkout，也不能假定 HEAD 已保存其他会话改动。
先保存并审阅当前 diff，明确 G3/G4 hunk 后制作定向反向补丁；回退后重编隔离产物并重跑负控。
本轮没有执行源码回退、提交、推送或生产部署。
