
## KVMem 环关键事实（从 AGENTS.md 迁入）

- 环只在 `--max-concurrency 1` + `NINFER_KV_REUSE_HOSTBACKED=1` 时激活
- `startup.cpp` 在 `ring_requested() && max_concurrency > 1` 时直接抛异常
- 生产用 `--max-concurrency 4`，不跑环
- 环的 KV 页循环：VRAM ↔ pinned host RAM（`HostKVArena` → `PinnedHostBuffer` = `cudaHostAlloc`），不涉及磁盘
- 磁盘 tier（`--disk-kv-path`）是正交的持久化层，仅 `--disk-kv-restore` 时查询
- 召回/预算调参结论以 `docs/superpowers/specs/` 与本文件为准
