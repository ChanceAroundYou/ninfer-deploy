# `src-tree/` — 引擎源码镜像

设备 B（`ssh ai`）上 `/home/ET/chenran/strata-lab-20261017/` 的**只读镜像快照**。权威源不在这里：那个目录是 `Niko1221/Strata` 的 git worktree。

## 快照信息

| 项 | 值 |
|---|---|
| 快照日期 | 2026-10-19 |
| 上游 | `https://github.com/Niko1221/Strata`，worktree 分支 `main` @ `477ac13` |
| 内容 | `src/` 7.3 MB（434 个源文件）· `include/` 1.4 MB · `tests/` 692 KB（57 个文件）· `docs/` 的 markdown 948 KB（45 个） |
| 不含 | `build*/`、`data/`（含 `tests/data/` fixture）、`bench/`（结果）、PDF/media、模型、venv、日志、`tools/`、`cmake/`、`.git/` |

## 刷新

```bash
R=ai:/home/ET/chenran/strata-lab-20261017
D=src-tree/strata-src
rsync -a -e ssh $R/src/    $D/src/
rsync -a -e ssh $R/include/ $D/include/
rsync -a -e ssh $R/tests/   $D/tests/
rsync -a -e ssh --include='*/' --include='*.md' --exclude='*' $R/docs/ $D/docs/
```

刷新后 `git status` 应当只看到镜像内的改动；不要把上面排除清单里的东西带进 commit。`tests/data/` 的 fixture 保留本地副本但不入库（`.gitignore` 已含）。

## 已知偏差

- 设备 B 上 `tools/strata_bench.py` 相对上游有 2 增 1 删的本地改动（修 `main()` 里未定义的 `port`），不在本镜像内（`tools/` 未纳入）。主文档原本写「结束后已恢复原文件并核对 SHA-256」——2026-10-19 核对时**该改动仍在文件里**，该记录有误。
- 文档里的源码行号引用是 2026-10-18 的快照，与本镜像（`477ac13`）可能有漂移；引用前先在镜像里核对。
