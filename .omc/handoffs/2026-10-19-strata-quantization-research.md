# ninfer-deploy — 会话交接（Handoff）

日期：2026-10-19
交接自：Strata Qwen3.8-Flash-Next 量化选择研究
接续方式：见文末 "Resume Prompt"

## 1. 项目概况
当前工作目录：`/home/nnb/projects/llm-deploy`（原 `/home/nnb/projects/ninfer-deploy`，目录已更名）。本次是针对 Strata 官方模型选择资料与社区量化报告的研究问答，没有检查或修改项目代码。

## 2. 已完成的工作
- 使用 AnySearch 搜索 Strata 官方 `MODELS.md`、`COMMUNITY_BENCHMARKS.md`、社区讨论及第三方量化对比。
- 汇总 Q2_0、IQ2_XS、IQ3_XXS、IQ3_S、Coder、Swift 1.5、UD-IQ4_XS、UD-Q4_K_XL 的官方定位、硬件适配和注意事项。
- 比较社区报告：MindStudio 双 RTX 3060 上单条 SVG 创作提示测试；另有社区任务平均分和 IQ3 家族对比的非官方报告。明确指出小样本结论不能泛化，并引用官方提示“速度测试不是质量测试”。
- 提供了以当前 IQ2_XS 为生产基线、按真实任务进行 A/B 验证的建议。

## 3. 关键事实（防遗忘）
- 官方按 RAM 的建议：32GB 可考虑 Coder（但非代码能力和中文等语言较弱；有 24GB GPU 时 Q2_0/IQ2_XS 也可运行且更通用）；48GB 推荐 IQ2_XS，追求速度可选 Q2_0；64GB 可尝试 IQ3_XXS/IQ3_S，IQ3_S 需要少开其他应用；96GB+ 可评估 IQ3_S 或 Unsloth 4-bit。
- 官方列出的专家 RAM 驻留量约为 Q2_0 34GB、IQ2_XS 35.5GB、IQ3_XXS 43GB、IQ3_S 50GB、Coder 23GB；Unsloth UD-IQ4_XS / UD-Q4_K_XL 全部专家驻留分别约需 72GB / 88GB RAM。SSD 取专家会显著变慢。
- Coder 是保留 512 位专家中的 256 个、按代码相关数据筛选的特化模型；官方引用作者数据称其达到完整模型 SWE-bench Verified 的 91%、LiveCodeBench 的 99%，此为作者报告，不应当作独立验证。
- Swift 1.5 是微调版本，官方称其思考更短、质量大致相同；不是量化等级。
- MindStudio 测试仅一个动画 SVG 提示及特定双 RTX 3060 环境，主观上 IQ3_S 输出更完整；不足以建立普适质量排名。社区也有 IQ2_XS/Q2_0 任务平均成绩近似以及特定设置下 IQ3_XXS 胜过 IQ3_S 的报告，均应视为依任务/测法而异。
- AnySearch 已完成检索。WebFetch 有些 GitHub/Medium 页面 403，主要依据搜索摘录及成功读取的官方页面/报告，未来如需核实最新说法，应重新抓取官方原始文档或查对应提交。

## 4. 当前文件状态
| 文件 | 状态 |
|---|---|
| `.omc/handoffs/2026-10-19-strata-quantization-research.md` | 本交接记录 |
| 项目代码与配置 | 本次未查看、未修改 |

## 5. 待办（Next Steps）
1. 若需要继续研究，重新打开 Strata 官方 `docs/MODELS.md`、`docs/COMMUNITY_BENCHMARKS.md` 及相关社区基准原始数据，核对本次摘要的数值、版本和测试条件。
2. 若目标是生产选型，整理一组代表性内部任务，对当前 IQ2_XS 与候选量化档固定提示、上下文、采样参数和硬件，记录质量、速度及 RAM/VRAM 峰值后再决策。

## 6. 常用命令

```bash
python3 /home/nnb/.claude/skills/anysearch/scripts/anysearch_cli.py search "Strata Qwen3.8-Flash-Next quantization quality" --max_results 5
```

Transcript：`/home/nnb/.claude/projects/-home-nnb-projects-ninfer-deploy/6f6b1535-b5cd-481a-bc26-dfed597fa22d.jsonl`

## 7. Resume Prompt

工作目录 `/home/nnb/projects/ninfer-deploy`；背景：研究 Strata 的 Qwen3.8-Flash-Next 量化变体及社区质量证据。请先阅读交接文档 `.omc/handoffs/2026-10-19-strata-quantization-research.md`，关键资料为 Strata 官方 `docs/MODELS.md`、`docs/COMMUNITY_BENCHMARKS.md`、MindStudio SVG 对比报告及相关社区原始基准。第一项待办：重新核对官方文档及原始结果的数值、版本、硬件和测试条件，标出哪些结论来自官方、哪些是第三方或单任务观察。请先阅读交接文档与关键文件，确认环境可用（编译/测试通过），再开始待办第 1 项；开始前与我确认。
