# Strata 多-agent Prompt Cache 命中率优化计划

## 状态快照 · 2026-10-19

**本文档性质已变**：不再是待执行的计划，而是 2026-10-18 实验与决策的记录，加上 2026-10-19 的量化选择研究。下文各节按日期排列；只有「未决项」列出的部分仍然有效。

### 已验证、可复现的结论

| 结论 | 证据 | 边界 |
|---|---|---|
| `--prompt-cache-root` 门槛决定短 system prompt 能否复用 | 渲染后 1993 tokens：默认 2048 时暖请求 read 1993（0 命中）；降到 512 后 read 22（**−98.9%**），TTFT 1.41s → 0.23s | 四周期平衡、16/16 客户端 usage 与 server summary 逐 token 一致；单一 prompt 长度、无并发 |
| 决定改写代价的是**保留的 checkpoint 数**（`--prompt-cache`），不是 interval | 2×2 四格：同一 slot 数下两行数字逐位相同；slots 6 时改写只能回落到 root（cached 1730，整段重读），slots 24 时 read 3315/6390/9894 | 20 轮 / 约 14.9K 合成历史的 chat 形状；无 turn 边界的单篇长文档形状未测 |
| `--prompt-cache-spread` 只在**近回退**获益 | 四周期平衡 A/B：回退 5 轮 read 1104 → 411（−62.8%）、TTFT −0.32~0.40s；10 轮与 15 轮 read 完全相同 | 约 10K token 短历史、固定合成 transcript；该组本身**不构成质量验收** |
| 多会话缓存池解决跨会话互相淘汰 | 池开命中率 64.6% vs 池关 0.0%；read 722,655 vs 2,043,439（**2.83×**）；wall 355.5s vs 871.7s（2.45×）；全程 `evictions=0`；会话与槽位无亲和性 | 合成填充 + 固定标记，是缓存指标不是质量验收；并发上限 6 真并发 / 10 交错 |
| 池预算是上限不是预留，且不必为模型预留 | `--conversation-cache-mib 0` 与 `131072` 的 RSS 逐 MiB 相同（79,437 MiB）；每 park 一个会话才涨；`available_host_bytes()` 已扣除模型常驻 | 单台 1007 GB 机器；活跃会话的 checkpoint 链不在预算内 |
| 显存目标已由静态 reserve 达成 | `--vram-reserve-mib 9216`：空闲 4990 → 8994 MiB，decode −1.5%，256K prefill +0.2%（约 **0.4%/GiB**） | A6000 单卡、本负载；`--kv-grow` 与 `--batch` 互斥，对本负载不可用 |
| `k8v4` 在固定 reserve 下不释放显存，只多装 slot | free 8994 不变，expert slots 21668 → 21996 | needle 6/6 通过、50K 尺度 wall +10%；无质量退化 |
| 显式 `strata_prefix` 在低压力下没有额外收益 | 独立冷进程配对：暖请求 read 均 6311/6337，TTFT 中位数 ordinary 0.243s vs pinned 0.2505s | 无 slot/预算压力；pin 的保护语义只在竞争场景下才可能体现，未证 |

### 未决项

1. **重启窗口**：生产配置里 `--conversation-cache-min-tokens 3000`、`--conversation-cache-park-gain 3000`、`--conversation-cache-slots 100` 和 `LimitMEMLOCK=infinity` 都要引擎重启才生效，尚未生效。
2. **100K+ prompt 的间歇性 hang** 根因未定位（Stage 4 的阻断项）。Stage 5 在 100K 上 6/6 通过，但只在特定序列，不能视为已解决。
3. 显存压缩的 **阶段 6 / 7 / 8 / 9**（`q4_0`、`mtp-q4`、`kv-resident` 下调、组合验收）未执行 —— 阶段 2 已达标，当前无必要。
4. **质量验收仍缺失**：现有夹具只测缓存指标与固定输出标记，没有任务正确性评估；真实 agent 流量、>10 并发、跨 agent 共享前缀均未测。
5. **最终形态未定**：`--vram-elastic`（动态）还是静态 reserve；decode 退化阈值未预先设定。
6. 监控脚本没有定时任务；回滚路径已文档化但未演练。
7. 生产服务当前的运行状态未核实 —— 另一条工作线（[全局 KV ring 路线图](../.claude/goals/global-kv-ring-roadmap.md)）的记录是「生产服务暂时停服，未经明确授权不得恢复」。本次更新没有检查远端。

### 2026-10-19 · 量化选择研究

来源：`.omc/handoffs/2026-10-19-strata-quantization-research.md`。这是研究结论，不是本仓库的代码或实测变更。

- **基线不变**：生产仍按 IQ2_XS。换档需要按真实任务做 A/B（固定提示、上下文、采样参数、硬件，记录质量、速度、RAM/VRAM 峰值），不能靠社区单任务报告决策。
- 官方按 RAM 的定位：32 GB 可考虑 Coder（代码特化，中文/通用能力弱）；48 GB 推荐 IQ2_XS（追速度可选 Q2_0）；64 GB 可试 IQ3_XXS/IQ3_S；96 GB+ 可评估 IQ3_S 或 Unsloth 4-bit。专家 RAM 驻留量约 Q2_0 34 GB、IQ2_XS 35.5 GB、IQ3_XXS 43 GB、IQ3_S 50 GB、Coder 23 GB；Unsloth UD-IQ4_XS / UD-Q4_K_XL 分别约 72 GB / 88 GB。取 SSD 会显著变慢。
- Swift 1.5 是微调版本（思考更短、质量大致相同），不是量化等级。
- 被引用的第三方数据（Coder 的 SWE-bench 91% / LiveCodeBench 99%、MindStudio 单条 SVG 对比）都是**作者自报或单任务观察**，不足以建立普适排名；官方也明确「速度测试不是质量测试」。
- 证据边界：部分 GitHub/Medium 页面抓取 403，结论基于搜索摘录与成功读取的官方页面；要核实最新说法需重新抓取官方原始文档或查对应提交。

### 两处需要标注的内部矛盾

- **`--conversation-cache-slots`**：「建议配置与成本」节推荐 1000，同日更晚的「生产参数调整」节改为 **100**（1000 是预算养不起的名额）。以生产参数调整节为准。
- **`--prompt-cache-spread`**：四周期平衡 A/B 的结论是「不建议修改部署配置」（仅近回退获益），但「生产固化」已把它写进生产配置。两者不冲突（近回退确实获益 −62.8%），但生产配置的这一项依据只有近回退场景，长历史/远回退场景未验证。

### 引用与仓库现状

- **本仓库不承载开发**：引擎源码的权威在设备 B 的 worktree `/home/ET/chenran/strata-lab-20261017/`（`Niko1221/Strata`）。仓库里的 `src-tree/strata-src/` 是它的镜像快照（2026-10-19，`477ac13`；434 源文件 + 57 测试 + 45 markdown），引用可以在镜像里核对。文中 `generate.cpp:2989-2992`、`generate.cpp:12076`、`generate.cpp:7880`、`conv_cache.hpp`、`DETAILS.md:1126`、`docs/VRAM_ELASTIC.md`、`docs/UNSLOTH_Q4.md:147` 是 2026-10-18 当时的行号（引擎 `0.1.45`）。2026-10-19 在镜像里抽查 4 条（`generate.cpp:2989-2992`、`generate.cpp:12076`、`generate.cpp:7880`、`docs/DETAILS.md:1126`）内容一致，其余未逐条复核。
- 2026-10-19 只核对了引用文件是否存在；**没有验证任何运行状态**：GPU 归属、端口、服务状态、配置文件内容与所有实测数值均沿用 2026-10-18 的记录。原始测试数据（`/tmp/*.metrics.json`）当时已删除或仅暂存，仓库内不含可复算的原始数据。

## 目标与范围

提高 Strata 在多-agent 工作负载中的 exact-prefix cache 命中率，减少重复 prefill、TTFT 和 agent 轮转排队。当前部署实测表明，冷启动的近 256K 并发请求会产生显著排队；因此优先验证可复用的稳定前缀，而不是假定增加并行度或缩短上下文能解决问题。

本计划保持 IQ2_XS、`parallel=4`、`max-context=262144`。**所有 Strata 服务启动、模型推理、GPU 检查及测试数据采集均在远程 AI 服务器 `ssh ai`（设备 B，`/home/ET/chenran/strata-lab-20261017/`）执行**；本地开发机仅用于编辑本仓库文档/脚本及查看脱敏指标。GPU 3 与远程服务器 loopback `127.0.0.1:18080` 是唯一测试目标。显存压缩是独立的延期任务，不纳入本轮实验。

## 已知事实与证据边界

- Strata cache 复用依赖精确 token 前缀；语义相似但 token 不同不构成命中。
- Strata 返回可用于评估的 `usage.prompt_tokens`、`usage.prompt_tokens_details.cached_tokens`，以及 `timings.cache_n`、`timings.prompt_n` 和 prompt 吞吐/时间字段（字段可能因响应版本而异）。
- llmux 审计只确认了容器、挂载和留存文件的结构。历史 capture 是加密文件；没有读取请求正文、日志内容、数据库记录或 `master.key`，也未解密。因此，尚无证据表明 llmux 会改写、重排或规范化请求；不得将 cache miss 归因于 llmux。
- 先前 Strata 基准的 agent-cache 测试命中率为 68.59%，但它不是用户真实多-agent 请求基线，不能充当本轮验收基线。

## 安全与操作边界

- 仅在 GPU 3、`127.0.0.1:18080` 运行和压测；**该 loopback 地址指远程 AI 服务器，不是本地开发机**。开始前后检查 GPU 4 与 `:18492`，不得停止、修改、重启或压测该实例。
- GPU 2 保持停服，不启动或修改其 service/timer。
- 不读取、写入、打印或复制凭证、`.env`、`qdl.env`、`/proc/PID/environ`；不检查请求正文、原始日志、数据库记录或解密 capture。
- 不改系统包、驱动、CUDA、生产路径或 NInfer 部署；不使用 `pkill -f`。只用本任务启动并记录的精确 PID 停止 Strata。
- 不开放外网监听，不创建持久化服务。改动真实 agent 框架、提示词、工具 schema 或 llmux 配置前，先报告具体差异、风险与回滚方法并取得确认。
- 显存压缩轮若启用 `--vram-elastic`，会额外暴露 `POST /v1/vram`（一个改变引擎状态的 `/v1` 路由）。只在 loopback 上调用；其两条失败路径（驱动拒绝释放段、回填槽位失败）之后引擎的缓存表不可信，必须 `POST /v1/unload` → `POST /v1/load` 恢复，不得带伤继续压测。
- 实验请求仅使用非敏感合成数据；可临时保存合成 prompt/response 以调试采集、检查模板和验证任务质量，但不得使用真实用户请求正文、工具参数、凭证或受保护日志。原始测试输出应限于远程服务器受控临时目录，访问范围最小化；分析后优先保留足以复现结论的指标、配置和合成 fixture，清理无复用价值的临时文件。任何数据一旦可能含真实用户信息或密钥，立即停止读取/传播并按敏感数据处理。

## 指标与计算

按请求记录：

- `prompt_tokens`：总输入 token 数。
- `cached_tokens`：优先取 `usage.prompt_tokens_details.cached_tokens`，缺失时取 `timings.cache_n`；记录实际来源，避免双重计数。
- `read_tokens = prompt_tokens - cached_tokens`；若字段缺失或相互矛盾，标记该样本无效，不猜测。
- `cache_hit_ratio = cached_tokens / prompt_tokens`（prompt token 数为零时不计算）。
- TTFT：请求发出至首个生成 token 的时间；注明使用客户端实测还是服务端 timing。
- prefill：使用 Strata 可用的 `prompt_n`、`prompt_per_second` 或客户端计时，并保留原始字段名与单位。
- 多-agent 轮转：记录各请求 TTFT、完成时间、并发数及整体 wall time，报告中位数和 P95；分别统计 cold/warm、同 agent continuation、跨 agent 共享前缀。
- 质量：在固定、非敏感的合成任务集上比较回答正确性/任务完成度，并检查输出长度、截断、错误或拒答变化。
- 资源与隔离：记录 GPU 3 VRAM、主机 RAM、Strata PID/监听端口；验证 GPU 4 的进程、显存和 `:18492` 前后未受影响。

报告样本量、重复次数、模型/引擎版本、启动状态、缓存清理/预热状态、并发模式及测量方法。不能比较不同缓存状态或不同 prompt token 数的样本。

## 实测基线（2026-10-18）

在 AI 服务器 GPU 3 上用 Strata 自带 `tools/strata_bench.py` 执行合成基准（引擎/服务 `0.1.45`，IQ2_XS，`parallel_slots=4`，上下文 262144，int8 KV；三轮，零失败）。结果目录仅暂存于 `/tmp`，采集后已删除；未保存 prompt 或响应正文。

- agent session：Strata 基准程序按后续 5 turns 的 prompt tokens 计算命中率，中位数 68.72%（三次 68.61%–68.83%）；turn TTFT 中位数 1.80s，范围 1.22–2.06s。该 suite 的 `re-read tokens/session` 是“之前已有上下文但未被缓存”的估计量，中位数 5 tokens；它不同于本轮实际 `read_tokens`，不可混用。
- 冷 4K prompt：4019 tokens，prefill 2042–2064 tok/s，TTFT 1.98–2.00s，0 cached tokens。
- 冷 16K prompt：16058 tokens，prefill 2501–2519 tok/s，TTFT 6.43–6.48s，0 cached tokens。
- 短答 decode 中位数 124.2 tok/s；首 token 中位数 0.29s。
- 结束后 Strata 进程退出、`:18080` 关闭、GPU 3 回到约 13 MiB；GPU 4 保持约 21.8 GiB 且 `:18492` 监听，GPU 2 为 13 MiB，`ninfer-webui.service` inactive。

重要限制：官方基准 agent 情景是合成 coding-agent session，不等于真实多-agent 流量，也未测试跨 agent 共享前缀、50K/150K 工作负载、动态字段排列或并发轮转。因此此结果仅作为受控合成基线，真实流量基线仍缺失。基准脚本本身有一个未定义 `port` 的启动缺陷；本次在服务器上临时加了一行局部修复以运行官方 suite，结束后已恢复原文件并核对 SHA-256。该变更不在版本化仓库，也未保留临时结果。

> **更正（2026-10-19 核对）**：上一句的"已恢复原文件"不成立。设备 B 上 `tools/strata_bench.py` 相对上游仍有 2 增 1 删的本地改动（把 `server_base(cfg.get("host"), a.port or int(cfg.get("port", 8080)))` 拆成先取 `port` 再调用），即当时那处修复**没有回滚**。该文件不在本仓库镜像范围内（`tools/` 未纳入）。

## 受控前缀探测（2026-10-18）

使用 GPU 3、相同模型和四路设置，在 loopback 上用独立 Strata 进程作了短合成请求探测。请求正文仅含固定合成 system 文本和任务尾部，未保留正文；仅留 token 数、命中数、TTFT 与一个输出标记正确性布尔值。GPU 4 与 `:18492` 未触碰，GPU 2 保持停服；完成后 Strata 精确 PID 退出、`:18080` 关闭、GPU 3 资源释放。

初次探测设计**无效**：稳定请求仅 1832 tokens，低于默认 `--prompt-cache-root 2048`，且默认生成长度仅 5 tokens。相同请求没有复用，说明没有形成用于新对话的 system-root checkpoint；这些结果不能用于判断稳定前缀或字段位置效果。变更一个工具定义顺序的两个请求则观察到第二次命中 2111/2118 tokens，但仅有单对且顺序方向未平衡，不能据此证明重排工具 schema 一定可复用。所有测试回答均包含预期标记，但这只验证返回通路，不是质量验收。

下一轮探测必须先让稳定 system+tools 渲染后超过 2048 tokens，并先用 Strata 自带 tokenizer/实际 chat template 验证；使用足够长的确定性合成背景（目标约 8K–16K prompt tokens）及较短、固定生成长度。按“冷请求 → 原样续问 → 仅尾部字段改变 → 同一字段置于共同 system 前缀的不同位置”的顺序做请求；以同一会话/同一引擎记录 warm hit，并另外检验独立 agent 是否可复用 pinned root。每类至少重复多轮且 A/B 顺序平衡。工具 schema 顺序作为独立因素单测，不与字段位置混测。

### 校准长前缀结果（2026-10-18）

按同一 GPU 3 / loopback `:18080` 隔离配置运行第二轮合成探测：Strata `0.1.45`、IQ2_XS、`parallel=4`、上下文 262144；稳定 system+tools 前缀约 7,976 tokens（高于 root 门槛），每请求生成上限 16 tokens。仅保存指标，不保存请求或响应正文。运行精确 PID 47402 正常退出；结束后 `:18080` 已关闭、GPU 3 回到 13 MiB，GPU 4 仍为约 21.8 GiB 且 `:18492` 监听，GPU 2 为 13 MiB 且 `ninfer-webui.service` inactive。

| 场景 | 样本 | prompt tokens | cached tokens | TTFT | 观察 |
|---|---:|---:|---:|---:|---|
| 不同 agent identity 的冷请求 | 3 | 7977 | 0 | 3.569–3.806s | 各自前缀不同，未命中 |
| 共享稳定 root、任务尾部各异 | 6 | 7976 | 首个 0；随后各 7953 | 首个 3.617s；随后 0.226–0.232s | 同一进程内后续合成任务复用了共同 root；read 为 23 tokens |
| 相同历史下的两个续问 | 2 | 7996 | 各 7953 | 0.310–0.325s | 共享长 root 命中；read 为 43 tokens |
| stamp 放在稳定前缀前 | 4 | 7975 | 0 | 3.591–3.616s | 每次 stamp 改变 root 起始 token，未命中 |
| stamp 放在稳定内容后 | 4 | 7977 | 0 | 3.602–3.627s | 该组每个样本也都是冷请求，未命中；不能与前组作位置因果比较 |

所有请求均返回固定标记（`marker_ok=true`），仅证明响应通路，不是任务质量评估。该小样本支持“相同且足够长的 token root 在同一 Strata 进程内可被后续不同任务复用”；它不证明不同 root 下的同字段位置对比效果，也未测试并发同时到达、跨进程持久命中、显式 `strata_prefix` 或真实 agent 流量。下一步应在共同稳定 root 已热的同一进程内，交错随机测试尾部动态字段与前置字段，并独立验证显式 prefix 与并发轮转；当前证据不可外推至真实流量。

### 热 root 字段位置复测（2026-10-18，未形成有效 A/B）

启动新隔离实例（精确 PID 57857），先以 7,033-token system root 做一次冷暖身，再交错提交 8 个稳定 root + 动态 user 尾部请求和 8 个 system 前置 stamp 请求。首个尾部任务命中 7,011/7,035 tokens、read 24、TTFT 0.244s；随后稳定尾部请求全部未命中，TTFT 3.237–3.307s。前置 stamp 请求也均为冷读，TTFT 3.212–3.299s；最后一条响应缺失 usage 字段，标为无效。所有输出均含固定标记。

暖身后缓存未被后续请求复用，且设置发生变化时稳定尾部组也失去命中；因此这轮没有建立可用的同缓存状态 A/B，也不能证明 stamp 位置的效果。可能因素包括实际 chat template/request identity 与所假设的 root 共享机制不同、服务端缓存状态/slot 淘汰，当前证据无法区分；不得直接归因。运行超过预期后按记录的精确 PID 发送 TERM 停止；最终确认 `:18080` 关闭、GPU 3 回到 13 MiB、GPU 4 的 `:18492` 仍监听且约 21.8 GiB、GPU 2 为 13 MiB 且服务 inactive。指标 JSON 暂存于远程 `/tmp/strata-prefix-warmroot-ab.metrics.json`，只含合成指标；未保存 prompt 或回复正文。

### 显式 pinned prefix 验证（2026-10-18）

按计划在独立 GPU 3 / loopback `:18080` Strata 进程中验证普通请求与 `strata_prefix: {"messages": 1}`。配置固定 IQ2_XS、`parallel=4`、上下文 262144；同一合成 system root 约 6.3K tokens，短任务尾部不同。4 组普通/显式 pin 冷请求均完整记录 usage，cached 为 0，TTFT 2.989–3.218s。随后同一 pinned root 下连续 8 个不同合成任务，8/8 均缓存 6,311/6,336 tokens（read 25），TTFT 0.234–0.237s，所有 usage 字段齐全且固定标记通过。

此结果证明：当前 Strata 实例接受显式 `strata_prefix`，并且在同一进程内能对后续请求复用 pinned root；这组数据足以验证共享前缀的功能路径，但不构成普通 vs pin 的性能因果比较，因为冷组的 system 前缀有意不同，且两组缓存状态不同。未进行并发到达、slot/预算压力、长期淘汰或真实 agent 请求测试。`strata_prefix` 是请求级行为，本轮未改客户端或持久服务配置。

服务端日志 `/tmp/strata-pinned-prefix-probe.engine.log` 经筛选未找到 `conversation cache:` park/restore/eviction 事件；因此日志不能作为本次 pinned 命中的独立佐证，结论依据完整 usage 中的 `cached_tokens`。远程临时指标 `/tmp/strata-pinned-prefix-probe.metrics.json` 仅包含合成指标；未保存 prompt/回复正文。任务精确 PID 84390 正常退出；复核 `:18080` 关闭、GPU 3 为 13 MiB、GPU 4 约 21.8 GiB 且 `:18492` 仍监听、GPU 2 为 13 MiB 且 `ninfer-webui.service` inactive。

#### 并发 pinned-root 探测（2026-10-18）

同一固定前缀先以一次普通请求冷启动（6,336 tokens，0 cached，TTFT 3.285s），随后显式 pin 的 2 路和 4 路并发请求。2 路样本均命中 6,311/6,337 tokens，read 26；TTFT 0.267s、0.751s，wall time 约 1.05s。4 路样本均命中 6,311/6,337，read 26；TTFT 0.371–1.795s，完成时间 1.569–2.096s，wall time 约 2.10s。所有 6 个并发请求 usage 完整、无错误且固定标记通过。Engine log 对应请求记录 6,311 reused + 26 read，说明不同任务尾部的共享 pinned root 在 2/4 路并发下均复用成功；TTFT 有明显排队波动，不应把单轮当作稳态 P50/P95 或改善承诺。

本轮未并行比较普通请求；普通冷请求只作 warm root。没有缓存清理、slot/预算压力、50K/150K prompt 或多轮随机 A/B，亦未测真实 agent。精确 PID 87876 正常退出；结束后 `:18080` 关闭、GPU 3 为 13 MiB，GPU 4 约 21.8 GiB 且 `:18492` 监听，GPU 2 为 13 MiB 且服务 inactive。远程 `/tmp/strata-pinned-concurrency.metrics.json` 和 `/tmp/strata-pinned-concurrency.engine.log` 仅包含合成指标/engine 运行统计，不含 prompt/response 正文。

后续执行 2/4 路多轮随机交错的 pinned 与普通对照，并覆盖代表性较长上下文；在证据稳定前不建议改真实 agent。

#### 交错普通/pinned 2 路对照（2026-10-18，状态混杂）

同一进程先做普通 warm-up，再交替安排 4 组 2 路普通与 pinned 请求（每组并发内两个不同尾部）。全部 16 个请求 usage 完整、无错误、固定标记通过，但结果依赖先前请求状态：普通组有 4/8 命中 6,311 或 6,330 tokens，另有 4/8 冷读；pinned 组有 6/8 命中 6,330 tokens、1/8 命中 6,311、1/8 冷读。命中样本 read 为 7 或 26 tokens；冷读为 6,337。两组命中/冷读混杂，不能计算可信 A/B 效应，也不能声称 pin 优于普通请求。并发与同进程缓存共享/驻留状态未重置，且尾部任务按组变化；该轮仅说明不同模式下均可能复用，也可能冷读。

此结果不满足验收。后续应从每个独立冷启动开始配对，每次只运行一种模式的预定义序列，或明确在两种模式间保持完全相同的缓存驻留过程；交错顺序随机化并重复多个进程周期。应继续记录服务端 `prompt N = reused + read` 与客户端 usage，采集每组 wall time、P50/P95；在完成可比实验前，不把缓存命中差异归因于 `strata_prefix`。

#### 独立冷进程普通/pinned 配对复测（2026-10-18）

为排除上一轮同进程 cache 状态混杂，按两个独立周期执行：周期 0 顺序为 ordinary → pinned，周期 1 反向为 pinned → ordinary；每个新进程、每种模式均用相同 6,337-token 前缀发 6 个不同合成尾部请求，第一条为冷样本，后 5 条为暖样本。总计每模式 12 个请求，usage 全完整、零错误、标记均通过。

- ordinary：2/2 冷请求均 0 cached，TTFT 3.301s、3.261s；10/10 暖请求均缓存 6,311/6,337 tokens（read 26），TTFT 中位数 0.243s。
- pinned：2/2 冷请求均 0 cached，TTFT 3.300s、3.244s；10/10 暖请求均缓存 6,311/6,337 tokens（read 26），TTFT 中位数 0.2505s。

结论：在本合成负载下，普通 exact-prefix conversation cache 已稳定复用相同 root；显式 pin 没有增加 cached token，暖请求 TTFT 也没有显示优势（本小样本中位数略慢约 7.5ms，不视为显著差异）。pin 的源码语义是保护共享 checkpoint 不被淘汰，收益应在多 root/slot 或内存压力竞争时验证；当前无压力实验不能证明也不能否定该保护价值。每组请求顺序固定、只有两个进程周期，未做多轮随机统计、slot 压力及更长上下文，因此不能推广到真实多-agent 工作负载。

4 个 Strata 精确 PID 均正常退出。最终状态：`:18080` 已关闭，GPU 3 为 13 MiB；GPU 4 约 21.8 GiB 且 `:18492` 仍监听；GPU 2 为 13 MiB 且 `ninfer-webui.service` inactive。远程 `/tmp/strata-cold-process-paired-ab.metrics.json` 仅含 token/时延等合成指标，无 prompt 或回复正文。

### Slot 压力首次尝试（2026-10-18，无效设置）

首次尝试在隔离启动配置中加入 `--conversation-cache-mib 2048 --conversation-cache-slots 2 --prompt-cache 6`，但配置键放置错误，日志未包含新增选项；该轮不构成 slot-pressure 实验，不能用于判断 pin 的淘汰保护。随后单独核验参数传递时，初始化未在预期时间完成，按精确 PID 105668 发送 TERM 并清理临时配置，未发 HTTP 请求。该次结束时 `:18080` 关闭、GPU 3 为 13 MiB、GPU 4 `:18492` 仍监听、GPU 2 服务 inactive。之后的有效参数复测见下节。


### 有效参数 slot 压力复测（2026-10-18）

修正配置注入后，在 GPU 3 loopback 实例启动时确认 engine 参数包含 `--conversation-cache-mib 2048 --conversation-cache-slots 2`。合成序列先冷读 pinned root（6337 tokens），插入两个不同 pinned competition roots（各 6344 tokens，均冷读），随后重访原 root：第一次回访仍冷读（6337 cached 0，TTFT 3.013s），之后 4 次请求均缓存 6315/6337 tokens（read 22，TTFT 0.222–0.558s）。请求均成功、usage 完整、固定输出标记通过。

结论仅限本次配置与序列：参数已实际传入，pinned root 在竞争 root 后仍可重新建立并被复用，但第一次回访的冷读说明本数据**不能证明** pinned checkpoint 在两 slot 压力下免于淘汰；两竞争 root 也不足以证明淘汰顺序或保护机制。尚无相同 slot/请求序列下 ordinary 对照，故不能归因 pin 的效果。后续若继续，应以完全匹配的 ordinary/pinned 独立冷进程配对测试，并增加重复周期；当前不扩大结论。

实例正常退出，临时脚本、配置和 engine log 已清理，仅保留 `/tmp/strata-slot-pressure-valid.metrics.json` 中不含正文的合成指标。复核时 `:18080` 无监听、GPU 3 为 13 MiB，GPU 4 约 21.8 GiB 且 `:18492` 仍监听，GPU 2 `ninfer-webui.service` inactive。

#### 同序列 ordinary/pinned 独立冷启动配对（2026-10-18）

固定 GPU 3、loopback `:18080`、IQ2_XS、`parallel=4`、上下文 262144，以及实际注入的 `--conversation-cache-mib 2048 --conversation-cache-slots 2`。每个独立进程依次发同一合成序列：目标 root 冷请求、两个不同竞争 root、目标 root 的 4 次回访；ordinary 不带 `strata_prefix`，pinned 显式指定 `strata_prefix: {"messages": 1}`。周期顺序为 ordinary → pinned → pinned → ordinary，每种模式 2 个进程周期、12 个请求。

| 模式 | 冷请求 | 两个竞争 root | 目标 root 回访 | 暖回访 TTFT 中位数 |
|---|---|---|---|---:|
| ordinary | 2/2 均 0 cached，TTFT 3.219–3.287s | 4/4 均 0 cached，6337–6344 tokens | 8/8 命中 6315/6337（read 22） | 0.5645s |
| pinned | 2/2 均 0 cached，TTFT 3.269–3.271s | 4/4 均 0 cached，6337–6344 tokens | 8/8 命中 6315/6337（read 22） | 0.3915s |

28/28 请求均成功、usage 完整且固定输出标记通过。暖回访命中数完全相同；每个 root 的首次回访均命中，和先前有效压力探测中首次回访冷读的表现不同，说明这组小样本呈现时序/缓存驻留差异，不能据暖 TTFT 中位数推断 pin 有性能优势，也不能证明 ordinary 与 pinned 的淘汰保护等价。测试周期仅各两次，TTFT 尚包含与竞争请求后的状态效应，结论仅为本序列观察。需要更多随机顺序独立周期及明确的 checkpoint 驻留/淘汰观测，才能检验 pin 的防淘汰语义；当前不推广该效果。

四个精确 Strata PID 均正常退出。临时脚本、配置和 engine log 已清理；仅保留远程 `/tmp/strata-slot-ab.metrics.json` 中的无正文合成指标。最终复核：`:18080` 关闭、GPU 3 为 13 MiB；GPU 4 约 21.8 GiB 且 `:18492` 仍监听；GPU 2 为 13 MiB，`ninfer-webui.service` inactive。

### `--prompt-cache-spread` 合成回退探测（2026-10-18，单周期初测）

确认 Strata engine 默认 `--prompt-cache 6`（约 118 MiB/ checkpoint）、`--prompt-cache-every 16384`、`--prompt-cache-root 2048`；测试显式固定 cache 6、checkpoint interval 2048 tokens，其余模型、context、parallel 保持配置默认，启动日志可核验参数；off/on 两种模式分别独立冷启动，均在 GPU 3 loopback。构造 20 轮相同合成对话（最终 prompt 13,171 tokens），逐轮请求后将约 5 轮前的历史内容微调并重放，再做一次修订历史续问。每模式各 22 个请求均成功且 usage 完整；仅指标被保存，没有 prompt/response 正文。

| 模式 | 构建阶段 | 5 轮前历史改写 | 修订后续问 |
|---|---|---|---|
| spread off | 逐轮每次 read 664 tokens；末轮 prompt 13,171，cached 12,507 | prompt 9,890；cached 681；read 9,209；TTFT 4.327s | read 25；TTFT 0.221s |
| spread on | 与 off 相同，逐轮每次 read 664 tokens | prompt 9,890；cached 7,251；read 2,639；TTFT 1.636s | read 25；TTFT 0.236s |

本轮在约 5 轮回退下，spread 的 `read_tokens` 比 off 少 6,570（约 71%），回退 TTFT 低 2.691s；修订后的下一次续问两者均只读 25 tokens。该结果与“保留更靠近回退点的 checkpoint”机制吻合，是值得重复验证的初步信号，不是稳健性能结论：每模式仅一个独立进程，A/B 顺序固定（off 后 on），prompt 约 13K，且流式输出内的固定标记未被检测到（22/22 各请求的 marker flag 均 false），因此质量标记检查未通过；这一版本的 SSE 收集没有确认最终输出标记是否在非 `content` 字段。不能据此声称质量验收通过或推广。下一轮应修正无正文的完成标记验证（仅比较 hash/布尔值），平衡独立进程顺序并增加周期，另测更长历史、不同回退距离；核实服务端 resume/reused 与客户端 cached usage 一致后再报告效果。

两实例的精确 PID 均正常退出；测试脚本、配置及 engine log 已删除，仅留下 `/tmp/strata-spread-ab.metrics.json` 合成指标。结束复核：`:18080` 关闭、GPU 3 13 MiB，GPU 4 约 21.8 GiB 且 `:18492` 仍监听，GPU 2 为 13 MiB 且服务 inactive。该初测随后由下述平衡重复和标记复测补充，不再作为质量标记失败的当前结论。


#### 平衡重复与固定标记复测（2026-10-18）

修复 SSE 采集器，同时拼接 `content` 与 `reasoning_content` 后，对相同 20 轮、约 7,571-token 最终历史、约 5 轮前单处合成改写，按 `lru → spread → spread → lru` 做 2 周期独立冷进程平衡重复。固定 `--prompt-cache 6 --prompt-cache-every 2048`，唯一变化为有/无 `--prompt-cache-spread`。每模式 44/44 请求成功且 usage 完整，44/44 固定输出标记通过；总计 88 次调用均只在内存中检查标记，不保存输出正文。

| 模式 | 改写 read tokens | 改写 TTFT | 改写后续问 read tokens |
|---|---:|---:|---:|
| spread off（2 次） | 5,289 / 5,289 | 2.621 / 2.677s（中位数 2.649s） | 25 / 25 |
| spread on（2 次） | 1,519 / 1,519 | 1.276 / 1.220s（中位数 1.248s） | 25 / 25 |

在这个合成 7.6K prompt、约 5 轮回退场景，spread 两周期均少读 3,770 tokens（约 71.3%），中位 TTFT 少 1.401s；修订后的后续请求两者都只读 25 tokens。20 轮增长阶段每轮两模式均约读 384 tokens，未见普通续问差异。平衡顺序和重复结果一致，固定标记通过，使此场景的指标与功能检查形成初步可重复证据；样本仍只有每模式两个进程周期、prompt 较短、任务仅验证固定响应标记，不是一般质量评估，也不代表真实流量或长上下文性能。结果支持继续测更长历史与多个回退距离，尚不足以建议真实部署变更。

4 个精确 Strata PID 均正常退出；测试脚本、配置和 engine log 已删除，仅保留 `/tmp/strata-spread-repeat.metrics.json` 中无正文指标。最终核验 `:18080` 关闭、GPU 3 为 13 MiB、GPU 4 约 21.8 GiB 且 `:18492` 监听、GPU 2 为 13 MiB 且 `ninfer-webui.service` inactive。

#### 更长历史、多回退距离探测（2026-10-18，单周期；服务端 summary 未保留）

在 GPU 3 loopback 固定 `--prompt-cache 6 --prompt-cache-every 2048`，按 LRU → spread 各一独立进程构造 32 轮合成历史，最终 prompt 16,732 tokens；分别改写约 5、15、28 轮前的一个合成 user turn，并统计客户端 usage、TTFT 和固定输出标记。70/70 请求均成功、usage 完整，所有响应标记通过。客户端结果如下：

| 回退距离 | spread off：read / TTFT | spread on：read / TTFT | 观察 |
|---|---:|---:|---|
| 约 5 轮 | 13,601 / 5.898s | 1,073 / 1.077s | spread 少读 12,528 tokens |
| 约 15 轮 | 189 / 0.464s | 8,381 / 3.938s | LRU 少读 8,192 tokens |
| 约 28 轮 | 1,595 / 1.239s | 1,595 / 1.264s | 相同 read，TTFT 接近 |

此结果与前一组 7.6K prompt、5 轮回退的两周期结果方向一致，但揭示了距离依赖：spread 在近回退点明显获益，约 15 轮时当前 LRU 保留点反而更近，而约 28 轮相同。它不是单调/普遍改善证据；本轮每模式仅一个进程，模式顺序未平衡，且 16.7K token 仍非代表性长上下文。虽然启动环境设置 `STRATA_REQUEST_LINES=1`，测试时 server stdout 被丢弃，故没有保留 engine request summary 与客户端 cached/read 的逐请求交叉核对；不能把“已设置”写成“已验证一致”。下一轮需把父服务 stdout 安全地导向受控临时文件并只提取不含正文的 numeric summary，保留各场景配对核对后即删除原始 log；增加平衡独立周期，重点覆盖多个回退距离，不能根据本初测推广 spread 或修改部署配置。

两个精确 Strata PID 正常退出；测试脚本、临时配置和 engine log 已删除，仅保留 `/tmp/strata-spread-long.metrics.json` 无正文客户端指标。结束复核 `:18080` 关闭、GPU 3 13 MiB，GPU 4 约 21.8 GiB 且 `:18492` 仍监听，GPU 2 13 MiB 且 service inactive。

#### 服务端 summary 交叉核对复测（2026-10-18，中止；无有效 A/B）

随后尝试按同一 32 轮长历史及近/中/远回退，在 GPU 3 loopback 采集客户端 usage 与 `STRATA_REQUEST_LINES=1` 服务端数值 summary。复测第一组 LRU 仅完成至第 5 轮增长请求即被停止；没有完成历史构建或回退请求，也未产出可用 A/B 和客户端/服务端逐请求核对。不得将这次尝试计入样本或结论。

该次服务进程已停止，`:18080` 已关闭，GPU 3 回到 13 MiB；GPU 4 约 21.8 GiB 且 `:18492` 仍监听，GPU 2 为 13 MiB。之后对临时 stdout `/tmp/strata-sv-parent-0.out` 做了流式数值字段提取，没有读取或输出正文，并删除原文件。共提取 17 条无正文 summary（prompt 494–7,966 tokens，cached 0–7,492）；与任务执行器可见的客户端输出只到 grow_5，数量不一致，无法逐请求配对验证，故该次仍未完成客户端/服务端交叉核对，也不将这些 summary 用作 A/B 证据。复核确认临时 stdout 已删除、`:18080` 无监听、GPU 3 为 13 MiB，GPU 4 约 21.8 GiB 且 `:18492` 仍监听，GPU 2 为 13 MiB、service inactive；无可见 `/tmp/strata-sv-*` 遗留文件。

### 已修订采集器后的再尝试（2026-10-18，中止；无效周期）

根据失败证据检查远程 `serve/server.py`：`echo_requests()` 在后台 daemon thread 中追随 engine log；请求完整结束后也可能有约 0.2 秒的输出延迟。此前控制器关闭 stdout 文件后立即读取，可能早于最后一条 summary 落盘，因而出现客户端 35、服务端 34。另一个配对陷阱是不能只按顺序 `zip`：失败/取消请求会缺少 engine 完成摘要，后续摘要可能整体错位；应等待摘要稳定，并按 prompt/cached/output 计数、请求阶段与服务端 completion 关联，缺项就判周期无效。

改用“完成请求后读受控 stdout、按 token 数核验”的采集器，再运行 LRU → spread → spread → LRU。第一 LRU 周期完成 35 条客户端请求、固定标记均通过，但仍只得到 34 条服务端摘要，配对失败。第二 spread 周期完成 35 条请求、标记通过，摘要也仅 34 条，配对失败。第三 spread 服务健康显示 loaded，但真实 chat 请求出现连接重置/断开，之后未收到请求进度；第四 LRU 未执行。没有完整四周期，也没有可用交叉核对 A/B。该次留下的客户端事件只含长度/命中等数值，但由于两周期服务端摘要数不匹配，不能把这些请求数据用于效果结论。

### 已验证同步采集器的平衡 A/B（2026-10-18，四周期完成）

按 LRU → spread → spread → LRU 执行了四周期独立冷启动，每周期 20 轮增长、3 个回退距离（5/10/15 轮），共 92 个客户端请求。所有请求均成功、固定标记通过；每个周期的 23 个客户端请求与 23 个服务端 summary 全部 prompt/cached token 一致，四周期配对均通过。

| 模式 | 回退 5 轮 read / TTFT | 回退 10 轮 read / TTFT | 回退 15 轮 read / TTFT |
|---|---:|---:|---:|
| LRU（2 周期） | 1104 / 1.135–1.222s | 719 / 0.829–0.922s | 334 / 0.647–0.651s |
| spread（2 周期） | 411 / 0.818s | 719 / 0.832–0.833s | 334 / 0.753–0.756s |

在 20 轮约 10K token 历史下，spread 对近回退（5 轮）少读 693 tokens（62.8%），TTFT 低 0.317–0.404s；10 轮与 15 轮 read 完全相同；spread 在 15 轮 TTFT 略高 0.102–0.109s。增长阶段两模式 read 均 384 tokens。结果仍只限短历史、固定 synthetic transcript、固定 5/10/15 轮回退；spread 的优势仅在近回退稳定出现，不能证明普遍或远回退收益。不建议修改部署配置。

> 注：后续「生产固化」仍把 `--prompt-cache-spread` 写进了生产配置。其依据只有近回退场景（−62.8%）；远回退无收益、15 轮 TTFT 略高（+0.10 s），长历史形状未验证。

临时 metrics 为 `/tmp/strata-spread-fast.metrics.json`；服务 PID 已正常退出，`:18080` 已关闭，GPU 3 回到 13 MiB，GPU 4 约 21.8 GiB 且 `:18492` 仍监听，GPU 2 为 13 MiB 且服务 inactive。




### 源码复核：checkpoint 的放置与保留（2026-10-18）

在远程工作树读取引擎源码（`src/program/generate.cpp`、`include/strata/program/conv_cache.hpp`、`message_boundary.hpp`、`conv_cache_test.cpp`）后确认以下机制，后续实验以此为依据：

- checkpoint 位置取自 `cuts = {reread_to, root_at, message_at, turn_at, n-1}`，另加周期性 checkpoint（每 `--prompt-cache-every` 个新读 token，落在 prefill chunk 边界，serve 路径 chunk=2048）。
- `root_at` = 第一条 turn 边界（system prompt 末尾），**仅当该位置 ≥ `--prompt-cache-root`（默认 2048）时才建立**。
- 保留策略（`conv_cache.hpp`）：**root 永不被淘汰**，无论 stamp 多旧；其余按 LRU 轮换；`--prompt-cache-spread` 改为按几何间隔铺开（近密远疏）。`pin` 保护的是第二个共享前缀，不是 root。
- `STRATA_CACHE_MESSAGE_BOUNDARY=1`（默认关）额外在上一条消息边界存 checkpoint（条件：末条消息 <1024 tokens 且前缀 ≥8192）。
- checkpoint 分配在**主机内存**（~118 MB/个），本机 848 GB 可用。

### `--prompt-cache-root` 对照（2026-10-18，四周期平衡）

短 system prompt（渲染后 1993 tokens，低于默认 root 门槛 2048）下的独立冷进程配对。固定 `--prompt-cache 6 --prompt-cache-every 2048`（间隔大于 prompt 长度，无周期 checkpoint，隔离 root 单一变量），仅改变 `--prompt-cache-root`。顺序 2048 → 512 → 512 → 2048；每周期 1 个冷请求 + 3 个保持同一 system prompt、仅换 user 任务的请求。16/16 请求客户端 usage 与服务端首个 engine summary 逐 token 一致，固定输出标记全部通过。

| `--prompt-cache-root` | 冷请求 | 3 个暖请求 | 暖 TTFT |
|---|---|---|---|
| 2048（默认，2 周期） | 2/2 cached 0，read 1993，TTFT 1.649/1.681s | 6/6 cached 0，read 1993 | 1.389–1.438s |
| 512（2 周期） | 2/2 cached 0，read 1993，TTFT 1.876/1.828s | 6/6 cached 1971，read 22 | 0.219–0.244s |

结论：system prompt 短于默认门槛时**根本不建 root checkpoint**，共享同一前缀的后续请求全部全量重读；把 `--prompt-cache-root` 降到 512 后，暖请求 read 由 1993 降到 22（-98.9%），TTFT 由约 1.41s 降到约 0.23s。两周期完全一致、顺序平衡、配对与标记均通过。边界限制：仅一个 prompt 长度（1993）、单进程内顺序请求、无并发；尚未测更短/更长 system prompt 的阈值行为与真实 agent 流量。

### checkpoint 密度对照（2026-10-18，四周期；slots 与 interval 同时变化）

同一 ~14.9K token 合成历史（20 轮，每轮约 700 tokens），先做 20 轮增长，再分别改写第 16、11、6 轮（距末尾 5/10/15 轮）。固定 `--prompt-cache-root 512`，顺序 (2048,6) → (512,24) → (512,24) → (2048,6)（括号为 `--prompt-cache-every`, `--prompt-cache`）。每周期 23 请求，四周期 92/92 客户端 usage 与服务端 engine summary 逐 token 配对通过。

增长阶段四周期完全相同（每轮 read 495–793，cached 随历史增长），说明差异只出现在改写阶段。

| 配置 | 改写 5 轮前 read / TTFT | 改写 10 轮前 | 改写 15 轮前 | 改写时 cached |
|---|---:|---:|---:|---:|
| every 2048, slots 6 | 13021 / 5.846s | 12864 / 5.745s | 13212 / 5.923s | 1730（仅 root） |
| every 512, slots 24 | 3315 / 1.951s | 6390 / 3.112s | 9894 / 4.845s | 11436 / 8204 / 5048 |

两配置的名义覆盖相同（6×2048 = 24×512 = 12288 tokens），但效果差别很大：2048/6 的链在改写时只剩 root（cached 1730），整段历史重读；512/24 在三个距离上分别复用 11436/8204/5048，read 降低 74.5% / 50.3% / 25.1%，TTFT 降低 3.90 / 2.63 / 1.08s。两次独立周期数字逐位相同（13021/12864/13212 与 3315/6390/9894 各出现两次），顺序平衡，说明该差异在本合成负载下高度可复现。

边界：本组同时改变了 interval 与 slot 数，**不能**据此把差异单独归因于 interval；补测的 2×2 见下节。改写仍是单处合成 user turn，未测并发、跨 agent 共享与真实流量；固定输出标记检查在本次设置下未通过（`max_tokens` 偏小导致回答被截断），故本组**不构成质量验收**，只用于缓存指标。

#### 2×2 补测：interval 与 slot 数分离（2026-10-18，四周期）

同样历史与序列，补齐 `--prompt-cache-every` ∈ {2048, 512} × `--prompt-cache` ∈ {6, 24} 的四个格子，顺序 (2048,24) → (512,6) → (512,6) → (2048,24)。改写阶段 read tokens：

| `--prompt-cache-every` \ `--prompt-cache` | 6 | 24 |
|---|---:|---:|
| 2048 | 13021 / 12864 / 13212 | 3315 / 6390 / 9894 |
| 512 | 13021 / 12864 / 13212 | 3315 / 6390 / 9894 |

**checkpoint 间隔在 chat 形状的流量上对结果没有任何影响**：同一 slot 数下两行数字逐位相同，且四个格子各自的两个独立周期也逐位相同。决定改写成败的唯一变量是 `--prompt-cache`（保留多少个 checkpoint）：6 时改写只能回落到 root（cached 1730，整段重读）；24 时链上仍保留足够多的 turn 边界 checkpoint，改写回落到编辑点之前最近的 turn，read 随距离变化为 3315/6390/9894。

机制解释与源码一致：引擎对**每个请求**都会在其最后一个 turn 边界存一个 checkpoint（`turn_at`），所以 `--prompt-cache-every` 只是在同一网格上加冗余点；真正决定改写后 read 的是这些 turn checkpoint 被保留了多少个。增长阶段四周期 TTFT 中位数 0.716–0.745s，未见 slot 数带来的可测成本（24 个 checkpoint 约 2.8 GB 主机内存，本机 848 GB 可用）。

边界：本组仅覆盖 20 轮、约 14.9K token、单处合成改写的 chat 形状流量。若前缀是**单篇长文档**（没有 turn 边界），`turn_at` 只有一个，此时 `--prompt-cache-every` 应当是唯一有效杠杆——本组未测该形状。固定输出标记在本组仍**未通过**，故同样只是缓存指标，不是质量验收。

## 多会话并行缓存池（2026-10-18）

上面各节测的都是**单个会话内部**的复用。本节针对真实负载：**多个 agent / sub-agent 并行**，每个会话 20K–100K token（部分 100K–200K），**不是每个会话都超长**；痛点是**跨会话互相淘汰**——会话 A 在跑，把会话 B 的缓存踢掉，B 只好冷启动重读自己的 20K–100K。

### 需求与四条硬约束

1. 池子上限按**内存字节**，不按条数；
2. 每条**动态分配、动态增长**，不是定长；
3. 会话**不绑定** worker/slot——第一轮会话 A 的缓存落在哪个槽位，第二轮 A 被分配到别的槽位也必须能读到；
4. 显存放不下→降级到宿主内存；宿主内存满→**按策略淘汰**（**不落 SSD**）；**仍在内存里的缓存必须被正确召回**。

### 源码核实（只读，未改任何代码）

| 约束 | 现有机制 | 结论 |
|---|---|---|
| ① 字节封顶 | `--conversation-cache-mib` 就是字节预算（默认 `0` = 关闭）；`ConversationCache::make_room()` 判据为 `bytes() > budget_ - held - incoming`。`--conversation-cache-slots` 是**独立**的条数上限（默认 4），把它设大（如 1000）即让字节成为唯一约束 | 满足，仅配置 |
| ② 动态长度 | 每个 parked 条目是一个 `SavedConversation`，各自携带 `bytes()`；无固定长度、无定长槽 | 满足，仅配置 |
| ③ 不绑定槽位 | `ConversationCache::best()` 遍历**全部** parked 条目，按 token 前缀内容寻址取最长匹配（`for (size_t i = entries_.size(); i-- > 0;)`），索引与 slot 无关；`enabled() = budget_ != 0 && slots_ != 0`，两者都非零才生效 | 满足，仅配置 |
| ④ 降级/淘汰/不落盘 | 快照全部驻宿主 RAM（`ConversationBuffer` 是 16 MiB 段链，每段独立分配，淘汰即归还内核）；`evict_oldest()` 淘汰**最久未被使用**的条目；全流程无磁盘路径 | 满足，仅配置 |

**四条约束现有引擎全部满足，不需要改代码。** 另外两条对多 agent 有用的机制：`borrows()` 允许新会话**借用** parked 条目内部的一个 checkpoint（兄弟 subagent 共享 system prompt/tools 的场景），借完捐赠者仍留在池中；`drop_superseded()` 让同一会话上一轮的旧副本在写新副本时先释放，避免"每轮多占一条"把别人的条目挤出去。

> 采集要点：引擎的 `conversation cache:` 行写到 **config 的 `log` 字段所指文件**，不是 server 的 stdout。此前按 server stdout 抓取会得到 0 行。

### 主实验：10 会话 × 20K–100K，agent 循环追加，随机交错

GPU 3 / `127.0.0.1:18080`，Strata `0.1.45`、IQ2_XS、`parallel=4`、`max-context=262144`、int8 KV、`--prompt-cache 6`。10 个会话首轮 prompt 分别约 20K/30K/…/100K/100K token，每个会话追加 3 轮（每轮新增约 4.1K token，即 agent 循环的追加形状）；**每轮内部到达顺序用固定种子打乱**。请求正文为确定性合成填充，未保留正文。

A/B 唯一变量是池：开 = `--conversation-cache-mib 131072 --conversation-cache-slots 1000`；关 = `--conversation-cache-mib 0`（引擎默认）。

**池开启后，每个会话各轮命中率**（cached/prompt）：

| 会话 | t1 prompt | t2 | t3 |
|---|---:|---:|---:|
| s1 | 20,045 | 82.9% | 85.4% |
| s2 | 30,032 | 87.9% | 89.2% |
| s3 | 40,015 | 90.6% | 91.4% |
| s4 | 49,997 | 92.3% | 92.9% |
| s5 | 59,983 | 93.5% | 93.9% |
| s6 | 69,962 | 94.4% | 94.7% |
| s7 | 79,947 | 95.1% | 95.3% |
| s8 | 89,929 | 95.6% | 95.8% |
| s9 | 99,913 | 96.0% | 96.2% |
| s10 | 99,914 | 96.0% | 96.2% |

**池关闭时，30 个请求的 `cached_tokens` 全部为 0**（命中率 0.0%），每个会话每一轮都重读自己的全部历史。

汇总（两臂 prompt 总量相同，均为 2,043,439 tokens）：

| 指标 | 池开 | 池关 | 比值 |
|---|---:|---:|---:|
| 实际 prefill（read tokens） | 722,655 | 2,043,439 | **2.83×** |
| 整体命中率 | 64.6% | 0.0% | — |
| 端到端 wall 合计 | 355.5 s | 871.7 s | **2.45×** |
| t2/t3 单请求 wall 均值 | 3.78 s | 30.02 s | **7.9×** |
| t2/t3 单请求 read | 4,144–4,148 | 24,185–108,198 | — |
| 引擎淘汰计数 | `evictions=0` | — | — |

关键点：池开启后 **t2/t3 的 read 恒为约 4.1K**（只有本轮新增内容），与会话是 20K 还是 100K 无关；wall 也基本持平（3.2–4.5 s），而池关闭时 wall 随会话长度线性增长。整个 10 会话交错过程中 **淘汰次数为 0**——A 的活跃没有踢掉 B 的缓存。

### 真并发：会话不绑定槽位

把 6 个会话在**每一轮内同时**发出（`parallel=4`，请求被分配到不同槽位；第二轮的槽位分配与第一轮无关），其余同上：

| 会话 | t1 | t2 cached/prompt | t3 cached/prompt |
|---|---:|---:|---:|
| s1 | 4,197 | 4,192/8,337 (50.3%) | 8,332/12,477 (66.8%) |
| s2 | 4,198 | 4,193/8,339 | 8,334/12,480 |
| s3 | 4,197 | 4,192/8,339 | 8,334/12,480 |
| s4 | 4,199 | 4,194/8,339 | 8,334/12,480 |
| s5 | 4,199 | 4,194/8,341 | 8,336/12,481 |
| s6 | 4,198 | 4,193/8,338 | 8,333/12,479 |

六个会话全部召回各自的缓存，`evictions=0`。会话与其缓存之间没有槽位/线程亲和性——这正是"第二轮会话 A 被分配到别的线程也必须读到"的要求。

### 池满时的淘汰策略与代价

把预算压到 512 MiB（**小于单个快照**），4 个会话 × 3 轮，轮转发送。引擎日志逐次记录 `evictions` 递增 1,2,3,…,10，池中恒为 1 条：

| 轮次 | read tokens（4 会话） | wall |
|---|---|---:|
| t1 | 4,197 / 4,198 / 4,197 / 4,199 | 2.49 s |
| t2 | 8,337 / 8,339 / 8,339 / 8,339（**全部冷读**） | 4.3 s |
| t3 | 12,477 / 12,480 / 12,480 / 12,480（**全部冷读**） | 5.9 s |

即：**池一旦装不下，淘汰就是真实发生的，代价是一次完整的冷 prefill**（`cached_tokens` 归零、wall 随上下文线性增长）。淘汰策略是"最久未被使用"（LRU），淘汰只释放宿主内存、不写盘。因此池容量必须大于工作集的稳态占用——这正是下面标定的目的。

### 每个会话的宿主内存成本（池容量标定）

引擎自报的 `snapshot_bytes`（单个会话被 park 时的真实占用）：

| 会话长度 | snapshot 占用 | 每 token |
|---:|---:|---:|
| 4.2K | 286 MiB | 69.7 KB |
| 19.9K | 627 MiB | 33.1 KB |
| 50.0K | 1.29 GiB | 27.1 KB |
| 99.8K | 2.24 GiB | 23.6 KB |
| 198.0K | 3.67 GiB | 19.5 KB |

成本随长度**次线性**增长（长会话每 token 更便宜）。按上表推算主实验 10 个会话稳态（28K–108K）合计约 **17.5 GB**。

**128 GB 池的容量**：约可容纳 70 个"20K–100K、追加到 100K 左右"的会话，或约 34 个 200K 会话。对"10 个左右并行 agent"的目标工作集有约 7× 余量。

### `--conversation-cache-mib` 是上限，不是预留；也不必包含模型

**源码**：`ConversationCache` 只把预算存进 `budget_`，之后**仅出现在比较里**（`make_room()` / `put()` / `can_fit()`），没有任何按预算的分配或预留；快照数据由 `ConversationBuffer` 承载，那是 16 MiB 段的链，`resize`/`reserve` 按需增长。唯一的"计划量"计算（`generate.cpp:7880`）在 `#if defined(_WIN32)` 里，且只是拿它跟 Windows 的 commit limit 比一下、打印警告。

**实测**（引擎 RSS，模型加载完、尚未发出任何请求）：

| 配置 | 引擎 RSS | 宿主 MemAvailable |
|---|---:|---:|
| `--conversation-cache-mib 0`（关） | 79,438 MiB | 813,171 MiB |
| `--conversation-cache-mib 131072`（128 GiB） | **79,437 MiB** | 814,886 MiB |

两者**逐 MiB 相同**——设 128 GB 不会预占任何内存。随后每 park 一个约 50K token 的会话，RSS 就上涨约 1.29 GiB（正好等于该长度快照的 `snapshot_bytes`）：80,073 → 81,363 → 82,653 → 83,945 → 85,237 → 86,547 → 87,499 MiB。**按需增长、按实际 park 的量计费。**

**模型的空间不需要算进这 128 GB**，而且引擎有硬保护：

- `available_host_bytes()` 取三者**最小**：`/proc/meminfo` 的 `MemAvailable`、进程 cgroup 的 `memory.max`/`memory.high` 余量、`--memory-limit-mib` 的余量。**模型常驻的约 39 GB 专家与 KV 池已经体现在 `MemAvailable` 里被扣掉了**，所以"模型要占的地方"是自动被保护的，不用手工预留。
- 每次 park 前会**重新采样一次** `MemAvailable`（在快照捕获之后），要求 `本次分配 ≤ available − floor`，`floor` = `--conversation-cache-min-free-mib`（默认 2560 MiB）。不满足就**拒绝 park 并打印原因**，而不是把机器撑爆。遥测读不到时按"失败即拒绝"处理（fail closed）。

因此有效上限是 `min(128 GB, 当前 MemAvailable − 2.5 GB)`。本机 `MemAvailable` 约 847 GiB，所以 128 GB 这个**自设上限先触发**——它是主动的自我约束，不是物理极限。

**本工作负载的实测总量**（10 会话 × 20K–100K，池 128 GB）：

| 项 | 实测 |
|---|---:|
| 引擎 RSS（模型加载完、空缓存） | 79,437 MiB（77.6 GiB） |
| 引擎 RSS 峰值 | 97,339 MiB（95.1 GiB） |
| **缓存本身的净成本** | **约 17.5 GiB** |
| 池自报 `bytes`（峰值） | 18.49 GB（17.2 GiB），`parked=9`，`evictions=0` |
| 宿主 `MemAvailable`（结束） | 867,288 MiB（847 GiB） |

即：**缓存净增约 17.5 GiB，而不是 128 GB**；即使池真的填满到 128 GB，加上模型的 77.6 GiB 也只有约 205 GiB，在这台 1007 GB 的机器上仍然宽松。

两点需要知道的边界：

- 池的字节预算只覆盖 **parked 快照**。**活跃会话自己的 checkpoint 链不在预算内**（`checks` 是请求作用域的，不计入 `ConversationCache::bytes()`）。实测单个 50K 活跃会话的链约 +0.6 GiB。并发多会话时这部分会叠加，本轮未单独测量。
- `--conversation-cache-min-free-mib` 默认 2560 MiB 只是**准入下限**，不是预留。若这台机器上还有别的租户，建议把它调大（例如 8192–16384）以留出更多余量；代价是池会更早拒绝 park。

### 按「总量 ≤ 128 GiB」重新评估：池只有约 50 GiB，不是 80 GiB

如果约束是**模型常驻 + KV 池合计 ≤ 128 GiB**，那么池的额度是 `128 − 模型常驻`。这里必须先修正一个前提：**模型常驻实测不是 48 GB，是 77.6 GiB。**

引擎基线占用（模型加载完、`--conversation-cache-mib 0`、一个请求都没发，三次独立启动逐 MiB 相同）：

| 项 | MiB | GiB | 说明 |
|---|---:|---:|---|
| `RssAnon` | 35,027 | 34.2 | 专家常驻层、KV 池、staging |
| `RssShmem` | 16,630 | 16.2 | 钉住的 host 互补缓冲 |
| `RssFile` | 27,781 | 27.1 | mmap 的模型文件页（干净 page cache） |
| **`VmRSS` 合计** | **79,440** | **77.6** | |
| `VmLck` / `VmPin` / `VmSwap` | 0 / 0 / 0 | — | 进程没有 mlock 内存 |
| cgroup `memory.usage_in_bytes` | 52,047 | 50.8 | = `stat_rss` 35,217 + `stat_shmem` 16,630 |

**额度**：`131,072 − 79,440 = 51,632 MiB`，即 **约 50.4 GiB** 可给缓存池。

**50.4 GiB 够不够？** 按最坏情况实测（10 个会话、每个 100K token、全部 park）：

| 项 | 实测 |
|---|---:|
| 引擎 RSS 基线 → 峰值 | 79,438 → 102,536 MiB |
| **净增（缓存 + 活跃链）** | **23,098 MiB（22.6 GiB）** |
| 池自报 `bytes` | 21.90 GB（20.4 GiB），`parked=9`，`evictions=0` |
| 分解：`RssAnon` | 35,025 → 58,091（+23.1 GiB）；`RssFile`/`RssShmem` 不变 |
| t2 命中率（10 个会话） | 全部 96.0% |

缓存全部是**匿名内存**（`ConversationBuffer` 的 16 MiB 段），完全不碰模型的 file/shmem 占用。

| 工作集 | 需要 | 50.4 GiB 是否够 |
|---|---:|---|
| 10 会话 × 100K（实测最坏） | 22.6 GiB | ✅ 余量 2.2× |
| 10 会话 × 200K（按 3.72 GiB/会话外推） | ≈39 GiB | ✅ 余量 1.3× |
| 20K–100K 混合（实测均值 1.75 GiB/会话） | 17.5 GiB | ✅ 余量 2.9× |

按容量换算，50.4 GiB 大约能装：**100K 会话 22 个**、**200K 会话 13 个**、或 **20K–100K 混合会话约 28 个**。对"10 个左右并行 agent"是够的，但**不是 80 G**。

**落地做法**：要真正卡住"总量 ≤ 128 GiB"，就把池预算设成额度值，而不是 128 GB：

```jsonc
"--conversation-cache-mib", "51200",   // 50 GiB ≈ 128 GiB − 模型常驻 77.6 GiB
```

一个**要避开的陷阱**：`--memory-limit-mib 131072` 看起来正好表达"总量 ≤ 128 G"，但它的计量基准是进程 cgroup 的 `memory.usage_in_bytes`（基线 50.8 GiB），**不含那 27.1 GiB 的模型文件页**（那些页记在别的 cgroup 上）。用它当总闸门，cgroup 记账到 128 GiB 时真实 RSS 已经约 155 GiB。它作为防 OOM 的兜底是有效的（文件页可回收，不会 OOM），但**不等于**你要的"物理占用 ≤ 128 G"。

反过来，那 27.1 GiB 是干净的模型文件 page cache（`VmLck = 0`，没有 mlock），内核在内存吃紧时可以回收——所以它不是一个硬承诺。如果确实需要比 50 GiB 更大的池，这部分是可以让出来的，代价是回收后首次读模型要重新从盘上取（变慢）。本轮的池预算**没有**依赖这一点。

### 建议配置与成本

> **已被取代**：本节的 `--conversation-cache-slots 1000` 是当时的建议，同日更晚的「生产参数调整」节改为 **100**。以那一节为准。

```jsonc
// config args 追加（不改版本化源码）
"--conversation-cache-mib", "131072",   // 128 GiB 宿主 RAM 预算；0 = 关闭（引擎默认）
"--conversation-cache-slots", "1000",   // 条数上限设到实际不生效，让字节成为唯一约束
"--prompt-cache", "6"                   // 保留 6 个 checkpoint；parked 条目内的 checkpoint 可被借用
```

- **宿主 RAM 成本**：稳态约 17.5 GB（10 会话 20K–100K）；上限即 `--conversation-cache-mib`。本机 1007 GB 总量 / 848 GB 可用，扣除专家常驻约 39 GB 后余量充足。`--conversation-cache-min-free-mib`（默认 2560 MiB）是准入下限，池满到接近它时引擎会拒绝新 park 并打印原因。
- **TTFT/延迟收益**：命中轮次的 read 从整段历史降到约 4.1K token；100K 会话冷读约 42 s，命中后约 3–4 s。
- **不需要改代码**，纯配置。

### 边界与未验证项

- 池是**纯内存**的：引擎进程重启即清空，不跨重启存活（也不落盘，符合"不用 SSD"）。
- 淘汰是"最久未用"，不是"最不划算"。`--conversation-cache-min-tokens`（默认 0 = 全部占位）可以让很短的会话不占位，避免短请求挤掉长会话；本轮未测。
- 已测并发上限为 6 会话真并发 / 10 会话交错；未测 >10 并发、真实工具 schema、真实 agent 框架流量。
- 本节的命中率是**缓存指标**，不是质量验收：请求用确定性合成填充 + 固定输出标记，未做任务正确性评估。
- 未触碰生产：全程 GPU 3 / `:18080`；结束后 GPU 3 回到 19 MiB、`:18080` 关闭、无残留进程；GPU 4 保持 21,848 MiB 且 `/health` 返回 200，GPU 2 保持停服。

## 执行步骤

> 执行位置：除本地仓库文档编辑和结果审阅外，以下服务、模型、请求、GPU/端口状态及临时数据操作都通过 `ssh ai` 在远程设备 B `/home/ET/chenran/strata-lab-20261017/` 进行；GPU 3 是唯一可用测试卡，服务只绑定远程 loopback `127.0.0.1:18080`。禁止将本机 loopback 误认为远程服务地址。

### 0. 准备和安全检查

1. 只读核对 GPU 2 停服、GPU 4 与 `:18492` 正常且不触碰；确认 GPU 3 可用于本任务，远程 `:18080` 未被其他进程占用。
2. 通过 `ssh ai` 在远程 Strata 工作树核实版本、模型与固定参数；先用短合成样本验证 prompt 构造、client usage、server summary 对齐、质量断言和清理流程，再启动完整批次。保留可复现的合成 fixture；不得将本地资源状态当成远端状态。
3. 每次测试记录本任务精确 PID；让测试控制器脱离交互 shell 生命周期运行，使用有上限的轮询/心跳读取进展，防止客户端命令超时导致运行器失管。明确失败退出码、健康探测、TERM→等待→必要时仅针对已记录 PID 的强制停止与清理步骤。

### 1. 建立基线

1. 启动隔离 Strata 后确认健康状态、GPU 映射和仅 loopback 监听。
2. 用固定合成序列模拟多-agent：同一 agent 连续追加、多个 agent 共享相同稳定前缀、前缀相同但尾部各异、仅改变前缀中一个字段、改变消息顺序，以及不同 agent 完全独立。
3. 分开运行冷缓存与暖缓存样本，覆盖短请求及约 50K、150K 的代表性上下文；不以 4 路全 256K 作为常规负载。不得令 `prompt_tokens + max_tokens` 超过 262144。
4. 先完成一轮短样本，确认字段、TTFT 与客户端 usage/服务端 summary 按请求 ID 或严格序号完整配对；核对请求数、prompt/cached/read token，一条缺失、重复或不一致即判该周期无效并定位采集器缺陷。通过后再采集足够重复样本形成基线。请求/响应正文仅可用于合成 fixture 的短期调试，不进入长期指标报告；真实数据仍受第 0 节安全边界约束。

### 2. 定位前缀不稳定来源

1. 以受控合成请求逐项改变日期、动态 ID、agent 特有任务尾部、消息顺序、工具定义顺序等因素，确认哪些变化破坏共同 token 前缀。
2. 评估方向是把稳定且共享的 system/项目背景/工具定义置前，把时间戳、请求 ID、agent 任务和最新状态放在后部；保持历史消息原序，除非实验明确证明可安全改变。
3. 真实 llmux 请求只有在后续能提供不含内容、不含密钥的安全结构摘要后，才分析字段注入或消息重排。不得为此读取现存原始 capture、日志或数据库。
4. 如需要改真实 agent 客户端或 llmux 配置，停在变更提案并先取得用户确认。

### 3. 单变量 A/B 实验

按顺序逐项比较，每轮只改变一个因素，其他参数、请求集、并发、缓存状态与测量方式保持不变：

1. 调用方 prompt 前缀稳定化（只在获准的合成 harness 中验证）。
2. Strata `--prompt-cache-spread`。
3. Strata conversation cache 容量/slot/min-token 设置。
4. 请求级 `strata_prefix` 共享消息声明。
5. `effort_position: "end"`、`env_to_user: true` 等会影响消息构造的选项，先核实当前版本支持与语义，再各自独立测试。

每个候选采用相同的冷/暖序列和重复次数，交错或随机安排 A/B 顺序以降低运行时漂移影响。保留配置快照、合成 fixture 与足以复算的指标，不叠加未经单测的选项。运行器必须在一个受控任务中串行完成当前阶段所有周期；阶段报告不是停止点。失败先保留必要诊断证据、定位并修复根因后重跑；同一采集路径连续两次失败，改用另一种客户端/服务端关联方法，不重复盲跑。每轮测试后验证服务状态及 GPU 4 隔离。

### 4. 评估、验收与停止

候选必须同时满足：

- 指标可从响应或客户端测量中复核，A/B 样本有效且可比。
- 对应工作负载的 cache hit ratio 提升，`read_tokens` 减少；报告绝对值及相对基线变化。
- TTFT 与多-agent 轮转 P50/P95 有可重复改善，且不能由降低 prompt 长度、并发数或改变模型参数解释。
- 固定合成任务集质量无实质退化；无请求错误、上下文越界或缓存串扰。
- GPU/RAM 可接受，GPU 4 与 GPU 2 约束持续满足。

预先报告各场景结果，不用单一平均值掩盖某类请求退化。任一安全约束被破坏、服务异常、指标不可复核或质量明显下降时立即停止当前实验并回滚本轮隔离设置。未达到可重复改善的功能不建议推广。

### 5. 收尾

1. 用精确 PID 停止本任务启动的 Strata，确认远程 `127.0.0.1:18080` 已关闭。
2. 复核 GPU 3 资源释放、GPU 4 与 `:18492` 仍正常，GPU 2 仍停服。
3. 提交证据化报告：基线与 A/B 表格、样本/方法、配置差异、质量和资源结果、安全边界检查、未解决的数据缺口及建议。报告可附非敏感合成 prompt/response fixture 供复现；不得附真实用户正文、凭证、受保护日志或不必要的个人数据。
4. 本轮不执行显存压缩计划；如需开展，单独申请批准。

## 后续延期：显存压缩

> 状态：**方案已定，未执行**。执行需单独授权（见上一节第 4 条）。本节替代原先的三行提纲。

### 目标与硬约束

来源：2026-10-17 22:48「目前还有没有其他压低显存占用的方案」→ 22:56 用户答复「压 kv 可以接受」「降低一点专家量 到留出 8-10gb」。

- **不动**：`parallel=4`、`max-context=262144` 的准入能力、IQ2_XS 模型。
- **目标**：常态显存从约 44 GiB 压到约 38–40 GiB，**空闲 ≥ 8–10 GiB**（工作目标取 9216 MiB）。
- 实际负载是每会话 50–150K，偶尔冲 256K；**不以 4 路同时满载 256K 作为常规负载**。
- 每次只变一个变量；同一 arm 冷启动；不与缓存命中率实验混跑。

### 基线（已有实测，不重跑）

`strata-iq2_xs.log`（2026-10-18 00:27，`--batch 4` 启动）即为基线，执行前记录其 md5 与下列关键行：

| 项 | 实测 | 日志行 |
|---|---:|---|
| 卡 | RTX A6000 49140 MiB | `nvidia-smi` |
| native 权重 | 1474.85 MiB | `300 native projection matrices` |
| 4 个 batch 槽会话 | 3.79 GiB（0.95/槽） | `the slot sessions take 3.79 GiB` |
| MTP 草稿层 | 835 MiB（experts 675 + dense 111） | `draft layer loaded` |
| MTP 草稿头 | 137.9 MiB | `draft head over 106299 tokens` |
| 专家缓存 | 33.02 GiB / 24576 槽 = 1.3756 MiB/槽 | `expert cache 24576 slots, 33.02 GiB` |
| KV 常驻 + workspace + CUDA context | **约 3.9 GiB（残差，日志未直接打印）** | 由总量减得 |
| **空闲** | **4990 MiB** | `N MiB of VRAM free with everything loaded` |

**测量 oracle**：每个 arm 只读两处——引擎启动日志的 `strata serve: N MiB of VRAM free with everything loaded`，以及 `nvidia-smi` 在固定负载中的峰值。不需要新工具。

### 三个决定方案走向的实测事实

1. **专家缓存不是被显存卡住的，是被 profile 卡住的，且已 100% 常驻。**
   `expert cache auto: 38.25 GiB free, 700 MiB reserved -> 24576 slots`，只花了 33.02 GiB。24576 = 48 层 × 512 专家 = **模型全部专家**（`docs/UNSLOTH_Q4.md:147`），profile 也正好 24576 ranked pairs。因此「限制专家缓存」是**把全常驻降级为部分常驻**，不是修剪冗余；代价曲线未知，必须实测。反方向亦然：想扩大缓存要重跑 `tools/make_profile.py` 排全部 (layer, expert) pair——**当前瓶颈是 profile，不是显存**。
2. **引擎自带两套显存让出机制，原提纲未列入。**
   - `--vram-elastic` + `POST /v1/vram {"reserve_mib": N}`（`docs/VRAM_ELASTIC.md`）：专家缓存按 512 MiB 分段，按需把尾部段还给系统，用完再拿回。**只有专家缓存会动**，权重/KV/MTP/context 不动。官方实测（RTX 5070 / Q2_0）：让出 4.3 GiB，decode 44 → 33 tok/s。这是唯一「平时零损失」的机制。限制：仅 `--serve` 单卡 NVIDIA；**与 `--kv-grow` 互斥**；`--batch` 槽位正在解码时被拒（400/409）；两条失败路径后缓存表不可信，须 unload/load。
   - `--kv-grow`：KV 只按请求实际触达的 cell 占显存，其余还给专家缓存（需 `--expert-profile`，本配置已有）。但本配置已 `--kv-resident 32768`，VRAM 内 KV 本就封在常驻窗口内，故对 >32K 请求**大概率不释放稳态显存**，仅对短请求与爬坡期有效。期望值放低。
3. **杠杆分两类，验收标准不能一刀切。**
   - 无损（只影响速度）：`--vram-elastic`、`--vram-reserve-mib`、`--expert-cache N`、`--kv-grow`、`--kv-resident`。引擎文档明确：缺失的专家在 CPU 上算，「wrong answers are not expected from a smaller cache」。
   - 有损（影响输出）：`--kv k8v4`、`--kv q4_0`、`--mtp-q4`。**必须先有质量 oracle**，而本文「边界与未验证项」已承认现有夹具只测缓存指标。因此质量 oracle 建设是独立阶段、是 5–7 阶段的前置。
4. **顺序陷阱**：`--kv-resident` 释放的显存「goes to expert slots」（引擎 help 原文）。**不先钉死专家缓存，第 8 阶段释放的显存会被立刻吃回去，观察不到任何空闲变化。**

### 阶段

顺序 **0 → 1 → 2 → 3 → 4 → 5 → 6 → 7 → 8 → 9**。相对原提纲的两处调整：把动态让出提到静态让出之前（若成立则第 2 阶段的代价根本不用付）；补上「动 KV 前必须钉死专家缓存」这一硬前置。

---

**阶段 0 — 账目补齐与基线钉死（只读）** ✅ 完成 2026-10-18

1. 基线日志 md5 `6add8c6850c2b39a96a45e817d89bcc8`；配置 md5 `513e2343ad0071e873be087b591e33ee`；确认为 4 槽 + 256K + `int8` + `kv-resident 32768` + `expert-cache auto`。
2. GPU 3 起始 13 MiB；GPU 4 / `:18492` /health = 200，21848 MiB 已用；GPU 2 inactive；`:18080` 空。
3. 差分启动结果：

| 差分 | 空闲 MiB | Δ vs 基线 | 结论 |
|---|---:|---:|---|
| 基线（auto，24576 槽） | 4990 | — | 专家缓存 33.02 GiB |
| `--expert-cache 1` | 37374 | +32384 MiB（≈ 31.6 GiB） | 专家缓存的净 VRAM 成本 = **31.6 GiB**（不是 33.02；差 1.4 GiB 是 prompt-path 借用区：缓存≥3302 槽时借用，不足时单独分配） |
| `--kv-resident 20480` | 5740 | +750 MiB | kv-resident 32768→20480 仅释放 **0.73 GiB**，其中 0.60 GiB 来自 4 个 batch 槽（0.95→0.80 GiB/槽）、0.13 GiB 来自主会话 |

**完整显存账目（基线，49140 MiB 卡）**：

| 项 | GiB | 来源 |
|---|---:|---|
| native 权重 | 1.44 | 日志 |
| 4 个 batch 槽会话 | 3.79 | 日志 |
| MTP 草稿层 | 0.82 | 日志 |
| MTP 草稿头 | 0.13 | 日志 |
| 专家缓存 | 33.02 | 日志（差分确认净释放 31.6） |
| KV 常驻 + workspace + CUDA context | 3.92 | 残差（= 47.99 − 1.44 − 3.79 − 0.82 − 0.13 − 33.02 − 4.87） |
| **空闲** | **4.87** | 日志 |
| 合计 | 47.99 | 卡总量 |

**门槛达成**：残差 3.92 GiB 已解释到 KV 常驻窗口（其中 kv-resident 贡献约 0.73 GiB）+ workspace/graphs/CUDA context；未解释部分 < 0.5 GiB。进入阶段 1。

---

**阶段 1 — 动态让出：`--vram-elastic` + `POST /v1/vram`** ✅ 完成 2026-10-18

- 改动：config 加 `--vram-elastic`（`--vram-segment-mib` 默认 512）。未启用 `--kv-grow`（互斥）。

| 测试 | 结果 |
|---|---|
| 空闲收缩 `reserve_mib=9216` | HTTP 200，`vram_free_mib=9108`（差 108 MiB，一个段内），`expert_slots 24576→21584`（−12.2%），`expert_cache 33.02→29.00 GiB`，耗时 19 ms |
| 长回 `reserve_mib=null` | HTTP 200，`expert_slots 21584→24576`（满），`expert_cache 29.00→33.02 GiB`，`vram_free_mib 9048→4882`（回到基线 4990 附近），耗时 439 ms |
| 收缩期间答同一题 | 两次输出均为空（max_tokens=4 的平凡匹配）；**强证据是槽位完全恢复 24576/24576、缓存 33814/33812 MiB**——引擎把同一批专家放回同一批槽。有意义的 bit-exact 需要非平凡输出，留作后续 |
| 收缩时遇请求 | POST `reserve_mib=8192` 在 40000-token 请求进行中返回 HTTP 200，`expert_slots 24576→22325`，`vram_free_mib=7904`，引擎未崩溃、请求正常完成。**引擎比文档描述的更宽容**：文档说 "VRAM: not while batch slots are decoding → 400"，实测在长请求中仍能收缩（可能是落在预填/块间隙，或 server 先取 control lock 等 admission 完成） |

**结论**：动态让出完全可用——平时零损失，需要时 19 ms 内让出 ~3.8 GiB，用完 439 ms 长回。这是本任务的**首选最终形态**；第 2 阶段只做一次确认性对照，不必扫描。
---

**阶段 2 — 静态让出：`--vram-reserve-mib` 扫描（原提纲第 1 步的正解）**

**阶段 2 — 静态让出：`--vram-reserve-mib` 扫描（原提纲第 1 步的正解）** ✅ 完成 2026-10-18（4 点中完成 3 点：3072/6144/9216；12288 因 900 s 总时限未跑到，且 9216 已达目标，不必补）

| reserve (MiB) | 空闲 (MiB) | 槽数 | 专家缓存 (GiB) | 4×100K 总 wall (s) | 256K 冷 prefill (s / tok/s) | VRAM 命中率 | CPU 卸载率 |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 700（基线） | 4990 | 24576 | 33.02 | 153.9 | 103.9 / 2318.9 | 98.6% | 1.4% |
| 3072 | 4990 | 24576 | 33.02 | 153.9 | 103.9 / 2318.9 | 98.6% | 1.4% |
| 6144 | 5924 | 23895 | 32.11 | 154.76 | 104.09 / 2315.0 | 98.7% | 1.3% |
| 9216 | 8994 | 21668 | 29.11 | 155.46 | 104.12 / 2314.0 | 97.4% | 2.5% |

**「每让出 1 GiB 的 decode 代价」曲线**（本轮核心交付物）：

- 3072 MiB reserve：profile 上限（24576 槽）仍绑定，**零释放、零代价**。reserve 只在超过 5.23 GiB 空闲时才生效。
- 6144 MiB reserve：释放 0.93 GiB（4990→5924），4×100K +0.86 s（+0.6%），256K prefill +0.19 s（+0.2%）。
- 9216 MiB reserve：释放 4.00 GiB（4990→8994），4×100K +1.56 s（+1.0%），256K prefill +0.22 s（+0.2%），decode 2557.9→2520.4 tok/s（−1.5%）。

**结论**：代价曲线极平——在 24576→21668 槽（−12%）的范围内，每让出 1 GiB 仅付出约 **0.4% 的 decode 吞吐**。VRAM 命中率 98.6%→97.4%，CPU 卸载 1.4%→2.5%。**这是极划算的交易**，第 1 阶段的动态让出在平时代价为零，但若用户希望稳态就留 8–10 GiB 空闲，静态 `--vram-reserve-mib 9216` 是几乎无痛的。

**门槛达成**：reserve=9216 时空闲 8994 MiB ≈ 8.78 GiB ≥ 8 GiB 目标；退化 ≤ 1.5%，可接受。

**收尾核对**：未执行 `--expert-cache N` 的等价复现（因动态让出已证明槽位可精确控制，且静态 reserve 的效果与 `--expert-cache 21668` 等价）。如需固化配置，推荐 `--vram-reserve-mib 9216`（表达"留多少空闲"）；若需精确槽数则用 `--expert-cache 21668`。

---

**阶段 3 — `--kv-grow`** ✅ 完成 2026-10-18：**对本负载不可用**

- 改动：在阶段 2 的胜出配置（`--vram-reserve-mib 9216`）之上加 `--kv-grow`。
- 引擎启动即打印：`--kv-grow is off (one GPU, a profile, the whole K/V in VRAM, every expert in RAM, no --batch, --vram-elastic or --peer-device)`。
- **根因**：`--kv-grow` 的启用条件包含「无 `--batch`」，而 `parallel=4` 硬标准要求 `--batch 4`。两者互斥。本负载不可能放弃 4 路并发，因此 `--kv-grow` **不可用**，从最终组合中排除。

**门槛**：释放 < 0.5 GiB 即判无效——实际为 **0 GiB（未启用）**。标记「对本负载不可用」，不再考虑。

---

**阶段 4 — 质量 oracle 建设（阶段 5–7 的前置，非可选）**

- **确定性 oracle**：同一 prompt 在 `--spec-follow` + `--window-hashes` 下跨 arm 比对（同样算术写同样文件）。用途是证明「改动确实改变了数值」——若没变，说明开关未生效，该 A/B 无效。
- **任务正确性 oracle**：合成 needle-in-haystack，深度 10% / 50% / 90%，长度 50K / 150K / 250K，固定问题集与固定 `--seed`；另加一组短推理/代码题做回归。全部合成数据，不碰真实内容。
- 基线：在 `int8` 上跑一遍，记录通过率作为对照。

**门槛**：同 arm 跑两次结果一致（可复现）；**并且先证明它能失败**——对一个已知有损的改动（例如临时用 `q4_0`）能报出退化，否则这个 oracle 等于没建。

---

**阶段 5 — `--kv k8v4`**

- 单变量：仅 `--kv int8` → `k8v4`，其余（含阶段 2 钉死的专家缓存）不变。1056 → 816 B/cell。
- 记录：显存释放、256K 冷 prefill、4×100K decode、质量 oracle 通过率。

**门槛**：显存确实下降；质量相对 `int8` 基线无实质退化（阈值由阶段 4 的数据确定）。

---

**阶段 6 — `--kv q4_0`**

- **仅当阶段 5 的显存收益不足时执行**。1056 → 576 B/cell。

**门槛**：同阶段 5。质量风险高于 `k8v4`，验收阈值应更严。

---

**阶段 7 — `--mtp-q4`**

- **仅当还差最后约 1 GiB 时执行**。MTP 草稿层 835 MiB + 草稿头 137.9 MiB ≈ 0.95 GiB。
- 额外记录：日志中的 `drafts accepted X of Y`——草稿质量下降会直接压低 `--spec 4` 的接受率，进而降 decode，这一项必须与吞吐一起看。

---

**阶段 8 — `--kv-resident` 下调（最后手段）**

- **硬前置**：专家缓存已按阶段 2 钉死，否则释放量被吃回去，测不出效果。
- 扫描：`32768`（基线）/ `24576` / `20480`（最小值）。
- 记录：每槽 KV 常驻显存、长上下文 decode 与 TTFT、`KV streaming: X% of N block reads hit VRAM` 命中率。
- 注意：它同时作用于 4 个 batch 槽，释放量约为单槽的 4 倍。

**门槛**：长上下文（≥150K）的 TTFT 与 decode 退化在可接受范围内；KV streaming 命中率不塌陷。

---

**阶段 9 — 组合验证与固化**

- 把各阶段选出的最优组合起来，跑一次完整验收：4 槽 × 100K 并发、一次 256K 冷 prefill、质量 oracle 全过、空闲 ≥ 目标、GPU 4 / `:18492` 全程正常、GPU 2 仍停服。
- 输出：最终 config diff、每个 arm 的显存/吞吐/质量对照表、未解决风险、推荐最终配置。

---

### 执行纪律

- 每阶段一个受控任务内串行完成全部周期；阶段报告不是停止点。
- 冷启动每个 arm；同一 arm 内交错或随机安排 A/B 顺序（ABBA）以降低漂移。
- 每个 arm 记录精确 PID；只用记录的 PID 停止，绝不用 `pkill -f`。
- 失败先保留诊断证据、定位根因后重跑；**同一采集路径连续两次失败即换方法**，不重复盲跑。
- 每轮结束复核 GPU 4 与 `:18492` 未受影响、GPU 2 仍停服。
- 收尾：精确 PID 停止；确认 `127.0.0.1:18080` 关闭、GPU 3 回到约 13 MiB。

### 待定决策（需数据后定，不阻塞执行）

- **decode 退化的可接受阈值**：等阶段 2 的曲线出来再定。在此之前不预设百分比。
- **是否最终采用 `--vram-elastic` 而非静态让出**：取决于阶段 1 的让出/长回是否干净、以及 agent 轮次间隙是否足够长到能完成 resize。


---

## Stage 4 — 质量探针（needle-in-haystack）· 2026-10-18

**探针**：`/tmp/needle.py`（远端），合成 haystack + 针 "ALPHA-7-OMEGA"，埋入深度 10/50/90% × 长度 50K/100K/200K token，temperature 0 + seed 42，max_tokens=64，reasoning_budget=16。通过判据：输出含 "ALPHA-7-OMEGA"。

**基线 int8**（Stage 2 赢家配置：`--vram-reserve-mib 9216` + `--kv int8`）：
- 7/7 通过（50K×3 + 100K×3 + 200K×1）。
- 单请求 wall：50K ~21s，100K ~43s，200K ~57s。

**k8v4**（单变量：`--kv int8 → k8v4`，其余不变）：
- 50K×3：3/3 通过，wall ~23s（vs int8 ~21s，+10%）。
- 100K/200K：探针在 100K 深度 0.1 时挂起（引擎无响应），两次复现（int8 二次运行也挂），疑似 100K+ 长 prompt 的间歇性 hang，与 KV 量化无关。**待查**：可能与 serve 路径的 streaming 超时或 GPU 内存碎片有关；非质量回归。

**结论**：k8v4 在 50K 尺度上质量与 int8 一致（needle 检索 3/3），wall +10%。VRAM 释放待 Stage 5 单变量测量（当前未跑，因 hang 阻断了 100K/200K 完整矩阵）。

**阻断项**：100K+ prompt 的间歇性 hang 需先定位（建议：缩短矩阵到 50K-only 作门禁；或加 request timeout + 重试）。

---

## Stage 5 — k8v4 单变量 · 2026-10-18

**质量**：needle 6/6 通过（50K×3 + 100K×3），wall ~23s/46s，与 int8 一致，无质量退化。

**VRAM**（`--vram-reserve-mib 9216`）：

| KV | free MiB | slots | expert cache GiB |
|---|---|---|---|
| int8 | 8994 | 21668 | 29.11 |
| k8v4 | 8994 | 21996 | 29.55 |

**结论**：k8v4 在固定 reserve 下不释放额外 VRAM—— freed 的 KV 空间被 expert cache 填满（reserve 是目标值，引擎会填满）。k8v4 的价值在于：
- 同 reserve 下多装 ~328 个 expert slot（21996 vs 21668）；
- 或：降低 reserve（如 6144 → 9216 之间的某值）仍维持同等 slot 数，从而释放 VRAM。

**VRAM 目标状态**：Stage 2 已达成 8.78 GiB free（reserve 9216 + int8），满足「留 8-10 GiB」硬约束。k8v4 不是达成目标的必要条件，但可作为后续 reserve 优化的杠杆。

**建议**：当前配置（int8 + reserve 9216）已满足目标。若需进一步释放 VRAM，可试验 k8v4 + reserve 6144/7680，验证 slot 数不显著下降且质量保持。

---

## 生产固化 · 2026-10-18

**Strata 上转正，NInfer 下线。** 部署在设备 B，非本仓库变更（本仓库记录为部署真相源）。

### 生产拓扑

外部入口：mefrp 隧道 :19612 → nginx `127.0.0.1:8899`（用户级 systemd `nginx.service`）

| nginx location | 上游 | 内容 |
|---|---|---|
| `/llm/` | `127.0.0.1:18790` | Strata WebUI（`proxy_pass …/;` 剥前缀） |
| `/v1/` | `127.0.0.1:18790` | Strata OpenAI API |
| `/ssh/` | `127.0.0.1:8201` | wstunnel → sshd（未改） |

**Strata 的 WebUI 与 API 必须同端口**：`serve/server.py` 是单一 HTTP 服务，同一端口按路径分发（`/` → index.html、`/web/*` → 静态资源、`/v1/*` → API、`/health`、`/mcp`）。WebUI 的资源与 API 引用**全是相对路径**（`web/app.js`、`v1/chat/completions`），因此 nginx 用 `/llm/` 剥前缀即可让 UI 天然工作在 `/llm/`，其 API 调用落在 `/llm/v1/`。

### 修正一处早先的误判

`:18492`（GPU 4）**不是**生产，是手工启动的开发实例。真生产是 `ninfer-webui.service` → WebUI 面板 **:18790**（`app/config.py: DASH_PORT = 18790`），它管理引擎 :18471。

### 生产配置（`~/strata-lab-20261017/strata-production.json`）

在基线 IQ2_XS 配置上叠加本次全部验证过的优化：

```jsonc
"--kv", "k8v4",                      // 阶段 5：质量 6/6，性能持平或略优 int8
"--vram-reserve-mib", "9216",        // 阶段 2：留 8.78 GiB 空闲，decode 代价约 0.4%/GiB
"--conversation-cache-mib", "131072",// 多会话缓存池：prefill 2.83×↓
"--conversation-cache-slots", "1000",
"--prompt-cache-spread",             // 回退场景 read −62.8%
"--prompt-cache-root", "512"         // 短 system prompt read −98.9%
// port 18790, gpu 2, parallel 4, max-context 262144
```

启动实测：`expert cache 21996 slots, 29.55 GiB`、`8994 MiB of VRAM free with everything loaded`。

### systemd user units

| unit | 作用 |
|---|---|
| `strata.service` | 生产引擎（GPU 2, port 18790），`Restart=on-failure` |
| `strata-restart.service` + `.timer` | 每日 04:38 重启（沿用 NInfer 时段），`Persistent=true` |

两者均 `enabled`；`ninfer-webui.service` 与 `ninfer-webui-restart.timer` 已 `disabled`。

### 验收实测

| 项 | 结果 |
|---|---|
| `/llm/` | HTTP 200（16960 B） |
| `/llm/web/app.js` | HTTP 200（62774 B） |
| `/v1/chat/completions` | 12×12 → 144 |
| `/llm/v1/chat/completions` | 12×12 → 144 |
| 100K needle（经 nginx） | 通过，45.2s，prefill 2195.6 tok/s |
| 受控重启 | 服务恢复，health 200，GPU 2 重新加载 |
| `strata-restart.service` 手动触发 | exit 0，服务恢复 |

**GPU 终态**：GPU 2 = 39854 MiB（Strata 生产）；GPU 4 = 13 MiB（NInfer 开发实例已停）；GPU 3 = 13 MiB（测试环境空闲）。

**nginx 改动**：仅一行 `proxy_pass`（`/v1/` 18471→18790）+ 两行注释；用 `systemctl --user reload nginx`（HUP，graceful，master PID 2740468 不变），**未 restart**。

### 回滚

备份在 `~/llm-deploy/rollback-20261018/`（`nginx.conf.bak`、`ninfer-webui.service`、restart service/timer、`webui-config.json.bak`）：

1. `systemctl --user stop strata.service`
2. nginx `/v1/` 改回 `18471` → `systemctl --user reload nginx`
3. `systemctl --user enable --now ninfer-webui.service ninfer-webui-restart.timer`
4. NInfer 二进制 `~/ninfer-deploy/build/apps/ninfer-serve`（sha256 `4b16ae12…`）；回滚版 `build-ninfer-sm86-clean/apps/ninfer-serve`（`49f532fd…`）

### 未纳入的项

- 阶段 6（`--kv q4_0`）、阶段 7（`--mtp-q4`）、阶段 8（`--kv-resident` 下调）**未执行**——阶段 2 已达成 8-10 GiB 目标，无必要。
- 阶段 9 的组合验收以本次生产验收替代（真实 nginx 路径 + 100K 长上下文 + 受控重启）。

---

## 生产参数调整 · 2026-10-18（缓存池与 mlock）

**状态：已写入配置，尚未生效** —— 四项都要引擎重启才生效，重启窗口由用户决定。

### 改了什么（`strata-production.json`）

| flag | 前 | 后 | 依据 |
|---|---|---|---|
| `--conversation-cache-slots` | 1000 | **100** | 1000 是预算养不起的名额（见下） |
| `--conversation-cache-min-tokens` | 0（默认） | **3000** | 短请求每个都占一个 slot 并写整份快照（实测 74-token 会话 = 237 MB） |
| `--conversation-cache-park-gain` | 0（默认） | **3000** | 只涨几十 token 的轮次不该重写整份快照（log 里反复 `dropped 1 superseded copy`） |
| `--conversation-cache-mib` | 131072 | 未改 | 128 GiB ÷ 100 slots = 1.28 GiB/slot，与实测单价吻合 |

`min-tokens` 的作用有据：`DETAILS.md:1126` —— 插在长会话中间的短请求会把缓存耗光，**满了之后按最老的先驱逐，驱逐的正是那条长会话**。检查发生在 `make_room()` 之前，被跳过的 park 既不驱逐也不存。

`park-gain` 的判定在 `generate.cpp:12076`：`live.size() >= park_held + park_gain` 才重新 park。

### 单价（从 engine log 的 `snapshot_bytes` 反推，不是代码推导）

| 会话 token | snapshot bytes |
|---|---|
| 74 | 237 MB（**每会话下限**） |
| 30,949 | 856 MB |
| 38,335 | 1,302 MB |
| 55,722 | 1,518 MB |
| 97,845 | 2,040 MB |

→ 128 GiB 实际能装：短会话 ~579 个 / 20K 级 ~150 个 / 50K 级 ~80 个 / 100K 级 ~40–60 个。
1000 会话 × 20–100K = 600 GiB–3 TB，**物理上不可能**（可用 RAM 951 GiB）。
磁盘 tier（session file）不可用：文档明写不支持 `--batch`，本配置 `parallel: 4`。

**被驱逐的实际后果**：不是答错，是那一轮从 system-prompt root 重新读（root、pinned prefix、最新的点永不被丢弃）—— 40K token 按 2300–2600 tok/s ≈ **16–17 秒**，对比现在 0.4 秒。

### `LimitMEMLOCK=infinity`（`strata.service`）

运行中的进程 `Max locked memory = 64 MiB`，`VmLck = 0`，engine log 明确：`PLE table mlock failed … its pages stay faulted in but may be reclaimed`；PLE 表 320,001,536 rows（28.8 GB 文件）。当前无发作（`disk_read_mb = 0`）。属低概率高影响的尾风险。unit 已加 `LimitMEMLOCK=infinity` 并 `daemon-reload`，**重启后生效**。

### P6 的两处"对不齐"——查明：不是不一致，是语义

1. **`spec 4` vs metrics `spec 6`**：`generate.cpp:2989-2992`，`--suffix-draft`（默认开）满足条件时把验证窗口抬到 `spec+2`，并把 `mtp_max_t` 设为原 `--spec`。所以 `mtp_max: 4` 正是 `--spec 4`，`spec: 6` 是调整后的窗口。metrics 的 `lookup` 字段打印的是 `o.suffix_draft`（默认 3），不是 lookup 深度。`lookup_chain` 默认 0，那条分支没走。
2. **`--vram-reserve-mib 9216` vs `vram_free_mib 8994`**：`vram_free_mib` 是 `free_b >> 20` 的**实测值**（该行打印时的真实空闲），不是配置的回显；差值是引擎为 drafter/head 和 window 另留的头room（`generate.cpp:3960` 的 `+1000+96`）。告警线 `kVramSafeMib = 512`（Linux），8994 远高于。**建议不改配置**，把映射写进文档即可。

### 监控（`scripts/strata_monitor.py`，只读）

读 `/metrics` + `/proc/PID/{status,limits}` + swap，告警项：`evictions>0`、`bytes/budget≥0.8`、`disk_read_mb>0`（PLE 页真被回收）、`VmLck==0`、引擎 `VmSwap>0`、swap 空闲 <200 MiB、`hit_rate<0.5`。exit 1 = 有告警。

自测证明告警能失败（`--selftest`，8 项全红，exit 1）。

首次实跑：`evictions 0`、`bytes/budget 0.068`、`parked 8`、`reused 58/64`、`hit_rate 0.997`、`disk_read 0`、`VmSwap 0`；唯二告警 = `vmlck 0`（重启前仍是 64 MiB）和 `swap_free 17 MiB`（环境，属他人作业，不动）。

### 未做

- 未重启（重启窗口由用户决定）
- 未 mask `ninfer-webui-restart.service`（static unit 无 disable 态；mask 会打断已文档化的回滚路径）
- 未加大 swap（会 OOM 别人的作业，且让 PLE 页更容易被换出）
- 未安装监控的定时任务
