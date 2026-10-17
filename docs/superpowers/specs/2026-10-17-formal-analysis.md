# KVMem Max/LRFU 正式分析报告（2026-10-17）

## 1. 验证范围
- Stage C host tests：3/3 通过（lrfu / page_selection / score_binding）
- 设备 B 实例 smoke：启动 + 真实请求 200 OK + counter 端点响应
- A1 代码变更已推送，实例运行正常（无崩溃）

## 2. A1 变更影响
- `kvmem_lrfu_round_begin` 从 prefill（每 chunk 一次）移至 `select_retrieval_pages`（每检索轮一次）
- history 由 prefill seed（token=0，inactive），select_retrieval_pages 开 round 时使用
- 预期：历史衰减粒度从 chunk 级变为检索轮级，LRFU 关档臂读数更干净
- 实例 smoke 验证：A1 变更不破坏服务启动与请求处理

## 3. D1-D3 实施状态
- D1：已满足（query 使用固定 256-token 窗口，不依赖 kept 集）
- D2：score-gated seam completion 已实现（`add_seam_neighbors_gated`）
- D3：`neighbor_dropped` 字段已加入 `KvMemBudgetMargin`
- D4：完整 arm 重跑需冷启动授权（§11 矩阵），本次未执行

## 4. 生产决策
- 保持 ring OFF / RETRIEVE=3072 / NEIGHBOR_PAGES=0
- Max/LRFU/G3/G4 保持 gated OFF
- 翻默认值条件：D4 验证通过（双稳态消除）

## 5. 结论
- 所有代码变更编译通过，host tests 3/3 通过
- 设备 B 实例运行正常
- 生产配置不变，等待 D4 arm 验证

## D4 验证结果（2026-10-17 追加）

运行环境：设备 B GPU4，冷启动，A1/D1-D3 新 binary（sha 08eaf5454b2d...）。

| 臂 | B(页) | R | agg | recall |
|---|---|---|---|---|
| b16_r0_mean | 16 | 0 | mean | 6/8 |
| b64_r0_mean | 64 | 0 | mean | 8/8 |

与 §11 历史读数一致（B=16 6/8、B=64 8/8），新代码未引入回归；双稳态 across B/R/agg/lrfu 全矩阵仍需进一步运行，当前证据不足以翻默认值。

### 生产决策（更新）
保持 ring OFF / RETRIEVE=3072 / NEIGHBOR_PAGES=0 / Max/LRFU/G3/G4 gated OFF。翻默认值条件未满足。
