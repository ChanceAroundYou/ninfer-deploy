# B06: Per-Lane Mean-K Index — 完整改动方案

## 背景

B06 是 NInfer 中已注册的已知限制：mean-K 索引（用于 KVMem 内容打分）是进程级单例。
当 `--max-concurrency > 1` 时，并发序列共享同一索引，导致静默数据损坏（一个序列的历史块被另一个序列的 query 排序）。

**目标**：打破此限制，使 concurrency 4 + KVMem 可同时使用。

## 模型常量（Qwen3.8-27B）

| 常量 | 值 | 来源 |
|------|-----|------|
| `num_hidden_layers` | 64 | `config.json` |
| `full_attention_layers` | 16 | `full_attention_interval=4` → 64/4 |
| `num_key_value_heads` | 4 | `config.json` |
| `head_dim` | 256 | `config.json` |
| `kPagedKVPageSize` | 64 | `core/paged_kv_cache.h:19` |
| `kMaximumConcurrency` | 8 | `include/ninfer/types.h:22` |
| `kMeanKMaxLayers` | 64 | `mean_k_index.h:106` |
| `kv_size` | 1024 | `head_dim * kv_heads = 256 * 4` |

## 内存预算

### MeanKIndex 单 lane 字节数

```
mean_k_index_bytes(layers=16, blocks=B, kv_heads=4, head_dim=256)
  = layers * blocks * kv_heads * head_dim * sizeof(float)   (sums)
  + layers * blocks * sizeof(float)                         (counts)
  = 16 * B * 4 * 256 * 4 + 16 * B * 4
  = 16 * B * 4096 + 64 * B
  = B * 65600 字节
```

以 `max_context=65536` 为例：
- `capacity_blocks = 65536 / 64 = 1024`
- 单 lane：`1024 * 65600 = 65.6 MB`
- 8 lanes：`8 * 65.6 = 524.8 MB`

以 `kv-capacity=17920`（当前生产）为例：
- `capacity_blocks = 17920 / 64 = 280`
- 单 lane：`280 * 65600 = 17.9 MB`
- 8 lanes：`8 * 17.9 = 143.2 MB`

### RawKShadowHarvest 单 lane 字节数

```
staging_bytes = layers * kv_size * tokens * sizeof(BF16)
  = 16 * 1024 * 1024 * 2
  = 32 MB (device) + 32 MB (pinned host)
```

- 8 lanes：`8 * 64 = 512 MB`

### 总计（8 lanes, max_context=65536）

| 组件 | 单 lane | 8 lanes |
|------|---------|---------|
| MeanKIndex | 65.6 MB | 524.8 MB |
| RawKShadowHarvest (device + host) | 64 MB | 512 MB |
| **合计** | **129.6 MB** | **1036.8 MB** |

## 5 个进程级单例

### 1. MeanKIndex

- **文件**：`src/ops/kvmem/mean_k_index.cpp:240`
- **单例**：`static MeanKIndex* installed = nullptr`
- **调用点**：`text.cpp:1810`（round_begin）、`text.cpp:1835`（round_end）、`kvmem_score.h:415`（accumulate）
- **契约**：首次调用安装，后续返回同一对象；OFF 时返回 nullptr

### 2. KvMemScoreProbe

- **文件**：`src/ops/kvmem/kvmem_score.h:155`
- **单例**：`static KvMemScoreProbe probe`
- **调用点**：`kvmem_score_selected_blocks()`（context.cpp:1734）、`kvmem_score_host_scores()`（context.cpp:1735）、`kvmem_score_scored_blocks()`（context.cpp:1736）、`kvmem_score_begin`（text.cpp:1812）、`kvmem_score_finish`（text.cpp:1845）、`kvmem_score_accumulate`（attention.cpp:181）、`kvmem_score_set_query_span`（prefill.cpp:1202, program_impl.cpp:703）、`kvmem_score_set_chunk_origin`（text.cpp:1795）、`kvmem_score_set_harvest_layers`（text.cpp:1842）

### 3. KvmemHook

- **文件**：`src/models/qwen3_5/execution/attention.cpp:123`
- **单例**：`static KvmemHook hook`
- **调用点**：`text_set_kvmem_hook`（text.cpp:946 武装、text.cpp:1849 清除）、`kvmem_harvest_armed`（attention.cpp:133）、`kvmem_harvest_pre_rope`（attention.cpp:138, 222）

### 4. RawKShadowHarvest

- **文件**：`src/ops/kvmem/raw_k_harvest.cpp:327`
- **单例**：`static RawKShadowHarvest* harvest = nullptr`
- **调用点**：`text.cpp:1805`（round_begin 安装到 TextContext）

### 5. RawKBlockArena

- **文件**：`src/ops/kvmem/raw_k_block_arena.h:425`
- **单例**：`static RawKBlockArena* instance = nullptr`
- **调用点**：`raw_k_block_arena_store_round`（无外部调用者 — 死代码）
- **状态**：死代码，但单例模式与 harvest 一致

## Lane 传播路径

### 关键 key：`SequenceState::lane`

- **定义**：`program_impl.h:426` — `std::uint32_t lane = 0;`
- **赋值点**：`commit.cpp:119` — `sequence.lane = lane;`
- **稳定性**：序列生命周期内稳定，bounded by `kMaximumConcurrency`
- **正确性**：`state_source_slot` 是 StateImageStore 物理槽索引（fork 时变化），**不是** lane id

### 传播链

```
SequenceState::lane
  → PrefillContext::lane (需新增字段)
    → configure_text_card(..., lane, ...) (需新增参数)
      → TextContext::kvmem_lane_ (需新增成员)
        → kvmem_score_begin(..., lane, ...)
        → kvmem_score_finish(..., lane, ...)
        → kvmem_score_accumulate(..., lane, ...)
        → kvmem_score_set_query_span(..., lane, ...)
        → kvmem_score_set_chunk_origin(..., lane, ...)
        → kvmem_score_set_harvest_layers(..., lane, ...)
        → mean_k_index_for(..., lane, ...)
        → raw_k_shadow_harvest_for(..., lane, ...)
        → kvmem_hook() → per-lane
```

### 消费端（decode）

- `select_retrieval_pages(sequence, ...)` 在 `context.cpp:1714`
- 调用点：`context.cpp:2490` — `sequence` 参数可用
- 读取：`kvmem_score_selected_blocks()`、`kvmem_score_host_scores()`、`kvmem_score_scored_blocks()`
- **改动**：这三个函数需要 `lane` 参数，从 per-lane probe 读取

## kvmem_publish_query_span 挑战

### 问题

`kvmem_publish_query_span` 在 `frontend.cpp:1103` 调用 — 请求解析层**无 lane 概念**。
query span 在 lane 分配前发布到进程级 probe。

### 现有两条路径

1. **Path A（frontend）**：`frontend.cpp:1103` → `kvmem_publish_query_span` → `kvmem_score_set_query_span`（进程级，无 lane）
2. **Path B（prefill driver）**：`prefill.cpp:1202` → `kvmem_score_set_query_span`（进程级，但有 `staged.prompt` 是 per-request 的）

Path A 的存在理由（注释原文）：
> "Publishing HERE, in the request path, also covers prefill shapes that never pass through program/prefill.cpp's driver (e.g. the forced-token suffix prefill in transactions/commit.cpp)."

### 设计方案

**核心洞察**：Path B 已经有 per-request 数据（`staged.prompt.query_span_begin/end`），且在所有 prefill 路径中都可用。Path A 是冗余的（Path B 用相同值覆盖）。

**方案：移除 frontend 的进程级发布，改为 per-lane 发布**

1. **删除** `frontend.cpp:1103` 的 `kvmem_publish_query_span` 调用
2. **删除** `kvmem_publish_query_span` 函数（`prefill.cpp:1459-1461`）和声明（`prepared_prompt.h:30`）
3. **在所有 prefill 路径中添加 per-lane 发布**：
   - `prefill.cpp:1202`（staged prompt 循环）— 用 `sequence.lane`
   - `program_impl.cpp:703`（causal score）— 用 `sequence.lane`（需确认）
   - `commit.cpp:367`（forced-token suffix）— 用 `sequence.lane`（需确认）
4. **`kvmem_score_set_query_span` 签名加 `lane` 参数**

### 对 "never pass through prefill driver" 路径的影响

- 这些路径将**没有** query span → scorer 回落尾窗规则（`QUERY_TAIL`）
- 这是**可接受的降级**：尾窗规则是已验证的负控，不是正确性问题
- 注释中提到的 forced-token suffix prefill 在 `commit.cpp:367`，该处也有 `sequence.lane` 可用

## 改动清单

### 第一层：ops/kvmem 组件（5 个单例 → per-lane 数组）

#### 1. `mean_k_index.cpp` / `mean_k_index.h`

**改动**：
- `mean_k_index_for` 签名加 `lane` 参数
- `static MeanKIndex* installed` → `static std::array<MeanKIndex*, kMaximumConcurrency> installed`
- 首次调用时 `new MeanKIndex(...)` 并 `zero()`
- 后续调用按 lane 索引返回
- 几何不一致时 throw（同现有契约）

**调用点更新**：
- `text.cpp:1810` — `mean_k_index_for(layers, kv_heads, head_dim, capacity_blocks, lane)`
- `text.cpp:1835` — 同上
- `kvmem_score.h:415` — 同上（在 accumulate 中）

#### 2. `kvmem_score.h`

**改动**：
- `kvmem_score_probe()` → `kvmem_score_probe(lane)` — 返回 per-lane probe
- 所有 `kvmem_score_*` 函数签名加 `lane` 参数：
  - `kvmem_score_begin(..., lane)`
  - `kvmem_score_accumulate(..., lane)`
  - `kvmem_score_finish(..., lane)`
  - `kvmem_score_set_query_span(..., lane)`
  - `kvmem_score_set_chunk_origin(..., lane)`
  - `kvmem_score_set_harvest_layers(..., lane)`
  - `kvmem_score_fail(..., lane)` — 需加 lane 以操作正确 probe
- 访问器加 `lane`：
  - `kvmem_score_selected_blocks(lane)`
  - `kvmem_score_host_scores(lane)`
  - `kvmem_score_scored_blocks(lane)`
- `kvmem_score_dump_if_requested` — 死代码，可不改（或加 lane 保持一致）

**probe 存储**：
```cpp
inline KvMemScoreProbe& kvmem_score_probe(std::uint32_t lane) {
    static std::array<KvMemScoreProbe, kMaximumConcurrency> probes;
    return probes[lane];
}
```

#### 3. `attention.cpp`

**改动**：
- `kvmem_hook()` → `kvmem_hook(lane)` — 返回 per-lane hook
- `kvmem_harvest_armed(lane)` — 加 lane 参数
- `kvmem_harvest_pre_rope(..., lane)` — 加 lane 参数
- `text_set_kvmem_hook(..., lane)` — 加 lane 参数

**hook 存储**：
```cpp
KvmemHook& kvmem_hook(std::uint32_t lane) {
    static std::array<KvmemHook, kMaximumConcurrency> hooks;
    return hooks[lane];
}
```

#### 4. `raw_k_harvest.cpp`

**改动**：
- `raw_k_shadow_harvest_for` 签名加 `lane` 参数
- `static RawKShadowHarvest* harvest` → `static std::array<RawKShadowHarvest*, kMaximumConcurrency> harvest`
- 首次调用时 `new RawKShadowHarvest(...)`
- 后续调用按 lane 索引返回

#### 5. `raw_k_block_arena.h`

**改动**：
- `raw_k_block_arena_for` 签名加 `lane` 参数
- `static RawKBlockArena* instance` → per-lane 数组
- 死代码，但保持一致性

### 第二层：TextContext（lane 成员 + 传递）

#### `text.h`

**改动**：
- 新增成员：`std::uint32_t kvmem_lane_ = 0;`
- 新增 setter：`void set_kvmem_lane(std::uint32_t lane) { kvmem_lane_ = lane; }`

#### `text.cpp`

**改动**：
- `kvmem_round_begin()`：
  - `kvmem_score_set_chunk_origin(..., kvmem_lane_)`
  - `raw_k_shadow_harvest_for(..., kvmem_lane_)`
  - `mean_k_index_for(..., kvmem_lane_)`
  - `kvmem_score_begin(..., kvmem_lane_)`
- `kvmem_round_end()`：
  - `mean_k_index_for(..., kvmem_lane_)`
  - `kvmem_score_set_harvest_layers(..., kvmem_lane_)`
  - `kvmem_score_finish(..., kvmem_lane_)`
- `text_set_kvmem_hook` 调用点（text.cpp:946）：
  - `execution::text_set_kvmem_hook(fidx, T, ph == Phase::Prefill ? kvmem_harvest_ : nullptr, kvmem_lane_)`

### 第三层：PrefillContext + configure_text_card（lane 注入）

#### `context.h`

**改动**：
- `PrefillContext` 新增字段：`std::uint32_t lane = 0;`
- `configure_text_card` 签名加 `lane` 参数：
  ```cpp
  void configure_text_card(TextContext& card, const ExecutionCore& execution,
                           const ops::SamplingConfig* sampling, std::int32_t state_source_slot,
                           std::int32_t state_destination_slot, std::uint32_t mtp_proposal_extent,
                           std::uint32_t lane);
  ```

#### `prefill.cpp`

**改动**：
- `configure_text_card` 实现（:49）— 加 `lane` 参数，调用 `card.set_kvmem_lane(lane)`
- `prefill_text_chunk`（:77）— 传 `state.lane`
- `prefill_multimodal_chunk`（:114）— 传 `state.lane`
- staged prompt 循环（:1202）— `kvmem_score_set_query_span(..., sequence.lane)`

#### `mtp.cpp`

**改动**：
- `configure_text_card` 调用（:31）— 传 `state.lane`

### 第四层：PrefillContext 构造点（lane 初始化）

#### 构造点 1：`prefill.cpp:1206`（staged prompt 循环）

**现状**：
```cpp
execution::PrefillContext schedule_state{..., state_slot, state_slot, 0, nullptr};
```

**改动**：加 `sequence.lane`：
```cpp
execution::PrefillContext schedule_state{..., state_slot, state_slot, 0, nullptr,};
// PrefillContext 构造后：
schedule_state.lane = sequence.lane;
```

**注意**：此处使用位置初始化，加字段时需确认字段顺序。

#### 构造点 2：`program_impl.cpp:683`（causal score）

**现状**：
```cpp
execution::PrefillContext schedule_state{..., state_slot, state_slot, 0, nullptr};
```

**改动**：同上，加 `sequence.lane`（需确认 `sequence` 变量名）。

#### 构造点 3：`commit.cpp:367`（forced-token suffix）

**现状**：
```cpp
execution::PrefillContext schedule_state{..., selectors.source, selectors.destination, 0, nullptr};
```

**改动**：同上，加 `sequence.lane`。

### 第五层：decode 消费端

#### `context.cpp`

**改动**：
- `select_retrieval_pages`（:1714）— 已有 `sequence` 参数
- 内部调用：
  - `kvmem_score_selected_blocks(sequence.lane)`
  - `kvmem_score_host_scores(sequence.lane)`
  - `kvmem_score_scored_blocks(sequence.lane)`

### 第六层：frontend query span 发布

#### `frontend.cpp`

**改动**：
- **删除** :1103 的 `kvmem_publish_query_span(result.query_span_begin, result.query_span_end);`

#### `prefill.cpp`

**改动**：
- **删除** `kvmem_publish_query_span` 函数定义（:1459-1461）

#### `prepared_prompt.h`

**改动**：
- **删除** `kvmem_publish_query_span` 声明（:30）

### 第七层：startup 守卫

#### `startup.cpp`

**改动**：
- **删除** :1290-1297 的 B06 守卫：
  ```cpp
  if (inputs.max_concurrency > 1 && ops::detail::kvmem_shadow_enabled()) {
      throw std::invalid_argument("KVMem content scoring requires --max-concurrency 1");
  }
  ```
- **保留** :1269 的 ring 守卫（ring 仍需 concurrency 1）

## 探针设计

### 目的

验证 `SequenceState::lane` 在 prefill→decode 边界稳定，且 per-lane 索引不串。

### 探针点

1. **lane 赋值探针**（commit.cpp:119）
   - 打印 `sequence.lane` 和 `continuation_index`
   - 验证 lane 在 [0, kMaximumConcurrency) 范围内

2. **PrefillContext lane 探针**（prefill.cpp 构造点）
   - 打印 `schedule_state.lane` 和 `state_source_slot`
   - 验证 lane 与 sequence.lane 一致

3. **mean_k_index_for lane 探针**（mean_k_index.cpp:240）
   - 打印 `lane` 和 `installed[lane]` 指针
   - 验证不同 lane 得到不同指针

4. **kvmem_score_probe lane 探针**（kvmem_score.h:155）
   - 打印 `lane` 和 probe 地址
   - 验证不同 lane 得到不同 probe

5. **decode 消费端探针**（context.cpp:1734）
   - 打印 `sequence.lane` 和 `kept.size()`
   - 验证 kept 是 per-lane 的

### 验证方法

1. 并发 2 路请求，各发不同 query
2. 检查每路的 SELECT 日志只包含自己的块
3. 检查 decode 端的 kept 只来自自己 prefill 的打分

## 开关与负控

### 现有开关

| 开关 | 默认 | 作用 |
|------|------|------|
| `NINFER_TERNARY_KVMEM` | OFF（除非 ring ON） | 大开关：harvest + index |
| `NINFER_TERNARY_KVMEM_SCORE` | OFF（除非 ring ON） | 打分器 |
| `NINFER_TERNARY_KVMEM_RAWK_ARENA` | OFF | arena（死代码） |
| `NINFER_KV_WINDOW` | 无 | ring（需 concurrency 1） |

### 负控

- `NINFER_TERNARY_KVMEM=0` → harvest/index/scorer 全部 OFF
- `NINFER_TERNARY_KVMEM_SCORE=0` → 仅 scorer OFF
- `--max-concurrency 1` → 单 lane，per-lane 退化为单例

### 新增开关（可选）

- `NINFER_KV_PER_LANE=0` → 强制使用进程级单例（回退路径）
- 用于 A/B 对比和故障排查

## 风险与缓解

### 风险 1：内存增长

- **描述**：8 lanes 增加 ~1 GB device + host
- **缓解**：启动时打印每 lane 字节数和总字节数；`kv-capacity` 不变时自动缩减 `capacity_blocks`

### 风险 2：lane 回收后索引残留

- **描述**：lane 回收后，旧 lane 的 MeanKIndex 仍持有历史块
- **缓解**：lane 回收时不清零（新序列会从零开始 append）；或提供 `reset_lane(lane)` 接口

### 风险 3：query span 跨 lane 残留

- **描述**：Path A 删除后，某些 prefill 路径可能没有 query span
- **缓解**：回落尾窗规则（已验证的负控）；在所有 prefill 路径中加 per-lane 发布

### 风险 4：commit.cpp / program_impl.cpp 的 sequence 变量名

- **描述**：需确认这两个构造点中 `sequence` 变量的实际名称
- **缓解**：实现时 grep 确认

## 实现顺序

1. **Phase 1**：ops/kvmem 组件 per-lane 化（5 个单例）
   - mean_k_index.cpp/h
   - kvmem_score.h
   - attention.cpp
   - raw_k_harvest.cpp
   - raw_k_block_arena.h

2. **Phase 2**：TextContext lane 成员 + kvmem_round_begin/end 更新

3. **Phase 3**：PrefillContext + configure_text_card lane 注入

4. **Phase 4**：PrefillContext 构造点 lane 初始化

5. **Phase 5**：decode 消费端 per-lane 读取

6. **Phase 6**：frontend query span 发布移除

7. **Phase 7**：startup 守卫移除

8. **Phase 8**：探针添加 + 验证

## 文件:行号 索引

| 文件 | 行号 | 内容 |
|------|------|------|
| `mean_k_index.cpp` | 240 | `mean_k_index_for` 单例 |
| `mean_k_index.h` | 254 | `mean_k_index_for` 声明 |
| `mean_k_index.h` | 112 | `mean_k_index_bytes` |
| `mean_k_index.h` | 106 | `kMeanKMaxLayers` |
| `kvmem_score.h` | 155 | `kvmem_score_probe()` 单例 |
| `kvmem_score.h` | 221 | `kvmem_score_begin` |
| `kvmem_score.h` | 296 | `kvmem_score_accumulate` |
| `kvmem_score.h` | 465 | `kvmem_score_finish` |
| `kvmem_score.h` | 198 | `kvmem_score_set_query_span` |
| `kvmem_score.h` | 206 | `kvmem_score_set_chunk_origin` |
| `kvmem_score.h` | 191 | `kvmem_score_set_harvest_layers` |
| `kvmem_score.h` | 210 | `kvmem_score_fail` |
| `kvmem_score.h` | 163 | `kvmem_score_selected_blocks` |
| `kvmem_score.h` | 171 | `kvmem_score_host_scores` |
| `kvmem_score.h` | 176 | `kvmem_score_scored_blocks` |
| `attention.cpp` | 123 | `kvmem_hook()` 单例 |
| `attention.cpp` | 133 | `kvmem_harvest_armed` |
| `attention.cpp` | 138 | `kvmem_harvest_pre_rope` |
| `attention.cpp` | 186 | `text_set_kvmem_hook` |
| `attention.cpp` | 222 | `kvmem_harvest_pre_rope` 调用 |
| `raw_k_harvest.cpp` | 327 | `raw_k_shadow_harvest_for` 单例 |
| `raw_k_harvest.h` | 238 | `raw_k_shadow_harvest_for` 声明 |
| `raw_k_block_arena.h` | 425 | `raw_k_block_arena_for` 单例 |
| `raw_k_block_arena.h` | 453 | `raw_k_block_arena_store_round` |
| `text.h` | 281 | `kvmem_harvest_` 成员 |
| `text.h` | 282 | `kvmem_round_width_` 成员 |
| `text.cpp` | 946 | `text_set_kvmem_hook` 调用 |
| `text.cpp` | 1786 | `kvmem_round_begin` |
| `text.cpp` | 1824 | `kvmem_round_end` |
| `text.cpp` | 1795 | `kvmem_score_set_chunk_origin` 调用 |
| `text.cpp` | 1810 | `mean_k_index_for` 调用 |
| `text.cpp` | 1812 | `kvmem_score_begin` 调用 |
| `text.cpp` | 1835 | `mean_k_index_for` 调用 |
| `text.cpp` | 1842 | `kvmem_score_set_harvest_layers` 调用 |
| `text.cpp` | 1845 | `kvmem_score_finish` 调用 |
| `text.cpp` | 1849 | `text_set_kvmem_hook` 清除 |
| `context.h` | 50 | `PrefillContext` 定义 |
| `context.h` | 60 | `state_source_slot` 字段 |
| `context.h` | 144 | `configure_text_card` 声明 |
| `prefill.cpp` | 49 | `configure_text_card` 实现 |
| `prefill.cpp` | 77 | `configure_text_card` 调用 |
| `prefill.cpp` | 114 | `configure_text_card` 调用 |
| `prefill.cpp` | 1202 | `kvmem_score_set_query_span` 调用 |
| `prefill.cpp` | 1459 | `kvmem_publish_query_span` 定义 |
| `prefill.cpp` | 1206 | PrefillContext 构造（staged） |
| `mtp.cpp` | 31 | `configure_text_card` 调用 |
| `program_impl.cpp` | 683 | PrefillContext 构造（causal score） |
| `program_impl.cpp` | 703 | `kvmem_score_set_query_span` 调用 |
| `commit.cpp` | 367 | PrefillContext 构造（forced-token） |
| `commit.cpp` | 119 | `sequence.lane = lane` |
| `program_impl.h` | 426 | `SequenceState::lane` |
| `context.cpp` | 1714 | `select_retrieval_pages` |
| `context.cpp` | 1734 | `kvmem_score_selected_blocks` 调用 |
| `context.cpp` | 1735 | `kvmem_score_host_scores` 调用 |
| `context.cpp` | 1736 | `kvmem_score_scored_blocks` 调用 |
| `context.cpp` | 2490 | `select_retrieval_pages` 调用 |
| `frontend.cpp` | 1103 | `kvmem_publish_query_span` 调用 |
| `prepared_prompt.h` | 30 | `kvmem_publish_query_span` 声明 |
| `prepared_prompt.h` | 186 | `query_span_begin` 字段 |
| `prepared_prompt.h` | 187 | `query_span_end` 字段 |
| `startup.cpp` | 1269 | ring 守卫 |
| `startup.cpp` | 1290 | B06 守卫 |
| `startup.cpp` | 48 | `ring_requested` |
| `kvmem_shadow.h` | 73 | `kvmem_shadow_enabled` |
| `kvmem_score.h` | 70 | `kvmem_score_enabled` |
| `raw_k_block_arena.h` | 70 | `kvmem_rawk_arena_enabled` |
| `core/paged_kv_cache.h` | 19 | `kPagedKVPageSize` |
| `include/ninfer/types.h` | 22 | `kMaximumConcurrency` |
DOCEOF"