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
