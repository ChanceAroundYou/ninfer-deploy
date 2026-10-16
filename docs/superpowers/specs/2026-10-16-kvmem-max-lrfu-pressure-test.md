# KVMem Max / LRFU 大压力实测记录（2026-10-16，Agent A）

> 触发指令：`可以 那我们加规模 或者缩kv池，大压力实测吧`
> 目的：在真实压力（历史远超 KV 池）下，验证 ①Max 聚合 ②LRFU 是否有**可测的**
> 质量收益（对话记忆针召回），并如实报告。
> 前置：`2026-10-15-kvmem-max-lrfu-g3g4-design.md`、提交 `fe8d5fa`。
> 生产（GPU2 `:18471` / `:18790` / `build-current` / 模型 / unit / config / `qdl.env`）
> 全程只读；收工复核 `:18471 /health = {"status":"ok"}`。

---

## 1. 压力配方

| 项 | 值 | 说明 |
|---|---|---|
| 引擎 | GPU4 `:18492`，隔离 scratch build | 与生产完全隔离 |
| KV 池 | `--kv-capacity 16384` → **256 页** | 缩池档（生产为 716800/11200 页） |
| 历史跨度 | 80 轮 filler + 8 条散布针 → **715 页** | **超池 2.8×**，绝大部分页常驻 host |
| 恢复预算 | `NINFER_KV_RETRIEVE=1024` → **16 页/轮** | "抠门档"，制造 top-N 边缘排序 |
| 必需开关 | `NINFER_KV_REUSE_HOSTBACKED=1` | 生产同款；缺它超池必 429（见 §5） |
| fixture | `/tmp/cp3_spread.py`（ai 主机） | 8 条针分埋 filler 6/14/22/30/38/46/54/62 |

fixture 设计要点：8 条针是**独立消息**、埋在 80 轮的不同深度，每条针的探测都必须
**自己**挤进 top-16；filler 每 3 行插一条与探测**同词表**的假档案记录
（safe code / city / cat / coffee / plate / number / mentor / instrument 八个槽位），
保证"检索到附近页"不等于"检索到答案页"。单针版本（cp2）在 N=96/32/16 全 8/8 无区分度，
散布针是唯一出现区分度的设计。

判据：**严格口径** —— 答案必须含真值。`Light-roast Ethiopia`（属性对、值错，来自干扰记录）
不计命中；`56`（干扰号码）不计命中。

纪律：**每臂冷启动**。同一引擎上连跑第二个大会话会撞 429（§5），且 warm 跑本身改变
命中集，不能当复现样本。

---

## 2. 结果（严格口径）

| # | 臂 | 配置 | 召回 | 命中集 |
|---|---|---|---|---|
| 1 | Mean 冷 #1 | agg=mean, N=16 | 2/8 | mentor, instrument |
| 2 | Mean warm #2 | agg=mean, N=16 | 2/8 | safe code, instrument |
| 3 | Mean 冷 #3 | agg=mean, N=16 | **6/8** | safe code, city, coffee, plate, mentor, instrument |
| 4 | Mean 冷 #4 | agg=mean, N=16 | **6/8** | 与 #3 **逐位相同**（同答案文本、同延迟） |
| 5 | Max 冷 #1 | agg=max, N=16 | 4/8 | city, coffee, mentor, instrument |
| 6 | Max 冷 #2 | agg=max, N=16 | 4/8 | 与 #5 相同 |
| 7 | Max 冷 #3 | agg=max, N=16 | 4/8 | 与 #5 相同 |
| 8 | Max warm（中止） | agg=max, N=16 | 3/7 + 429 | 前 7 探测与 #5 相同，第 8 个探测 429 |
| 9 | **Max+LRFU 冷** | agg=max, LRFU=1, N=16 | 4/8 | 与 #5 相同 |
| 10 | **Mean, N=96**（阳性对照） | agg=mean, `RETRIEVE=6144` | **8/8** | 全严格命中，含全部 8 条深针 |

配置审计：各臂引擎日志逐一核对（`evidence=` / `agg=` / `lrfu=` / `reuse host-backed: on`），
与名义配置一致；各跑的首个探测 prompt 均为 `45,519` token ⇒ 探测之前对话逐 token 相同
⇒ 跨跑差异来自引擎侧，不是对话内容。

---

## 3. 结论

**(1) Max 的召回收益未获证实。** Mean 基线自身是**双稳态**的：2/8（#1/#2）与 6/8（#3/#4）
两个模式，每个模式内部逐位可复现。Max 的 4/8 落在两模式之间——**低于 Mean 的有利模式（6/8）**。
所以在这档压力下，"Max 比 Mean 多召回"没有证据；数据形态更像 Max 把 Mean 的两个模式
"平均"掉了。

**(2) 真正的杠杆是恢复预算，不是打分方式。** 同一 Mean 配置把预算从 16 页提到 96 页
（#10）→ **8/8 全严格命中**。也就是说：这档压力下召回是**预算受限**，不是打分器不足；
抠门档下（N=16）丢失的深针，扩大预算能全部捞回。

**(3) 原 LRFU 臂测得 4/8，但不能据此认定算法“中性”。** 后续定位到实现按 chunk 而非
检索轮提交历史；439 个评分 chunk 仅对应 89 个连续绝对 query span（见 §8）。这使历史过快衰减。
因此撤回“与设计相符”的解释；先前衰减/复活探针也没有覆盖真实 chunk→轮边界映射，不能替代
引擎链路验收。修正后仍须分别报告轮边界验收与召回结果，不能把二者等同。

**(4) Max 有一个观察到的性质：跨引擎启动稳定。** 三次冷启动（#5/#6/#7）答案逐位相同，
连 warm 跑（#8）的前 7 个探测也相同；而 Mean 在冷启动之间会翻模式。若要对生产说
"可预测性"，这是支持 Max 的一条观察——但 n=3 对 n=4，且机制未定，**不作为结论**。

---

## 4. 未决：N=16 下的双稳态（今晚未钉死）

- 现象：配置、构建、对话内容全部相同，Mean 却在两个模式间跳（2/8 ↔ 6/8），
  且每个模式内部逐位可复现（答案文本 + 延迟都相同）。
- 候选机制（均未证实）：
  1. **恢复可见性时序**：host→device 的页恢复若与 attention 读存在竞态，慢一轮就少一页；
     物理页/NUMA 落位随引擎启动变化，可解释"模式按引擎启动切换"。
  2. **atomicAdd ulp 噪声**：`kvmem_retrieve_launch.cu` 头部已自述 NONDETERMINISM
     （FP32 atomicAdd 求和顺序逐跑浮动，末位 ulp 会动）；若 top-16 边界是一簇近似并列
     （本 fixture 干扰记录与针**同词表**，正是这种边界），ulp 级噪声就能翻转成员。
- 判别方法：在恢复路径与 attention 之间埋点（记录每页恢复完成时刻 vs 消费时刻），
  或把每块分数打出来看边界簇宽度。**本记录不假装已归因。**

---

## 5. 侧发现：warm pool 下的 429（独立于上述结论）

同一引擎连跑第二个 715 页会话时，第 8 个探测 429。引擎日志原文（`gpu4-cp3-max-b16.log`）：

```
req#176 started | openai-chat non-stream | 175 messages | max output 800
[ninfer] adopt host-backed: frontier=45759 need=715 usable=256 host_backed=715
[ninfer] adopt host-backed: frontier=45729 need=715 usable=256 host_backed=469
[ninfer] demote-other short: freed=246 want=715 slots=2 ... noresident=469 pins=0 partial=1 ...
[diag] materialize-one FAIL tag=host-restore usable=256 allocated=256 reserved=460 reservation=460
WARN  req#176 failed during generation | openai-chat | HTTP 429 | server overloaded
```

读数：池 256 页被前一会话占满（`slots=2` 个非活跃地址、其中 469 页已无设备副本），
demote-other 腾出 246 页后，仍有一条 **eager** 恢复路径把池打满（`allocated=256`）并抛
`ContextCacheExhausted` ⇒ 429。**即使 `NINFER_KV_REUSE_HOSTBACKED=1` 是开的**。

边界：这是压力配方的产物（池:上下文 = 1:2.8）。生产 `:18471` 的池:上下文比例宽松得多，
本次全程未观察到。进入 eager 路径的确切条件（`judge_deferred_replicas` 的哪一条为假）
未从日志定论，记为待查。

---

## 6. 复现路径

- 一键冷启动脚本：本地 `/tmp/arm_run.sh` → `ai:/tmp/arm_run.sh`；
  `bash /tmp/arm_run.sh <臂名> [ENV=VAL ...]`（停旧引擎→冷启动→等 listening→跑 fixture）。
  引擎日志 `/tmp/gpu4-<臂名>.log`，结果 `/tmp/cp3_<臂名>.out`。
- 结果文件（ai:/tmp/）：`cp3_mean_b16.out`(#1) `cp3_mean_b16b.out`(#2)
  `cp3_mean16_cold3.out`(#3) `cp3_mean16_cold4.out`(#4) `cp3_max_b16.out`(#5,#8)
  `cp3_maxrepl_cold.out`(#6) `cp3_max16_cold3.out`(#7) `cp3_maxlrfu_cold.out`(#9)
  `cp3_meanb96_cold.out`(#10)。
- 注意：早期 Mean 臂的引擎日志名为 `gpu4-cp2-mean-b16.log`（文件名误导，内容即 #1/#2 两跑，
  `evidence=16`、`reuse host-backed: on`）。

---

## 7. 后续（若要给 Max 一个能站住的结论）

1. **控制双稳态**：多跑（每臂 ≥5 次冷启动）并在启动时记录能区分模式的状态
   （host 缓冲 NUMA 落位 / 首轮恢复耗时），否则样本是两簇而非一个分布。
2. **预算扫描**（N=16/24/32/48，Mean）定位"稳定达标的最小预算"——比争论聚合方式更实用，
   直接对应 §3(2) 的结论。
3. **埋点**：每页恢复完成时刻 + 每块分数（至少边界簇）打出来，判别 §4 的两个候选机制。

---

## 8. 实现缺陷与修正验收（2026-10-16）

用户质疑实现后，发现 **LRFU 将每个评分 chunk 当成一轮**。原链路有 439 次 SELECT，
却只有 89 段连续相同的绝对 query span。λ=0.5 因而在一个多 chunk 的 query 内被反复施加。

第一次修正未通过：代码用 `query_span_begin/tokens` 判轮，而这两个字段是 **chunk 局部评分窗**，
不是日志 `span_abs` 对应的 `abs_query_begin/end`。日志直接例证：同一绝对 `[3,639)`，局部窗
依次为 `[575,639)`、`[0,3)`、`[0,2)`；第一次重跑仍是 439 次评分 / 439 轮，召回 4/8。
不能把这个失败的重跑记成修复成功。

当前修正：由 `kvmem_lrfu_preview_round` 持有绝对 query 身份；同一身份只刷新 preview，
下一身份才 commit + begin_round。未打分的 chunk 不改历史。`score_host` 与 KS 仍独立，默认开关仍关。

验证（AI 主机 RTX 3090 GPU4、原 §1 配方、C++17 host probe）：

- 新持久化测试：`tests/host/test_kvmem_lrfu.cpp`，接入 `tests/ops/tests.cmake`。
  同一 query 三次 preview 后旧 KS 保持 0.45，下一 query 为 0.225；host 编译执行通过。
- 将 helper 临时变异成每次强制开轮，测试失败：第二个 chunk 得到 round=3、KS=0.225，证明护栏能红。
- 原 host 状态探针 10/10 通过；隔离 `apps/ninfer-serve` 以 `ninja -j32` 构建通过。
- **真实压力重跑 `lrfu_absfix_max`：439 次 SELECT / 89 个连续绝对 span / 最大轮号 89；
  轮号变化与绝对 span 变化逐项一致（0 mismatch），oracle_fail=0。**
- 此次严格召回仍 **4/8**（city、coffee、mentor、instrument）。这验证轮边界修正生效，
  **没有验证召回提升**，也不证明 Max 无其他缺陷。结果 `ai:/tmp/cp3_lrfu_absfix_max.out`，
  日志 `ai:/tmp/gpu4-lrfu_absfix_max.log`。

- 冷启动负控 `lrfu_absfix_off`：439 次 SELECT，全部 `lrfu=off lrfu_round=0`，
  oracle_fail=0，严格召回 4/8，命中集与原 Max 基线一致。
- 未运行完整 CTest 全集：测试集含 GPU 资源测试，生产与其他 agent 实例仍在运行。
  但**新 CMake 注册已通过 bundle 构建验证**（2026-10-16 补测，此前是已知缺口）：
  `ninfer_add_test` 不带 `STANDALONE` 时编入共享的 `ninfer_tests` bundle，不存在同名独立
  可执行文件 —— 因此 `--target ninfer_kvmem_lrfu_test` 只编译目标文件、不会链接。
  实际验证须构建 bundle：`cmake --build . --target ninfer_tests -j32` 退出码 0、
  `tests/ninfer_tests` 链接成功，`ctest -R '^ninfer_kvmem_lrfu_test$'` 通过。
  同一次验证中 A 索引在 GPU4 隔离 scratch tree 上 `ninfer-serve` 与 `ninfer_tests` 均
  0 error 构建通过，A 相关 6 项 CTest 全通过（1.48 s）。

未解决的生命周期问题：probe 是进程生命周期的 per-lane 静态状态，没有 conversation/checkpoint
归属。`SequenceHandle` 的 epoch 是请求身份，不是跨请求会话身份；prefix 命中也不是会话证明。
因此旧会话 KS 可能被下一会话复用。直接每请求清空会破坏跨轮记忆，不能当作无损修复。
当前 LRFU 仍只适合隔离实验，默认关闭，不据此建议生产启用。未知绝对 query（`-1,-1`）
尚无可用的真实轮身份；独立评审发现它会被误当作永久同一轮，已补 guard：未知/空范围不更新
LRFU。新增 `(-1,-1)` 回归先失败后通过。该分支仅证明“不污染历史”，不提供缺失身份时的跨轮
记忆保证。最后 guard 后重建隔离目标；完整压力两臂来自 guard 前的有效绝对范围路径，未重复整轮。
没有改生产、B 的二进制或运行实例。

回退：本次逻辑变更集中于 `kvmem_lrfu.h` 与 `kvmem_score.h` 的 KS 段；后者同时包含其他未提交
工作，**禁止整文件 checkout 回退**。远端修改前副本在 `ai:/tmp/lrfu-fix-backup-20261016/`；
需按 KS 段反向修改并重建隔离目标，不覆盖其他 agent 的 action 段。未提交或推送。

---

## 9. 生命周期断点、修复及有效的配对复测（2026-10-16）

### 根因与修复

最初的“有合法 private candidate、`lineage_verified=1`，但 `source=neutral`”不是
SessionIndex 资格丢失。最终逐段埋点显示：capture 在 Begin prefill round **accept 之前**
快照 `sequence.lrfu.history`；因此 rewrite checkpoint 保存的是空历史。下一请求虽正确选中
该 checkpoint，读取到的依然是空指针。

修复令 active capture 记录当前 active round 的 token。Begin commit 接受该 token 后，只结算
拥有同一 token 的 rewrite/long-anchor checkpoint：将刚接受的不可变 history 指针发布进去，
然后清除 pending token。这样不相关请求不能写入 checkpoint，也保留 fork/rollback 的不可变
快照语义。PrivateEndpoint 仍直接从 source `SequenceState::lrfu.history` 恢复。

host 回归 `ninfer_kvmem_lrfu_test` 先因缺少 `kvmem_lrfu_checkpoint_settle()` 无法编译（RED），
实现结算 helper 后通过；其断言覆盖“错误 token 不能结算、正确 token 恰好结算”。

GPU4 最小链路复测 `lineage-checkpoint-fix-v1` 证实：

```text
begin token=5  base_round=0 source=neutral
accept token=5 round=1
begin token=7  base_round=1 source=private-checkpoint
accept token=7 round=2
...
begin token=27 base_round=7 source=private-checkpoint
accept token=27 round=8
```

同一运行中，`continue_retrieval_history=false` 和不同 session 均保持 `base_round=0`/
`source=neutral`。因此现在的 GPU4 复测实际行使了跨请求 LRFU 历史，而不是只测每请求的
neutral first round。

### 有效配对压力复测

共同条件：GPU4 `:18492`、隔离 scratch build、256 页 KV 池、`RETRIEVE=1024`、80 fillers
+ 8 probes、每臂冷启动；只在 `NINFER_TERNARY_KVMEM_LRFU=1|0` 间切换。

| 指标 | LRFU on | LRFU off |
|---|---:|---:|
| 严格召回 | **1/8** | **4/8** |
| 请求数 | 88 | 88 |
| wall 时长 | 132.312 s | 134.336 s |
| 平均请求时长 | 1.504 s | 1.527 s |
| p50 | 1.447 s | 1.440 s |
| p95 | 2.198 s | 2.191 s |
| completion tokens | 2460 | 2214 |
| completion tokens / wall second | 18.592 | 16.481 |
| 进程 CPU tick 增量 | 13,369 | 13,572 |
| RSS MiB 范围 | 114180.54–114242.05 | 114180.76–114247.89 |

on 臂日志：94 Begin、94 accept、**88 次 `source=private-checkpoint`**、最大
`base_round=87`。这证明 paired run 的历史连续性有效。

### 结论与边界

- **正确性结论：通过。** 真实 checkpoint 会在它所对应的 Begin round 接受后获得正确的
  immutable LRFU history；真正的私有续写会恢复它；负控不会恢复。
- **质量结论：本工作负载下 LRFU 没有收益，反而从 4/8 降至 1/8。** 因为配对臂仍是单次冷启动，
  且模型产生的 completion token 数不同，不能把略短的 on wall time 或 token/s 说成吞吐提升。
- **性能结论：没有可归因的端到端性能收益或成本证据。** 延迟分位数和 CPU/RSS 同量级；需要固定
  输出长度、重复冷启动才能得出性能统计结论。
- **部署边界：默认关闭，未提交、未推送、未改生产。** GPU4 在全部运行后为 13 MiB，`:18492`
  无 listener。

验证命令（隔离 scratch tree）：

```text
cmake --build .../build --target ninfer-serve ninfer_kvmem_lrfu_test -j32
ctest -R '^ninfer_(kvmem_lrfu|resource_manager|openai_schema|qwen3_5_frontend|context_cache_defaults|prefix_cache_index)_test$' --output-on-failure
```

结果：6/6 通过（1.48 s），另 `git diff --check` 通过。

---

## 10. 合并后六臂全量 + 五项验收（2026-10-16，A/B 合并后）

### 前置：被测产物确实是被提交的那棵树

六臂跑的是隔离 scratch build，**不是** `build-selfcontained`（后者早于两个提交，且属不动项）。
声称"测的是合并产物"之前先证明它：

- `git archive HEAD src-tree/fusion-engine-src` 展开后与远端 `kvmem-lrfu-lifecycle/source`
  逐文件 sha256 对比：**2473 / 2473 文件，零差异**。
- 产物 `build/apps/ninfer-serve` sha256 前缀 `1c6b08b6b9b484e8`，构建于 10-16 14:00。
- 远端构建目录 `CMakeCache.txt` 的 `ninfer_SOURCE_DIR` 正是该 source 树。

### 配方

与 §1 同（GPU4 `:18492`、池 16384 token=256 页、`RETRIEVE=1024`=16 页、80 filler + 8 散布针、
每臂冷启动），唯一新增：`NINFER_TERNARY_KVMEM_SCORE_AGG` 与 `NINFER_TERNARY_KVMEM_ACTION_SPAN`。
脚本 `ai:/tmp/arm6.sh` + `ai:/tmp/arm6_all.sh`，结果 `/tmp/cp3_m6-*.out`，日志 `/tmp/gpu4-m6-*.log`。

**G4 的解释**：六臂按指令只由两轴定义（agg × action），G4 保持**默认关**——否则预算变了，
与 §2 的既有基线（Mean 2–6/8、Max 4/8、Max+LRFU 1–4/8）不再可比。但"G4 读数"在关档下是空的
（`gate=off`），故**额外加跑一臂**（与 `m6-maxlrfu-on` 逐项相同，仅打开 `RETRIEVE_MAX=8192`）
以产生真实读数。这一臂是附加项，不属于六臂，也不作门槛。

### 结果

| # | 臂 | agg | lrfu | action | 严格召回 | 命中集 |
|---|---|---|---|---|---|---|
| 1 | `m6-mean-off` | mean | off | off | **6/8** | safe code, city, coffee, plate, mentor, instrument |
| 2 | `m6-mean-on` | mean | off | **on** | **6/8** | 与 #1 **逐位相同** |
| 3 | `m6-max-off` | max | off | off | **4/8** | city, coffee, mentor, instrument |
| 4 | `m6-max-on` | max | off | **on** | **4/8** | 与 #3 **逐位相同** |
| 5 | `m6-maxlrfu-off` | max | **on** | off | **2/8** | mentor, instrument |
| 6 | `m6-maxlrfu-on` | max | **on** | **on** | **4/8** | city, coffee, mentor, instrument |
| + | `m6-g4on-maxlrfu-on`（附加） | max | on | on + **G4** | **6/8** | safe code, city, **cat**, coffee, plate, mentor |

每臂均为 88 请求 / 439 SELECT / 439 KEPT / `oracle_fail=0`。

**证据行（逐臂核对，与名义配置一致）**

| 臂 | span_mode | ACTION 行 | agg/lrfu |
|---|---|---|---|
| 1,3 | `abs:16 tail:423` | 0 | `agg=mean\|max lrfu=off` |
| 2,4 | `action:174 tail:265` | **1** | `agg=mean\|max lrfu=off` |
| 5 | `abs:16 tail:423` | 0 | `agg=max lrfu=on` |
| 6,+ | `action:174 tail:265` | **87** | `agg=max lrfu=on` |

### 结论

**(1) Max 相对 Mean 仍无召回收益。** 4/8 对 6/8，且 Max 两臂命中集与 §2 的三次冷启动逐位相同
（city、coffee、mentor、instrument）——**跨合并复现**了 §3(1) 的结论。

**(2) G3 动作区接线成立，但对本 fixture 的召回是中性的。** action 开档把 `span_mode` 从
`abs/tail` 换成 `action:174`，且产生了真实的 ACTION 证据行
（`src=prompt_assistant abs=[1750,1757) tail_n=4 -> local [0,4) of chunk [1750,1754)`）。
但 #1↔#2、#3↔#4 的命中集**逐位相同**：改"拿什么去检索"在这档压力下没有可测的召回差异。

**(3) 本 fixture 没有行使 LRFU 的跨请求历史——这是设计使然，不是缺陷。** 三个 LRFU 臂的谱系
完全一致：`begin=89 / accept=89`，`source=neutral ×89`、`private-checkpoint ×0`、
`max base_round=0`、`max round=1`、`evidence-rejected=0`。原因是 fixture 只发
`{model, messages}`，**不带 session key**，而 `continue_retrieval_history` 默认关且要求非空
session key ⇒ 每个请求都从 neutral 起。所以这六臂测的是 LRFU 的**单轮**行为，
**不能**用来声称 A 的跨请求记忆成立——那一项由 §9 的最小链路复测负责（那里 88/94 次
`source=private-checkpoint`）。**两者的结论不可互相替代。**

**(4) 探针生命周期在 LRFU 关档下仍是"每 lane 跨请求"的（本次新发现）。**
`prefill.cpp:270` 只在 `kvmem_lrfu_enabled()` 为真时创建并绑定
`sequence.kvmem_score_workspace`；关档时 `kvmem_score_probe()` 回落到
`thread_local fallback[lane]`，该对象**进程内按 lane 常驻**。可观测症状就是上表的
ACTION 行 **1 vs 87**：`logged_action_span` 是探针字段，只喊一次，关档下永远只喊那一次。
承重字段（`armed`/`scored_this_chunk`/`scored_last_chunk`）每 chunk 由 `kvmem_score_begin`
重置，`action_*`/`abs_*`/`chunk_origin` 由 driver 每 chunk 重发，所以**本次未观察到打分污染**
（各臂 439 SELECT、`oracle_fail=0`）。但 A 引入的"每请求状态"目前**只在 LRFU 档上成立**，
关档路径的状态归属与设计意图（"lane 只是执行槽，不是历史 owner"）不一致。记为待修，
不阻塞本次合并。

**(5) 实验混杂项，必须写明。** LRFU 开/关**同时**改变了探针生命周期（见 (4)），
所以 #3(4/8) 与 #5(2/8) 的差异**不能**单独归因于 LRFU 数学；§4 记录的双稳态
（同配置 2/8 ↔ 6/8 逐位可复现）也足以解释这个量级的摆动。样本均为单次冷启动。

**(6) G4 读数（只记录，不作门槛）。** 附加臂 `gate` 行：`cur` 由 16 爬到 **128（=upper）**，
`margin=0.0000` 触发增长、冷却置 0；计数 `hold cur=128 margin=0.0000`×802、
`cur=128 margin=n/a`×52、`cur=16 margin=n/a`×30。同配置下召回 4/8 → 6/8，
与 §3(2)"真正的杠杆是恢复预算"同向。**单样本，不作结论。**

### 五项验收（GPU4 `:18492`，生产五项口径）

脚本 `ai:/tmp/accept5.sh`，跑全开档（Max + LRFU + action），日志 `/tmp/accept5.out`。

| 阶段 | 检查 | 结果 |
|---|---|---|
| 1 health | `GET /health` | PASS `{"status":"ok"}` |
| 2 metrics | `GET /metrics` | PASS http=200，3761 B |
| 3 真实生成 | `POST /v1/chat/completions` | PASS `finish_reason=stop`，content 非空 |
| 4 controlled restart | 按精确 PID 停 → 重启 | PASS 退出 **13 s**、端口释放、新 PID 不同、重启后 ready |
| 5 三行齐 | SELECT / KEPT / oracle | PASS `SELECT=9 KEPT=9 ACTION=1 oracle_fail=0`，`span_mode` action×2 / tail×7 |

**`PASS=11 FAIL=0`。**

两个**测试脚手架**的坑（都不是引擎缺陷，但会让验收假红，记录备查）：
1. **"listening on" ≠ 就绪**。端口已开、`/v1/models` 已 200，模型仍在加载，`/health` 返回
   503 `model_loading`。必须以 `/health` 轮询为准。
2. **端口释放 ≠ 进程退出**，且**别用 pgrep 模式判"是否退出"**。第一次按 2 s 固定 sleep 判，
   假红；第二次按 pgrep 模式判 80 s，仍假红——而单独实测 SIGTERM→进程消失为 **13–14 s**。
   改为对**精确数字 PID** 做 `kill -0` 轮询后通过。

### 收工状态

GPU4 13 MiB、0 个 compute app；`:18492` 无 listener、无进程；生产 `:18471` `/health` 正常且
PID 2927765 **未变**（全程未重启）；`:18790` WebUI 正常；`build-selfcontained`、
`runtime/build-current`、`build-ninfer-sm86-clean` 三个目录均在。未推送。

> 路径更正：回滚目录的完整路径是 `/home/ET/chenran/llm-deploy/build-ninfer-sm86-clean`
> （在 `llm-deploy/` 下，不在 `chenran/` 顶层）。§11 收工时按此路径核对。

---

## 11. 检索预算是非单调旋钮：打分闭环的实测证据（2026-10-16，续）

§10 的六臂全部跑在 `RETRIEVE=1024`（16 页）下，只有 §2 的 `cp3_meanb96_cold` 一臂到过 96 页且 8/8。
当时的表面结论是"排序三条轴都不动召回、预算才是杠杆"。本轮先证伪了当时用来解释它的
**"宽度饱和"模型**，再找到真正的机制。**§11 的结论只覆盖 `cp3_spread.py` 这一种工况。**

### 11.1 先纠正一条历史读数（fixture 用错）

`ai:/tmp/budget_scan.sh` 得到的"8/8 @ 2 页"用的是 `conv_pressure.py` —— 它把 8 根针塞在**同一条消息**里，
即 1–2 个连续页，预算再小也够。**那个扫描不能给 `cp3_spread.py` 的分散工况定预算**，§10 里没有采用它，
本轮也不采用。本轮全部读数来自 `cp3_spread.py`（8 根针分散在 8 个深度）。

### 11.2 度量做深：接缝几乎从不切针（TRUNC 2/216，且两个都站不住）

`cp3_spread.py` v2 把每个探针答案分成 `FULL` / `TRUNC` / `DISTR` / `MISS`。
**消息序列与 v1 逐字节相同**（只改汇报），所以与 §10 已记录的运行可比。

**27 次 v2 运行、216 个探针答案，TRUNC 只有 2 个**，其余失败要么是 MISS
（`'Not provided.'`），要么是 DISTR（读到 filler 的干扰值：`'56'`、`'Pickle'`、`'GH-3456'`）。

**这 2 个 TRUNC 标签本身不成立。** 它们来自**同一个配置的同一个探针**：
`s0-b24` 与 `b24-r0`（都是 B=24 / R=0）的 `cat name`，两次都答 `'Mitzi'`。
分类器在 `'Mitzi'` 里匹配到了 `mituan` 的 **3 字符**前缀 `'mit'`。但：

- `'Mitzi'` 不在 `FAKE_CATS` 里，是模型**自己编的**名字；
- 分类器从长到短试前缀（`'mitua'` → `'mitu'` → `'mit'`），只命中 `'mit'`
  ⇒ 文本里**没有** `'Mitu'` 或 `'Mitua'` 这样的悬空残片；
- `'mit'` 是 `'Mitzi'` 这个词**自身的前缀**，不是被页缝截断的尾巴。

⇒ **没有观察到任何一次页缝把针切成两半。** 这也说明 v2 的分类器仍有一个已知假阳性模式
（短前缀撞上幻觉词），判读 TRUNC 时必须回看答案原文，不能只看标签。

`R=1` 付出 `2R+1 = 3` 倍的宽度税，而收益一次都没出现。**这不否定 D-5 修复本身**
（它针对的是真实症状），只说明这个 fixture 无法证明它的收益。
同时也**证伪了计划里 `needle_tokens/64 ≈ 0.31` 的跨页率估算**——实测远低于此。

### 11.3 B×R 四臂：R 不解释召回

| 臂 | B（页） | R | 召回 | 缺失 |
|---|---|---|---|---|
| `b16-r1` | 16 | 1 | 6/8 | cat, **lucky** |
| `b16-r0` | 16 | 0 | 6/8 | cat, **mentor** |
| `b24-r1` | 24 | 1 | 6/8 | cat, mentor |
| `b24-r0` | 24 | 0 | 6/8 | cat, mentor |

计划的主预测（**B=24 / R=1 应为 8/8**）**被证伪**。R 从 1 换到 0（即换成上游 actkv 的策略，
上游没有接缝机制）在 B=16 上不改变召回数，但换掉一根针（`mentor` ↔ `lucky`，两个缺失集
**互不包含**）；而在 **B=24 上两种 R 的缺失集完全相同**。

⇒ **R 在 B=16 有可测效应、在 B=24 没有**——即 R 的效应本身依赖于 B。这不是"R 无用"，
是"R 的效应不可外推"。到 §11.9 会看到它在 B=48 上能一次翻掉两根针。

### 11.4 预算扫描：非单调，且可复现

固定 `agg=mean` / `action=off` / **`R=0`**，每臂冷启动，`gate=off`（G4 全程关）：

| B（页） | 16 | 24 | 32 | **48** | 64 | 96 | 128 |
|---|---|---|---|---|---|---|---|
| `NINFER_KV_RETRIEVE` | 1024 | 1536 | 2048 | **3072** | 4096 | 6144 | 8192 |
| 召回 | 6 | 6 | 6 | **8** | 7 | 7 | 7 |

`preferred` 几乎总是打满预算（B=16 时 92/95 次 = 16；B=48 时 82/93 次 = 48）
⇒ **不缺候选，缺的是切割线的位置。**

**重复运行判定这不是抖动：**

| 配置 | 冷启动次数 | 结果 | 缺失针 |
|---|---|---|---|
| B=48 | **4** | 8/8, 8/8, 8/8, 8/8 | — |
| B=64 | **4** | 7/8, 7/8, 7/8, 7/8 | `coffee`（4 次全同） |

⇒ **把 `NINFER_KV_RETRIEVE` 从 3072 提到 4096，会稳定地丢掉一根针。** 这是可复现的回归，
不是噪声。`NINFER_KV_RETRIEVE` **不是单调的质量旋钮**。

**缺失的是哪一根针，本身也在漂移：**

| B（页） | 16 | 24 | 32 | 48 | 64 | 96 | 128 |
|---|---|---|---|---|---|---|---|
| R=0 缺失 | cat, mentor | cat, mentor | cat, coffee | **—** | coffee | instrument | mentor |
| R=1 缺失 | cat, lucky | cat, mentor | 未测 | **cat, coffee** | 未测 | 未测 | 未测 |

（`—` = 8/8 无缺失；"未测" = 该格没跑。R=1 的 B=48 一行来自 §11.9 的 `p4-b48-r1`。）

三点：

- `cat name`（filler 22）在 B ≤ 32 时**恒定失败**，B ≥ 48 后恢复；`mentor surname`（filler 54）
  在 B=16/24/128 失败却在 B=48/64/96 成功。**没有任何一根针的失败可以用"深度"解释。**
- **B×R 存在真实交互**：B=16 时 R=0 缺 `{cat, mentor}`、R=1 缺 `{cat, lucky}`（差一根）；
  B=48 时 R=0 是 8/8 而 R=1 缺 `{cat, coffee}`（**差两根**）。按计划阶段 3 写定的判据
  （"只在阶段 2 显示交互时才展开 B×R 面"），**交互成立**。
- 但注意 B=24 时两种 R 缺的完全相同 ⇒ **交互不是处处成立**，它自己也是轨迹的函数。
  展开 B×R 面的动机因此从"找最优点"变成 §11.6 的机制验证（见 §11.9）。

### 11.5 引擎按配置是精确确定的

对日志逐条比对 **438 行 × 每行 512 个 kept 块 id**：

| 对比 | 配置关系 | 438 行中不同的行数 | 曾分歧过的块数 |
|---|---|---|---|
| `s0-b16` vs `b16-r0` | **同一配置**，不同进程 | **0** | 0 |
| `s0-b64` vs `s0-b64-r2` | **同一配置**，不同进程 | **0** | 0 |
| `s0-b16` vs `s0-b24` | 只差 B | 143 | 474 / 716 |
| `b16-r0` vs `b16-r1` | **只差 R** | 144 | 495 / 716 |

浮点仍有 1 ulp 抖动（两次 B=48 运行的 `min_score` 是 `2.71735e-05` / `2.71734e-05`），
但**不改变离散的 kept 集合**。

两个推论：

1. **同配置零分歧** ⇒ §10 的六臂矩阵与 §11.4 的扫描都是**信号不是噪声**，单样本 A/B 有效。
   这同时追溯性地为 §10 的六臂结论提供了它当时没有的确定性依据。
2. **跨配置差异是真实效应**，可以归因到配置差异本身（进程身份已被上表排除）。

### 11.6 机制：打分闭环

关键在于**候选集其实不随配置变**。`kvmem_select_blocks`（`src/ops/kvmem/kvmem_select.cpp:134-139`）
的候选是**固定连续区间** `[max(middle_begin, first_block), middle_end)`。

**逐臂核对（27 个证据日志的末条 SELECT）：**

| 元数据 | 值 | 覆盖臂数 |
|---|---|---|
| `n_blocks` | 716 | 25 |
| | 715 | **2** —— `p4b-b16-lrfu-r1/r2`，见下 |
| `budget_blocks` / `kept` | 512 / 512 | **27（全部）** |
| `candidates` | 652（716−64） | 25 |
| `scored_kept` | 448（= 512−64−0） | **27（全部）** |
| `skip` | 204 | 25 |
| `window_tokens` | 32768 | **27（全部）** |
| KEPT 行数 | 438 | **27（全部）** |

**那 2 个 `n_blocks=715` 的例外已查清，且不削弱结论**：逐位比对 `n_blocks` 序列
（438 个值），`p4b-b16-lrfu-r1/r2` 与 `s0-b16` **在前 437 个 SELECT 上完全相同**，
只在**第 438 个（最后一个）** 是 715 而非 716。原因是最后一个探针的**答案文本**短一个 token，
而探针答案会被追加进对话。⇒ **探针阶段之前的输入布局在所有臂上逐位一致**，
KEPT 的分歧（起于第 291 行）不是长度或内容差异造成的。

所以分歧不在"谁有资格被打分"，而在**分数本身**。分数是当前 chunk 的 query 对每个块的注意力质量
（`src/ops/kvmem/kvmem_retrieve.h:155-159`），softmax 在**装配出来的窗口**上归一化。而

```
window_tokens = kept × kPagedKVPageSize = 512 × 64 = 32768
历史长度      = n_blocks × 64          = 716 × 64 = 45824
```

`window_tokens` 不是估算值：`KvMemStore` 组装 working set 时逐块累加，
`plan.total_window_tokens = window_pos`（`src/ops/kvmem/qw3/kvmem_store.cpp:741`），
且累加过程就是**把窗口内的块重排成连续位置**：

```cpp
b.baked_pos = static_cast<int64_t>(window_pos);   // kvmem_store.cpp:738
window_pos += b.n_tokens;
```

⇒ **模型只看装配出来的窗口，不是全历史**；而窗口正是上一轮的 kept 集。

#### 11.6.1 更正：耦合走**内容**，不走位置（2026-10-16 晚，读码更正）

上面这段初稿里有一句**错的**，必须撤回：我曾写"改动 kept 集会移动其后每一块在窗口里的位置
（`baked_pos`），即改变 RoPE 相位"，把位置当成分数变化的通道。**通道不成立。**

`src/ops/kvmem/kvmem_retrieve.h` 把两个操作数都钉在**内容帧**：

| 操作数 | 帧 | 出处 |
|---|---|---|
| query `q` | **pre-RoPE** | `:128-130` "does not de-RoPE anything: the caller hands over `qn` AFTER its rmsnorm and BEFORE the in-place rope, which is what 'in the content frame' means here"；`:86-88` "`kn/qn` are live, pre-RoPE and L2-hot in between" |
| index `kbar` | **de-RoPEd** 键的块均值 | `:25-27` `kbar[l,b,g] = (1/\|b\|) * sum_{t in b} deRoPE(k[l,t,g], p_t)` |

两者都与位置无关 ⇒ **`baked_pos` / 窗口布局 / `plan.remaps` 不移动检索分数**。
`window_tokens` 那段读数仍然有效（它证明"模型只看窗口"），但它**不是**分数变化的机制。

真正的耦合有两条，都在**内容**上：

1. **`q` ← 窗口。** 分数是当前 chunk 的 query 对块的注意力质量，而 `q` 是残差流的产物，
   残差流由**对装配出来的窗口做注意力**产生 ⇒ 窗口内容变 ⇒ `q` 变 ⇒ 分数变。
2. **`kbar_sum` ← 写入时的窗口。** 索引操作数不是存下来的均值，是块内 de-RoPEd 键的
   **有序 F32 和**（`:53-57`）；这些键是**该块被写入时那次前向**的产物，而那次前向同样只看得见
   当时装配出来的窗口。⇒ **块被写进去的内容本身依赖于检索历史。**

第 2 条比初稿的说法**更强**，而不是更弱：不只是"排序器给自己的输出打分"，
而是**排序器的输入（索引）本身就是检索决策的函数**。闭环因此不是实现缺陷，是这套设计的固有性质 ——
凡是从"模型自己在有限窗口上的表示"里取检索信号，索引就必然带上窗口的历史。

> **第 t 轮的选择决定第 t+1 轮的输入表示**（不只是决定 t+1 的候选集）。

**排除掉竞争解释：分歧不是"对话内容不同"造成的。** fixture 里的 assistant 回复由引擎自己生成，
所以各臂的 messages 理论上可能不同。实测**每个臂里 filler 与 needle 的回复全部是空串 `''`**
（`[needle ...] -> ''` ×8，四个臂逐字相同）⇒ 各臂看到的 user 消息与 assistant 占位**逐字节相同**。
而首次分歧落在第 291 行，属于 fixture 中段、**远在任何探针之前**（8 个探针是最后 8 个请求）。
⇒ 分歧只能来自引擎内部状态，不可能来自输入内容。

于是：

- 任何改变"取哪些页"的旋钮（**B 和 R 都是**）都扰动窗口 ⇒ 扰动分数 ⇒ 扰动下一轮选择；
- 两个臂**从来不在给同一张表排序**，所以"预算更大"不构成超集关系，召回对 B 自然非单调；
- 这也解释了为什么**只差 R** 的 `b16-r0`/`b16-r1` 会分歧 495/716 个块，
  且**首次分歧同样落在第 291 行** —— 与只差 B 的签名（474 块 / @291）几乎逐项相同。
  **同一个闭环，两个入口。**

### 11.7 这对"联合优化"意味着什么

把 (B, R, 排序) 当成一个联合分配问题去求解，**这个框架是错的**：B 和 R 根本不作用于排序，
它们作用于取页轨迹，而轨迹反过来决定排序的输入。在这个闭环上调参是拟合噪声 ——
B=48 的 8/8 是这一条轨迹的产物，**没有理由相信换一个 fixture 最优点还在 48**。

同样，§10 那三条排序轴（agg / action / lrfu）全部测在**同一条轨迹**上（B=16 / R=1）。
§11.9 显示这三条轴的效应**并不小**（max 4/8↔7/8、lrfu 5/8↔8/8），
但它们的方向随轨迹翻转 ⇒ §10"排序无用"这个结论的**适用范围只是那一条轨迹**，
既不是"排序无用"，也不是"排序有用"。

**§11.9 把这个判断从"不迁移"升级为"符号不固定"**：max 在 B=16/R=1 是 4/8（劣于 mean 的 6/8），
在 **同样 B=16** 的 R=0 上是 7/8（优于 mean 的 6/8）；lrfu 在 B=16/R=0 是 5/8（劣于 6/8），
在 B=64/R=0 是 8/8（优于 7/8）。**同一对配置，优劣关系随另一个旋钮翻转。**
这不能用"某个聚合更好"解释——聚合的数学没变，变的是它打分的对象。

**可复用的操作结论：**

- `NINFER_KV_RETRIEVE` 目前**不能按"越大越好"调**。3072 是本 fixture 上的观测最优点，
  但它是轨迹产物而非可外推的设计常量；3072 → 4096 稳定掉一根针（4 次对 4 次）。
- `R`（`NINFER_KV_NEIGHBOR_PAGES`）在本 fixture 上是纯成本项（§11.2：216 个探针里 0 次真实截断），
  **但没有证据支持关掉它** —— D-5 修的是真实症状，只是本 fixture 触发不到。
  反过来，B=48 上 R=0→1 掉两根针这件事**也不能**归因给"接缝变差了"，
  因为 R 改变的是取页轨迹。
- 任何 A/B 的召回差异，只要两侧的取页轨迹不同，就**不能**归因到旋钮名义上的机制。
  本轮能给出归因的只有一类对比：**同配置重复运行**（§11.5），它们零分歧。

### 11.8 复现路径

- 冷启动臂脚本 `ai:/tmp/arm7.sh <名> [ENV=VAL ...]`（精确 PID 停旧实例 → `/health` 轮询就绪 →
  标记日志行号 → 跑 fixture → 只 dump 标记之后的证据行）。R 的值由脚本 `echo "knobs: ..."` 记录，
  **引擎不打印它**（`context.cpp:1838` 是 `proto_kv_neighbor_pages()` 在树内的唯一引用）。
- 驱动：`arm7_all.sh`（B×R 四臂）、`arm7_sweep.sh`（B 扫描）、`arm7_rep.sh`（B=48/64 各 3 次重复）、
  `arm7_p4.sh`（§11.9）。
- 结果 `/tmp/cp3_s0-*.out`、`/tmp/cp3_b*-r*.out`；证据 `/tmp/ev_*.log`；knobs 记录在
  `/tmp/arm7_*.log`（**不在** `gpu4-*.log` 里——`arm7.sh` 的 `> "$LOG"` 会截断引擎日志，
  那些 `echo` 走的是脚本自己的 stdout）。

### 11.9 阶段 4：每条旋钮的**符号都不固定**

原计划阶段 4 的问题是"换一个未饱和的预算，排序杠杆能不能表达出来"。§11.4 给出两个候选工作点：
**B=64（mean 稳定 7/8，只剩一根针余量）** 与 **B=48（mean 8/8，顶到天花板）**。

脚本 `ai:/tmp/arm7_p4.sh` + `arm7_p4b.sh`。**下表除注明外，R 一律 = 0**（`NINFER_KV_NEIGHBOR_PAGES=0`），
`action=off`，每格重复次数列在括号里：

| B（页） | mean / lrfu off | max / lrfu off | mean / **lrfu on** |
|---|---|---|---|
| 16 | 6/8（×1） | **7/8**（×2） | 5/8（×2） |
| 24 | 6/8（×1） | — | — |
| 32 | 6/8（×1） | — | — |
| **48** | **8/8**（×**4**） | — | — |
| 48，**R=1** | 6/8（×2） | — | — |
| 64 | 7/8（×4） | **8/8**（×2） | **8/8**（×2） |
| 96 | 7/8（×1） | — | — |
| 128 | 7/8（×1） | — | — |

**三个翻转，每一个都是单变量：**

**(1) R 翻转 max 的符号 —— 在同一个预算 B=16 上。**
§10（B=16、**R=1**）：mean **6/8** > max 4/8。本轮（B=16、**R=0**）：mean 6/8 < max **7/8**。
预算没动，只动了接缝半径。**"Max 更差"不是 Max 的性质，是那条轨迹的性质。**

**(2) B 翻转 LRFU 的符号 —— 在同一个 R=0 上。**
B=16：mean **6/8** > lrfu 5/8。B=64：mean 7/8 < lrfu **8/8**。
两边都是 `agg=mean`、`R=0`、`action=off`，只差预算。

**(3) R 本身在固定 B=48 上翻转两根针。**
R=0：**8/8**（4 次）。R=1：**6/8**（2 次，缺 `cat`、`coffee`）。预算一分没动。
⇒ **B 和 R 不可分离**：动召回的不是"预算多大"，是"取了哪些页"。

**(Q2) 缺的针永远在候选区间里 —— 每一次 miss 都是排序 miss，不是可用性 miss。**
论证不需要知道针落在哪个块：候选区间 `[max(64, first_block), 716)` **与配置无关**（§11.6 已逐臂核对），
所以若某个臂找到了某根针，它的块在该臂里是候选；那么在**任何**臂里它同样是候选
（区间相同），只是分数落在了切割线之下。B=64 上的实测也与此一致：
只把 agg 换成 max、或把 lrfu 打开，7/8 就回到 8/8。
这一条同时**彻底取代**了原计划的"宽度饱和"模型（§11.2/§11.4 已证伪它）。

**合起来：没有一个旋钮有固定符号。** mean/max、lrfu 开/关、R=0/1、B 大/小 ——
每一个的召回效应都依赖于其余旋钮所决定的那条取页轨迹。这正是 §11.6 那个闭环的直接后果。

> ⚠️ 与 §10 对比时的口径：§10 用的是 `cp3_spread.py` **v1**，其 `hit` 判据
> （`any(k in low for k in keys)`）与 v2 的 `FULL` **完全相同**，v2 只是额外分出 TRUNC/DISTR/MISS；
> 且 v2 的消息序列与 v1 逐字节相同。两侧产物同为 `1c6b08b6b9b484e8`。故可直接比较。

### 11.10 收工状态（本轮）

**隔离核对（收工实测）**

| 项 | 状态 |
|---|---|
| `:18492` listener / 进程 | **0 个 / 无** |
| GPU4 compute apps / 显存 | **0 个 / 13 MiB** |
| 生产 `:18471` `/health` | `{"status":"ok"}` |
| 生产 PID | **2927765 —— 与 §10 收工记录相同，全程未重启** |
| `:18790` WebUI | 在 |
| `kvmem-lrfu-lifecycle/build` | 在 |
| `runtime/build-current` | 在 |
| `llm-deploy/build-ninfer-sm86-clean` | 在（回滚目录，未动） |
| `build-selfcontained` | 在（未动） |
| `qdl.env` | 2021 B，**未读值、未迁移、未删除** |
| 本地 git | 只有本 spec 一个文件被修改；**未提交、未推送** |

**本轮新增的临时产物**（全在远端 `/tmp`，不进 Git）：
`arm7.sh` / `arm7_all.sh` / `arm7_sweep.sh` / `arm7_rep.sh` / `arm7_p4.sh` / `arm7_p4b.sh` /
`cp3_spread.py`(v2)；结果 `/tmp/cp3_s0-*.out`、`/tmp/cp3_b*-r*.out`、`/tmp/cp3_p4*-*.out`；
证据 `/tmp/ev_*.log`；knobs `/tmp/arm7_*.log`。

**未做 / 留给下一轮：**

- 阶段 1.3（给 `[ring] budgets:` 行加 `neighbor=%u`）—— 需重建+重同步，本轮用脚本 `echo` 代替。
- 阶段 5 的行为改动 —— **需另行授权**，且设计前提已被 §11.6 取代（见计划文件"执行结果"）。
- §10(4) 的 `prefill.cpp:270` 状态归属缺口 —— 仍待修；它让 LRFU 关档臂的读数不干净。
  但注意 §11.9 已经表明：**在闭环收敛之前，修它也不会让 LRFU 的结论变得可迁移。**
- `tests/` 下对 `add_seam_neighbors` / `select_retrieval_pages` / `ring_measure_margin` /
  `RingBudgetMargin` **仍是零覆盖**（本轮未新增引擎代码，故未新增测试）。

见文末"§11 收工核对"。

### 11.11 本次全部测量的适用边界：**生产根本不跑环**（2026-10-16 收尾）

§11 的每一个读数都跑在"环开着"的工况（测试配方设了 `NINFER_KV_WINDOW`）。
收尾核对生产时发现：**生产的环是关的**，所以 §11 的结论对生产**当前配置**没有直接适用性。

判据是**反证，不需要读任何 secret**：

| 事实 | 出处 |
|---|---|
| 生产 cmdline 带 `--max-concurrency 4` | `/proc/2927765/cmdline`（只读 cmdline，未读 environ） |
| 环被请求且 `max_concurrency > 1` ⇒ **抛异常** | `startup.cpp:1270-1276`（`throw std::invalid_argument("NINFER_KV_WINDOW (KVMem ring) requires --max-concurrency 1")`） |
| 生产进程活着、`/health` = ok | 实测 |

⇒ `ring_requested()` 为假 ⇒ `NINFER_KV_WINDOW` 未设 ⇒ 环关。

而 `NINFER_KV_RETRIEVE` 只被 `proto_kv_retrieve_pages()`（`context.cpp:2046`）读，
该函数只在 `if (ring)` 内被调用（`context.cpp:2487`，`ring = proto_kv_window_pages() != 0 && …`）。
**环关 ⇒ 这个变量一次都不会被读。** 生产日志里也确实没有任何 `[ring] budgets:` 行
（早先一次 `grep -i ring` 的命中全是 `du**ring** generation`，是假阳性）。

**两条结论：**

1. **`NINFER_KV_RETRIEVE` 在生产是死变量。** 把它钉在 3072 是**测试配方**的约定
   （记录在 `gpu4-test-recipe` 记忆里），**不是生产调参建议**。生产配置无需改动 ——
   这恰好与"生产零接触"一致。
2. **规模差 44×，§11 的"非单调"不一定迁移到生产规模。** 测试配方 `--kv-capacity 16384`
   ⇒ **256 页**，`share = 128` 页；生产 `--kv-capacity 716800` ⇒ **11200 页**，
   `share = 5600` 页。§11.4 观察到的非单调（6,6,6,8,7,7,7）与"8192 撞上 share 上限"
   这件事，在 5600 页的 share 下形态会完全不同。

**所以本轮的性质要说清：这是一次原型评估，不是生产调参。** 它证明了闭环的存在与
非单调性，但**没有**证明生产规模下存在可调的旋钮。要断言生产规模的行为，
必须在生产规模上重跑（需要单独授权）。

### 11.12 `NINFER_KV_RETRIEVE` 的工作点：3072，**不要往大调**

测试配方下的预算扫描（§11.4，`cp3_spread.py 80`，8 根分散针，`agg=mean` / `action=off` / **`R=0`**）：

| `NINFER_KV_RETRIEVE` | 1024 | 1536 | 2048 | **3072** | 4096 | 6144 | 8192 |
|---|---|---|---|---|---|---|---|
| 页 | 16 | 24 | 32 | **48** | 64 | 96 | 128 |
| 召回 | 6/8 | 6/8 | 6/8 | **8/8** | 7/8 | 7/8 | 7/8 |
| 缺哪根 | cat, mentor | cat, mentor | cat, coffee | **—** | coffee | instrument | mentor |

⇒ **3072（48 页）是扫描到的唯一 8/8 点，且紧邻的更大值 4096 可复现地丢一根针**
（B=64 冷启动 4 次全是 7/8、缺的都是 `coffee`）。所以 **3072 不是"保守取值"，
是实测最优点**；`NINFER_KV_RETRIEVE` **不是单调的质量旋钮，别按"越大越好"往上调。**

⚠️ **这个工作点与 R 绑定**：同在 B=48，`R=0` 是 8/8 而 `R=1` 是 6/8（缺 `{cat, coffee}`，
§11.9 的 `p4-b48-r1`）。所以"钉 3072"必须连同 `NINFER_KV_NEIGHBOR_PAGES=0` 一起说，
否则不成立。**两者都是测试配方的约定，不是生产调参建议**（生产不跑环，见 §11.11）。

### 11.13 收尾：状态归属缺口修复 + 零覆盖补齐（2026-10-16 晚，未提交）

§11.10 的"未做"清单里有两项已完成，记在这里。

#### (a) `prefill.cpp:270` 状态归属缺口 —— 已修

**缺陷**：`sequence.kvmem_score_workspace` 的创建与绑定被 `kvmem_lrfu_enabled()` 包着，
于是 **LRFU 关档**下 `registry.bound[lane]` 始终为空，`kvmem_score_probe(lane)` 回落到一个
**进程常驻的 `thread_local fallback[lane]`**。而 `kvmem_score.h` 明确 `kept` 是
**刻意不按轮清空**的（"本轮没打分 ⇒ 保留上一次的选择"）—— 落在进程常驻对象上时，
"上一次"跨的是**请求**，不是轮。严重性是**读到上一个请求的选择**，不是悬垂指针
（`kvmem_score_lrfu_on()` 就是 `kvmem_lrfu_enabled()`，所以那个字段只在 LRFU 开时被读，
而那时绑定确实发生过）。

**修法**（4 处，均为一处原则的全部同类）：

| 位置 | 改动 |
|---|---|
| `prefill.cpp:272-273` | 创建 + 绑定**移出** LRFU 守卫（`start_sequence`） |
| `prefill.cpp:1190-1193` | 第二次绑定去掉守卫（`advance_prefill`） |
| `hybrid_program.cpp:760-761` | 同上（hybrid 续跑路径） |
| `kvmem_score.h` | **删掉 `thread_local fallback` 数组**，`kvmem_score_probe` 改返回指针，无绑定即 `nullptr` |

配套：`kvmem_score_unbind` 现在**连同绑定一起清掉 round 指针**（probe 可能比它的 sequence
活得久——`local` 那个 shared_ptr 是**生命周期护栏，不是缓存**，锁每次调用都取），
否则一个仍"看起来绑着"的 probe 会暴露悬垂的 `lrfu`。另删了 3 个零调用方的死 API
（`kvmem_score_selected_blocks` / `kvmem_score_host_scores` / `kvmem_score_scored_blocks`，
已核实全树无外部调用方）。11 个调用点改成"取指针 → 空则早返回 → 绑引用"。

**入口完整性已核**：`kvmem_score_workspace` 全树只有两个创建点
（`prefill.cpp:272` 与 `hybrid_program.cpp:760`），而序列入口只有两条
（`commit.cpp:122 → start_sequence`，与 hybrid 续跑）。**且绑定为空是 fail-safe**：
`bind(null)` ⇒ `probe()` 返回 `nullptr` ⇒ 各调用点早返回（退化为"不打分"），
不会回到"读别人的选择"。拆解在 `context.cpp:253-255` 对称解绑 + reset。

**回归测试**：`tests/host/test_kvmem_score_binding.cpp`（新增），核心断言就是缺陷本身 ——
`require(kvmem_score_probe(0)->kept.empty(), "the next sequence on the same lane inherits no selection")`。
在旧实现下这一行会读回 `{7,8,9}`，即**第一个请求要的页**。

#### (b) 零覆盖补齐 —— 2/3，第 3 个受边界约束挡住

`add_seam_neighbors` 与 `ring_measure_margin` **逐字搬**到
`src/ops/kvmem/kvmem_page_selection.h`（新文件，仅 `<algorithm> <cstdint> <utility> <vector>`，
零 CUDA 依赖 ⇒ 纯 g++ 可编）。搬而不是重写，所以行为等价是**可证的**而不是论证的。
放这里是因为 AGENTS.md 的边界："Ops own closed mathematical and state-transition
implementations"。`RingBudgetMargin` 在 `program_impl.h` 变成别名。

新增 `tests/host/test_kvmem_page_selection.cpp`：reach=0 恒等（负控）、空入空出、
`{15}` reach 1 → `{15,16,14}`、紧预算下前向优先、预算是硬上限、best-first 顺序、
边界夹取、越界页整页跳过、去重、reach 0..3 × budget 1..12 的性质环；margin 侧覆盖
陈旧读数清除、空切割、nullptr 出参、等距 → 1、近平局 → <0.05、大间隙 → >10、平分数 → 0、
double 路径与 float 一致。

> 两个 `ring_measure_margin` 重载**刻意不合并成模板**：double 那个是词法回退的负控路径，
> 它的 `1e-12` 是 double 字面量而 float 那个是 `1e-12F`；合并必须二选一，会静默搬动一个护栏常量。

**扰动证明（5 项，全部按名红）** —— 用影子 include 目录 `/tmp/pert`，**源文件未被修改**：

| 扰动 | 触发的断言 |
|---|---|
| 前向/后向顺序对调 | `one page completes to itself and both neighbours` |
| 去掉预算 resize | `property: never over budget` |
| 去掉越界跳过 | `out-of-range page is skipped entirely` |
| 去掉去重 | `no duplicate pages` |
| margin 不再清陈旧读数 | `non-binding budget clears the margin` |

**仍未覆盖：`select_retrieval_pages` 本身。** 它是 `ProgramImpl` 的成员，读
`SequenceState`（定义在 `program_impl.h`）。**它搬不到 `ops/` 而不倒置
ops → program 的依赖** —— 这正是只有"只吃基本类型"的两个 helper 能搬的原因。
收尾时已核：该函数**不使用任何 `ProgramImpl` 成员**（只用 `sequence.*` 与自由函数），
所以它"是自由函数穿着成员的外衣"，但要测它仍需构造一个真 `Program`（GPU + 模型 + KV 池），
`tests/` 下没有任何测试构造过真 `Program`（`test_resource_manager.cpp` 用的是 `FakeProgram` 替身）。

⇒ 现状是：**排序 + 接缝 + margin 这段（即该函数里真正的算术）已被覆盖**，
未覆盖的是它的守卫条件、IDF 词法回退路径，以及读 probe 的那层编排。
要补需先决定是否把内核抽成只吃基本类型的函数（约 120 行、参数较多，或引入一个小 struct），
**这是一次对 §11 全部读数所依赖的那条路径的重构**，故未在本次擅自做。

#### (c) 全量测试套件

`ctest -j2`（`ai:/home/ET/chenran/kvmem-lrfu-lifecycle/build`，共 261 项）：

| 项 | 结果 |
|---|---|
| 维度 | 结果 |
|---|---|
| 261 项中 | **258 通过（已跑）、3 失败、17 跳过**（需要真机 fixture 的 `*_real_*`） |
| 失败 | **3** —— 见下，**均与本次改动无关** |
| 未完成/放弃 | 无（宽变体 `--wide-only` 也全跑完了，包括 `ninfer_softmax_attention_wide_test` 1392 s、`ninfer_gdn_replay_fold_wide_test` 466 s） |

> 上一版按"已完成 257"拆成了通过 254 / 未完成 4；最终实跑是 261 项中 258 通过、3 失败、无一项未跑（`Exit code 8` 是 ctest 封装的失败计数，实际 FAILED 列表就是下面 3 个）。


三个失败，逐个定因：

| 测试 | 原因 | 判定依据 |
|---|---|---|
| `ninfer_state_store_test` | `LinearAttentionStatePool::copy_slot` 不复制（"copied conv slot expected byte 0x6b, got 0x0"） | **已证既有**：用 10-13 的旧二进制（`llm-deploy/build-ninfer-sm86-clean/tests/ninfer_tests`，早于本次全部改动）跑同一测试，**逐字相同的报错与退出码 1** |
| `ninfer_artifact_writer_interop_test` | `ImportError: cannot import name 'TypeAlias' from 'typing'` | **环境**：该 build 的 `Python3_EXECUTABLE=/usr/bin/python3.8`，而 `TypeAlias` 需 ≥3.10。`ai` 上**没有** 3.10+。异常在 import `tools/artifact/schema.py` 时抛出，与 C++ 无关 |
| `ninfer_chat_templates_test` | `jinja2.exceptions.TemplateRuntimeError: No filter named 'items' found`（`tools/chat_templates/qwen3_8.jinja:148`） | **环境**：`items` 不是 jinja2 内置过滤器，而 Python 侧参考渲染器（`test_chat_templates.py:27`）只设了 `trim_blocks`/`lstrip_blocks`/`loopcontrols`，没注册 `items`。异常由 Python 的 jinja2 抛出，**在 C++ 二进制被触及之前**。注：这一条是**按机制判定**，非按旧二进制证明（那两个 build 里没有 `ninfer_jinja_test`） |

> 本次改动的 3 个定向测试全绿：`ctest -R kvmem` → 3/3
> （`ninfer_kvmem_lrfu_test` / `ninfer_kvmem_page_selection_test` / `ninfer_kvmem_score_binding_test`）。




