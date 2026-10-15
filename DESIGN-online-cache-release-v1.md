# NInfer 在线缓存释放设计稿 v1（隔离侧，待隔离构建验证）

日期：2026-10-14｜作者：Claude 隔离实验｜状态：设计稿，未进生产
约束：生产 `3287635:18471` 只读；GPU0/1 他人进程不动；验证只用 GPU4 隔离实例；`clearkv=restart` 现状不变直到隔离验证通过。

---

## 0. 证据基线（本稿的前提，不是假设）

- `HybridPrefixCache::clear()`（`hybrid_cache.cpp:698`）：同步 `restore_stream` + 全部 `pending.write.done` event，回收 `landing`，释放全部 block 引用 + state image，`index_.reset + emplace` 重建。注释：`Engine-wide recovery` —— 调用者必须已独占 cache 内存。
- `ProgramImpl::fail_all_cleanup()`（`commit.cpp:911`）：abort capture/materialization（含 `synchronize_transfer_streams`）→清 lane→释 continuation/shared-prefix→`hybrid_->clear()`→`rebuild_context_stores()`。注释：`Only valid once no owner, lane or context transaction remains`。
- `engine_core.h:worker_loop:2350`：boundary→prefill/decode 全程持 `execution_mutex_`；`cleanup_program_locked / fail_all_locked / recover_from_oom_locked` 全是 `*_locked`。
- `http_server.cpp:register_routes:468`：有 `/health /metrics /stats /slots /v1/...`，**无 clear/cache/admin 路由**。
- WebUI 现状诚实，无需改文案包装：`web.py:592 clearkv → queue_action('restart')`；`app.js:162` 已是“重启并清空 KV（约40秒不可用）”。
- 隔离实测：117k×2 触发 spill 5679 页 / host 9.35GB，下沉后 100% 召回 0.5s；冷长度→热 queue 曲线 15k→0.9s / 30k→19.9s / 65k→91.5s / 117k→262.2s；65k/140k 断连 1–5s 内 `done cancelled + 499`，无槽泄漏。

## 1. 目标与非目标

目标：不停进程、约秒级把 KV/上下文缓存清到“新进程等价空”，期间可用性损失可预期、可观测。
非目标：单会话/单前缀定向清除（需 session-key 作用域，后续 v2）；磁盘 tier 清理；改调度公平性（另案）。

## 2. 命令队列（新增，唯一跨线程入口）

HTTP 线程**永不**直接碰 cache，只入队。复用现有 `queue_mutex_ + queue_cv_` 模式，另起一条 admin 队列避免与推理 pending 互堵：

```cpp
enum class CacheScope : uint32_t {
  DeviceMainKV  = 1u << 0,  // text_kv_pages / addresses
  BackendKV     = 1u << 1,  // backend_kv_cache（dflash2 相关）
  HostKV        = 1u << 2,  // host_kv_extents / arena
  HostState     = 1u << 3,  // state_store + host_state_images
  SharedPrefix  = 1u << 4,  // shared_prefix_slots/states
  Continuation  = 1u << 5,  // continuation_slots/states
  Anchors       = 1u << 6,  // long_anchors / auto-prefix-grid
  All           = 0x7Fu,
};
enum class ClearMode : uint8_t {
  TryIdle,       // 忙则拒绝（默认，最安全）
  DrainAndFail,  // 在途按 Overloaded 失败后再清（等价软重启语义）
  AbortAndClear, // 含 transaction abort（复用 fail_all 路径，需显式确认）
};
struct CacheClearCmd {
  uint64_t id; CacheScope scope; ClearMode mode;
  bool dry_run; std::string requested_by; TimePoint deadline;
};
```

队列：`std::deque<CacheClearCmd> admin_queue_` + `admin_cv_`，HTTP 入队后即返 `202 {op_id}`，轮询查结果。worker 在每次 boundary 顶部（`snapshot_cancellations` 同一位置）顺带取一条执行，保证与 cancel/admission 同一互斥粒度。

## 3. 语义定义（三种模式，三条路，不混）

| 模式 | 在途请求 | transaction | 行为 | 适用 |
|---|---|---|---|---|
| `TryIdle`（默认） | 任一 lane/materializing/pending 非空→拒绝 `409 busy` | 非 monostate→拒绝 | 只清，不碰请求 | 深夜低峰、基准重置 |
| `DrainAndFail` | 在途全部 `force_complete_error(Overloaded, retryable)`，pending 同理 | 先 abort（同 fail_all 顺序） | 清，调用方计数 blast radius | 高负载必须清时，显式承担失败数 |
| `AbortAndClear` | 同上 | `transfer_submitted` 则先 `synchronize_transfer_streams` 再 abort | 完整 `fail_all_cleanup` 复用 | 疑似 transaction 卡住、host 拷贝在飞时 |

`scope != All` 时只释放对应层并重建其索引，`ResourceManager::clear_after_program_cleanup` 照常对账；`scope == All` 即 `fail_all_cleanup(Failure)` 的非故障复用（不记 fatal 诊断，只记 admin 操作日志）。

## 4. 前置条件（worker 持 `execution_mutex_` 下逐项断言，任一不满足按模式拒绝/中止）

1. `materializing_ == null`（TryIdle；另两模式先 reset + force_complete）。
2. `slots_[lane] == null ∀lane`（TryIdle；另两模式先 fail）。
3. `context_transaction_ == monostate`，否则按模式 abort；`transfer_submitted` 必须先同步 transfer/compute streams。
4. `disk_kv->wait_idle()`（复用 `rebuild_context_stores` 前置）。
5. 在飞拷贝：`compute_streams + transfer_streams` 全同步；`restore_stream + pending.done` 由 `hybrid_->clear()` 内部再同步一次。
6. 清后断言：`context_stores_idle()` 或 `rebuild_context_stores()` 成功；`advance_resource_revision()`；`publish_runtime_stats()`。
7. 任一步抛异常→按现有 `fail_all_locked` 语义转故障路径，**不清一半**（注释已要求 worker 持锁跨清理）。

## 5. HTTP API（新增，隔离构建先加，生产后加）

```
POST /v1/admin/cache/clear
  Body: {"scope":["device_main","host_kv","host_state","shared_prefix","continuation","anchors"],
         "mode":"try_idle|drain_and_fail|abort_and_clear", "dry_run":false}
  → 202 {"op_id":123, "state":"queued"}
  → 409 {"error":"busy", "in_flight":2, "materializing":false}（TryIdle 忙时）
GET /v1/admin/cache/clear/{op_id}
  → {"state":"done", "kv_before":363648, "kv_after":0, "host_before":9350995968,
     "host_after":0, "duration_ms":812, "failed_requests":0}
```

鉴权复用现有 `api_key` 门；`dry_run=true` 只返回“若执行会走哪条路、影响多少 lane”，不清。

## 6. 可观测（随端点一起加）

- 计数：`cache_clear_requests_total{mode,scope,result}`，`cache_clear_failed_requests_total`。
- 直方图：`cache_clear_duration_seconds`。
- `/stats` 快照前后：`device_main_kv_pages / host_kv_bytes / pressure{spill_pages,demoted,degraded,evicted,dropped}`。
- 日志一行：`admin cache-clear op=123 mode=try_idle scope=all kv 363648→0 host 9.3G→0 812ms failed=0`，与 `req# done` 同格式可 grep。

## 7. WebUI（验证通过前不动，验证后这样改）

- 保留“重启并清空 KV（约40秒不可用）”。
- 新增“在线清空（仅空闲，忙则拒绝）”→ `mode=try_idle`；高级折叠页放 `drain/abort` 两档，二次确认框写明“将失败 N 个在途请求”。
- 按钮旁显示最近一次 op 结果（before→after、耗时、failed 数），不再只有一个 toast。

## 8. 隔离验证矩阵（GPU4，逐项过才谈生产）

1. idle 清：`kv 227k→0, host X→0`，召回同 prompt=0%（证明真清），`requests_processing` 全程 0。
2. 满载 TryIdle：冷 117k prefill 中发清→409 busy，无状态变化。
3. DrainAndFail：冷 65k + 热排队时发清→在途按 Overloaded 失败可计数，清后新请求 0% 冷起。
4. AbortAndClear：构造 transfer 在飞（大 host 召回中）发清→同步后清，无一半状态。
5. 并发请求：清操作期间新提交排队不插队、不丢（pending 语义与现有 admission 一致）。
6. 性能：清操作 p50/p99 耗时，`restart 40s` vs 在线清 `<2s idle / <5s drain` 对照。
7. 回归：现有形状探针（stable 100% / append 99.9% / tools+1 0%）清后重跑，确认索引重建无损坏。

## 9. 风险与拒绝理由

- 在高负载下 TryIdle 永远 409：是设计不是 bug，面板必须明示，不要静默转 drain。
- 清后 llmux 重试叠加→ thundering prefill：需配合 admission backoff 与客户端退避，另案。
- 单调 `hybrid_->clear()` 而不走 stores/ResourceManager 对账：已在源码注释层面否决，本稿不采用。

## 10. 落地顺序

1. `engine_core.h`：类型 + 队列 + worker hook（只隔离分支）。
2. `http_server.cpp`：两路由 + 鉴权复用。
3. stats/metrics/日志。
4. GPU4 隔离构建跑 §8 全矩阵。
5. 全绿后才出生产端点 + WebUI 双按钮；`clearkv=restart` 在此之前一行不改。
