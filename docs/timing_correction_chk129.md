# chk129：真实状态与周期补偿移除（2026-09-23）

## 交付状态

当前能够将生成 C 与共享运行时编译为加载源 ROM 的无头诊断 EXE，尚不能作为可玩移植版交付。
已观察 RESET、标题、菜单、角色选择；未验证比赛流程、完整赛道、音频正确性和存档。
没有桌面窗口、实时键盘/手柄输入与音频输出集成。不存在可据以宣布 99% 的游戏覆盖率。

gen_v5 清单有 72 个根、948 个不同 PC、975 个精确 M/X 节点，其中 849 个
aot_eligible、126 个 lle_only。11 个生成 C 文件合计 11,802,664 字节。
这些是分析图统计，包含地址别名与模式变体，不是游戏逻辑完成百分比。

## 修改

interp_bridge.c 移除 chk125 的每指令固定 80 主时钟补偿，恢复固定上游提交
15d7783c06fb509d9e8f157437dc13459028071a 的总线耗时加内部周期乘 6 算法。
未改生成代码、ROM 或旧工程源码。旧 EXE 保留在 artifacts/timing-chk129/smk_headless-chk128.exe。
当前 build/smk_headless.exe 已重新构建，SHA256 为
8d373d81ca1e0ab9c25ac91829ae6ed5698456efc703d752a69943d54f55ed48。

新增 tools/test_bridge_timing.c 与 .ps1，复用固定上游的 ROM-free bridge 测试替身。
测试分别验证慢速/快速 ROM 的 NOP 和 LDA immediate；需要本地上游 tests/interp816/bridge_test.c，
可用脚本 -Upstream 参数指定位置。

## 验证

- CPU 契约：41/41；bridge 契约：109/109；迁移门槛：5/5。
- 新增时钟测试：12/12。仅在 artifacts 内构建旧公式负对照，8/12，四项耗时断言全部失败。
  慢速 NOP 实测旧公式 88，修复后 14；快速 NOP 86→12；慢速 LDA immediate 96→16；快速 92→12。
- 1400 帧默认脚本 AOT、重复 AOT、调度器 bounce-off 均正常退出，耗时分别 66.530、66.340、74.730 秒。
- AOT 重复采样一致；AOT 与 bounce-off 画面哈希采样一致。
- 15 个内存采样点，带栈窗掩码差分 10 个，严格差分 13 个；最早 f200 WRAM $000E，AOT=10，另一侧=8。
- 三次均停于 mode=0/state=6，没有比赛采样。tier_equivalent、route_qualified、hardware_accuracy_qualified 均 false。
- 本轮未重跑 old_reference；旧新跨运行时差异仍未解决。bounce-off 也不是完全独立的纯 LLE oracle。

复验命令：`python -B tools/validate_migration.py --frames 1400 --output artifacts/route-chk129-timing --skip-old-reference`。
完整报告为 artifacts/route-chk129-timing/report.json。ROM 身份校验通过。

## 后续工作顺序

1. 在 f200 前定位 $000E 首次写入分歧，核查 AOT 块计费、细切片恢复点和事件调度；不要重新加入平均周期补偿。
2. 根据实际菜单状态核对输入脚本及单人模式选择，确认角色选择停滞来自输入还是执行错误；不得强写确认状态。
3. 完成真实进入比赛与持续驾驶的回归，增加独立可信基准校验。
4. 再集成 SDL 窗口、实时输入和音频，验证可玩性；最后优化切片成本与打包。

恢复上游公式及单指令测试通过，不等于整个硬件时序已经正确；当前主机记录的最大 deadline 超时仍达 538818 主时钟。
