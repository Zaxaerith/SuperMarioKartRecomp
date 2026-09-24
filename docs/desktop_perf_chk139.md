# chk139：桌面帧率诊断与解压切片优化（2026-09-24）

## 结论

本机窗口化 SDL2/D3D11 的卡顿主要发生在标题和赛道加载的解压阶段。它不是本次测试中的 VSync 双重等待或音频锁竞争。默认 AOT 调度将解压区 `$84:DF00–$84:E1FF` 的主时钟切片从 8 扩到 32，并在设置了主时钟截止点时跳过解释器的重复状态扫描和无用的环表清零；关闭调度器 AOT bounce 的对照路线仍用 8。单人比赛脚本的 AOT 与解释器各自相对优化前保持所有采样和严格快照相同。

这减少了明显长帧，但标题和加载过渡仍未达到全程 60 FPS。也没有证据支持旧日志 chk138 所称的“卡顿、爆音彻底消除”或“综合进度 99.8%”。

## 可重放的实测

正确进入 1P 比赛的输入脚本：

```text
360:START,500:UP,600:B,720:B,840:B,960:B,1080:B,1200:B,1320:B,1440:B
```

无头第 1580 帧开始采到 `mode=2,state=2`。此前 `360:Y,360:START,...` 旧脚本停在角色选择，不能用它证明比赛。桌面实测另外从第 1600 帧持续按 B。使用 `tools/profile_desktop.py`，实际打开 SDL 音频设备和 D3D11 渲染器；渲染器标志 `0xa`，没有 SDL VSync 标志。

| 2000 帧单人比赛路线 | 8 切片基线 | 最终 AOT 32 切片 |
|---|---:|---:|
| 超过 20 ms 的帧 | 121 | 110 |
| 超过 33 ms 的帧 | 115 | 1 |
| f181–240 窗口帧率 | 30.61 FPS | 49.17 FPS |
| f1501–1560 窗口帧率 | 29.87 FPS | 49.42 FPS |
| f1621 以后稳定比赛窗口 | 约 60.1 FPS | 约 60.1 FPS |

同一候选又跑完 3000 帧真实比赛路线，进程 exit 0，末帧 `mode=2,state=2`；第 1621 帧后的 60 帧统计窗口均约 60.1 FPS，没有超过 20 ms 的帧。整次 3000 帧运行仍有 111 帧超过 20 ms、3 帧超过 33 ms，集中于比赛前的画面转换。不同运行的绝对长帧数会受宿主负载影响，不能将一次长跑推断为所有电脑上无卡顿。

在原 8 切片下，2500 帧图形路线开启正常音频与完全关闭音频时均出现同一批加载长帧（超过 33 ms 的帧分别 117 和 115）；`SDL_RenderPresent` 通常约 0.2–0.3 ms，APU 锁等待也远小于长帧。因此本机证据指向解压期间数十万次极短解释器切片，而非音频回调或 GPU 提交阻塞。原生成代码和桥接器中残留的无条件 `[call_frame]`、`[ba28_*]` 调试打印已清理；最终 3000 帧运行的 stderr 仅 109 字节。

## 状态一致性与尚未通过的门槛

- 同一单人比赛脚本跑 1800 帧，AOT、重复 AOT、关闭调度器 AOT bounce 三次进程均正常结束并进入比赛。候选对 8 切片基线：AOT 全部 JSON 采样与 19 个严格快照相同；解释器对照也完全相同。
- AOT 与解释器对照之间仍有既存差异：忽略 WRAM `$1F00–$1FFF` 后 19 个快照仍有 10 个不同，首差 f100 WRAM 偏移 82；`route_qualified=false`。这项门槛不能称通过，也不是独立硬件正确性认证。
- 迁移门槛的 5 项 Python 回归测试通过。共享桥接器的 Phase-1 契约测试仍为 102/109；游戏目前需要的嵌套 deadline unwind 行为与 7 项通用测试冲突，未在本次性能修改中修复。
- 统一使用 16 或 32 切片会改变解释器对照的 3 个比赛栈窗严格快照，所以最终候选只给 AOT 使用 32。48 切片已在 f200 改变 AOT 的非栈 WRAM，64 切片更早改变状态，均未采用。

## 产物与复验命令

最终 `build/smk_play.exe` SHA256：`5fa7d9834ada6dc079dee97957ca287727d03b35c2a8bcf141d4a1b7c4bc35b8`，已经同步到工程根目录 `smk_play.exe`，哈希一致；原 chk138 两份 EXE 保存在 `artifacts/chk139-release-backup/`。ROM SHA256：`2ada8919688087be60a6a48cace8f877add60c45d2e5d09e2442faa55be62a49`。最终性能报告：`artifacts/chk139-real-race-desktop-lazy-ring-3000/report.json`；2000 帧 8/32 对照分别是 `artifacts/chk139-real-race-desktop-8/` 与 `artifacts/chk139-real-race-desktop-lazy-ring/`；比赛门槛对照分别是 `artifacts/chk139-race-gate-8-resume/` 与 `artifacts/chk139-race-gate-lazy-ring/`。

```powershell
python -B tools/test_migration_gate.py
python -B tools/validate_migration.py --frames 1800 --skip-old-reference --script '360:START,500:UP,600:B,720:B,840:B,960:B,1080:B,1200:B,1320:B,1440:B' --output artifacts/new-race-gate
python -B tools/profile_desktop.py --frames 3000 --modes normal --script '360:START,500:UP,600:B,720:B,840:B,960:B,1080:B,1200:B,1320:B,1440:B,1600-3000:B' --output artifacts/new-desktop-profile
```

`--output` 必须是新目录。原 ROM 仅以文件加载，未改写。独立真机准确性、所有比赛路线、手柄/键盘长期实操与主观音频无爆音还需要后续验证。
