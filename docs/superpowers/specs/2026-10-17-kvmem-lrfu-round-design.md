# KVMem LRFU / Max 闭环修复设计（A1 + D）

> 日期：2026-10-17
> 状态：设计草案，待授权实施
> 关联：docs/superpowers/specs/2026-10-16-kvmem-max-lrfu-pressure-test.md §11

---

## 1. 问题陈述

§11 证伪了「宽度饱和」模型，揭示真正机制：

> 选择 t 决定 t+1 的**输入表示**（不只是候选集）。
> 分数 = 当前 chunk 的 query 对每个块的注意力质量，softmax 在**装配出来的窗口**上归一化；
> 而窗口正是上一轮的 kept 集。耦合走**内容**：q ← 窗口；kbar_sum ← 该块写入时的窗口。

后果：
- B 与 R 都不作用于排序，而作用于**取页轨迹**；
- 召回对 B 非单调（实测 6,6,6,8,7,7,7）；
- LRFU 关档臂读数不干净（prefill.cpp:270 已修 probe 归属，但 round 边界仍按 chunk）；
- 439 评分 chunk 仅对应 89 连续 query span → 历史过快衰减。

## 2. A1：LRFU chunk→round 映射修正

### 现状
- `prefill.cpp:275` 调用 `kvmem_lrfu_round_begin(sequence.lrfu, lane_epochs[lane], retrieval_history)` **每 prefill 一次**。
- `kvmem_lrfu_round_begin` 以 `token` 为键；`kvmem_lrfu_round_add` 校验 `token == round.token`。
- 一个 prefill 内有多个 chunk，但只开一轮 → 439 chunk 压缩成 89 span。

### 设计
- **按检索轮开轮**：检索轮 = 一次 `select_retrieval_pages` 调用（context.cpp:1758 附近）。
- 在 `select_retrieval_pages` 入口处，若 `kvmem_lrfu_enabled()`，调用 `kvmem_lrfu_round_begin`，token 用 `sequence.lrfu.token` 的**检索轮计数**（递增），history 用 `sequence.lrfu.history`。
- `prefill.cpp:275` 处的 `round_begin` 改为**仅初始化**（token=0 → inactive），避免重复开轮；或移除，改由 select_retrieval_pages 触发。
- `kvmem_lrfu_round_add` 的 token 校验不变；round.token 由检索轮计数器维护。

### 验证
- `test_kvmem_lrfu.cpp`：round_begin/round_add 序列 unchanged（单测已覆盖）。
- 新增：探针日志 `round_boundary chunk=%u round=%llu`，确认 chunk→round 映射 1:1 或 N:1（N=检索轮内 chunk 数）。
- 召回 arms 重跑，LRFU 关/开读数对比。

### 回退
- 单独提交，`kvmem_lrfu_enabled()` 默认关，回退只回退该提交。

## 3. D：Stage 5 重新设计

原设计（按分数门控接缝补全 + G4 信号替换）建立在「宽度/完整度权衡」前提上，已被证伪。新设计：

### D1 闭环解耦（核心）
- **问题**：scorer 的 query 窗口 = 上一轮 kept 集；kept 集 = 排序器输出。这是正反馈。
- **方案**：把 query 窗口改为**固定滑动窗口**（最近 N token，N=KV_RETRIEVE 的 1/4），不随 kept 集变化；kept 集只影响候选集，不反向影响 query。这样 B/R/排序轴可独立评估。
- **代码**：`context.cpp` select_retrieval_pages 入口，query 构造处（`q` 的来源）改为固定窗口；原逻辑保留为 `NINFER_KV_QUERY_MODE=kept` 开关（默认 fixed）。

### D2 分数门控接缝补全
- `add_seam_neighbors` 无条件花 2 槽位买邻居；改为仅当邻居页自身分数过阈（或证据跨页）时才花。
- `ops/kvmem/kvmem_page_selection.h` 新增 `add_seam_neighbors_gated(ranked, begin, end, budget, reach, threshold)`。
- 阈值默认 = 切割线分数 × 0.5。

### D3 G4 信号替换
- 新信号：入选页中，邻居恰好排在切割线之下的数量（`neighbor_dropped_count`）。
- `RingBudgetMargin` 扩展字段 `neighbor_dropped`；`ring_measure_margin` 在 binding budget 时计算。

### D4 验证
- 重跑 §11 双稳态 arms（B=16/48/64 × R=0/1 × agg=mean/max × lrfu on/off）。
- 期望：B 单调、R 单调、排序轴符号固定。

## 4. 实施顺序

```
A1 (映射修正) → A2 (probe 断言) → A3 (日志) → C (单测) → D1 (解耦) → D2 (门控补全) → D3 (G4 信号) → D4 (验证) → E (卫生) → F (生产决策)
```

每步：独立提交、默认关、可回退、补测试。

## 5. 生产决策门禁

- 全绿 + 双稳态消除 → 评估翻默认 ON。
- 任一失败 → 保持 ring OFF / RETRIEVE=3072 / NEIGHBOR_PAGES=0。

---

*设计 doc，非实施。实施需另行授权。*
file_path="/home/nnb/projects/ninfer-deploy/docs/superpowers/specs/2026-10-17-kvmem-lrfu-round-design.md"