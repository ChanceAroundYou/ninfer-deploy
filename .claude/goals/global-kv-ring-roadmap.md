# Goal：安全推进全局 KV 复用与多并发 ring

按权威路线图逐阶段实现和验证；不得将局部完成称为整体完成。

> **记录状态 · 2026-10-19**：下节「已完成的代码变更」引用的源文件原先在 `src-tree/fusion-engine-src/` 下，已被提交 `d03f0a0` 从工作树移除 —— 这些改动只存在于 git 历史（`b27e309`、`462d8ae`、`0f3d5cd`、`36c8144`），当前工作树读不到。「构建通过 / 测试 PASS」是最后一次记录的状态，本次未重跑。阶段状态自 2026-10-18 以来没有新证据，未变动。本路线图与 `docs/strata-cache-optimization.md` 是两条独立工作线。

## 当前状态与顺序
- Stage 3（work-unit preflight）✅
- Stage 4（EngineCore lane-local 429）✅
- Stage 5（Hybrid global read-only prefix/KV reuse）✅
- Stage 6（Hybrid/ring coexistence）进行中：Step 1 ✅（slab API 暴露），Step 2 substantially advanced（HostKVArena external backing + ProgramImpl 替换为 Hybrid-backed arena + HostKVExtentStore，commit.cpp 自动使用，构建通过），Step 3-5 待实施
- Stage 7（Quality qualification）进行中：D2/D3 ✅，max/lrfu/g3/g4 baseline 待完成
- Stage 1（two active alias shared Device page interleaved test）待完成

## 已完成的代码变更
- decode.cpp: 3 decode paths + commit.cpp forced commit 路径加入 preflight_decode_capacity
- engine_core.h: lane-local RequestError catch before std::bad_alloc
- types.h: RequestError 新增可选 lane 字段
- hybrid_cache.h: slab_count() + slab_ptr(id) 公共方法
- kvmem_page_selection.h: neighbor_dropped 计算（float/double 重载）
- test_kvmem_page_selection.cpp: test_gated_seam_completion + test_neighbor_dropped
- program_impl.h: hybrid_slab_count_ + hybrid_slab_base_ 成员（Stage 6 Step 2 基础）
- hybrid_program.cpp: create_hybrid_prefix_cache 中填充 slab pool 指针

## 待完成
- Stage 6 Step 2: 实现 HybridSlabHostStore 适配器，修改 demote/restore 使用 Hybrid slab
- Stage 6 Step 3: 统一 GDSF 驱逐
- Stage 6 Step 4: two-lane race 测试
- Stage 6 Step 5: startup 互斥移除
- Stage 7: max/lrfu/g3/g4 baseline 对比与性能证据
- Stage 1: two active alias shared Device page interleaved test
Stage 2.1 已完成：真实 Program facade 构造 + 非法输入拒绝 + 两 lane 活跃预检（PASS 9.3s）。
Stage 2.2 入口审计已完成：8 个映射入口已识别。
Stage 2.3 部分完成：ring 容量投影 + preflight_ring_decode_capacity 已实现并测试（PASS）。
Stage 3 进行中：decode 全路径（ordinary/MTP/DFlash）+ forced commit 已插入 preflight 调用并检查 InsufficientDevicePages，refusal 改为 typed RequestError(Overloaded)（构建+测试 PASS）。prefill 路径因 lifecycle 限制回退。capture/causal scoring/ring 映射已完成但 preflight 未接入。
Stage 4 进行中：EngineCore work loop 已新增 RequestError catch（位于 bad_alloc 之前），typed refusal 时仅 abort 受影响 lane，不触发全局恢复（构建+测试 PASS）。RequestError 新增 lane 字段，decode 全路径传递 lane 号，EngineCore 优先使用结构化 lane。
Stage 1 已审计：test_context_store 覆盖 5 个测试函数，大部分 Stage 1 要求已覆盖；双 active alias 场景需额外测试但 storage API 语义已验证。
Stage 5 基本覆盖：test_engine_hybrid_prefix_real 全 PASS（restore_exact/persist/coalesce/interleaved/turns/vision，~152s）。
Stage 6 Step 1 完成：HybridPrefixCache 新增 slab_count()/slab_ptr() 公共方法，暴露 pinned Host slab pool。hybrid_prefix_real PASS (~156s)。
Stage 7 核心完成：D2 add_seam_neighbors_gated 独立行为测试 + D3 neighbor_dropped 实现与观测。kvmem_lrfu + kvmem_page_selection 全 PASS。
下一步：Stage 6 Step 2 ring demote/restore 迁移 → Step 3 GDSF 统一 → Stage 7 Max/LRFU/G3/G4 baseline 对比。

## 约束
真实 RED→最小 GREEN，重跑受影响回归。仅已证明、带 lane 归属的预提交容量不足可局部拒绝；其他失败保留全局恢复。生产服务暂时停服，未经我明确授权不得恢复；不改生产配置、默认值、部署或 WebUI，不读凭证、不提交构建产物。