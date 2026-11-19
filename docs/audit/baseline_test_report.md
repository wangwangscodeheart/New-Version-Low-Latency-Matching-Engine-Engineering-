# M0：构建、测试与缺陷复现基线

日期：2026-11-11（Asia/Tokyo）。源码提交：`c690dfd8b008e06690e914e272173a7f4e55d164`。独立 GitHub 克隆、起始工作树干净；构建前未修改任何生产代码。本轮新增 probe 不加入原 CTest，以免“缺陷仍存在”被误解为正确性通过。

## 1. 实际执行

在仓库根目录、VS 2022 x64 Developer 环境中运行（本机用 `D:\新言的vs2022\Common7\Tools\VsDevCmd.bat -no_logo -arch=x64 -host_arch=x64` 初始化 cmd）：

```powershell
cmake -S . -B build/m0-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/m0-release --parallel 8
ctest --test-dir build/m0-release --output-on-failure

cmake -S . -B build/m0-asan -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DENABLE_ASAN=ON -DENABLE_UBSAN=OFF
cmake --build build/m0-asan --parallel 8
ctest --test-dir build/m0-asan --output-on-failure
```

两个新构建目录均从空目录配置，46/46 编译/链接步骤成功；编译器 MSVC 19.44.35228，C++17。

| 验证 | 实际结果 | 原始测试输出 |
|---|---|---|
| MSVC Release / Ninja | 20/20 通过；CTest 总时间 5.27 s | [release-ctest.txt](evidence/release-ctest.txt) |
| MSVC ASan RelWithDebInfo / Ninja | 20/20 通过；CTest 总时间 9.40 s | [asan-ctest.txt](evidence/asan-ctest.txt) |
| 基线边界 probe（Release） | R01–R05 均复现；退出 0 表示观察符合基线，不代表缺陷已修复 | [core-edge-probe.txt](evidence/core-edge-probe.txt) |
| 历史 Benchmark 证据 SHA-256 | 2026-08-07 目录七个文件 7/7 匹配 | 清单见该目录 `checksums.sha256` |
| 本轮 Benchmark 证据 | 五进程、35 行结果、七个文件 7/7 SHA-256 匹配 | [性能基线](baseline_benchmark.md) |
| Linux / GCC / Clang / UBSan / TSan | 本轮未执行，不沿用 Windows ASan 结果 | 无 runner URL / 日志 |
| 千万命令 stress / soak / kill -9 / 磁盘满 | 本轮未执行 | 不声称通过 |

测试原始记录取自 CTest 自动生成的 `build/m0-*/Testing/Temporary/LastTest.log`，以 UTF-8 文本保存。其时间标签由 Windows/CTest 本地格式生成，个别中文时区字符显示异常；报告日期口径为 Asia/Tokyo，不据此推导跨机器时钟精度。

配置中的 pthread 探测失败后 `Found Threads: TRUE` 是 Windows 探测回退，不是构建失败。构建有 D9025（`/W3` 被 `/W4` 覆盖）与 `benchmarks/v2_perf.cpp:170` 等 uint64→double C4244 告警；本轮未声称零警告。中文 `/showIncludes` 前缀识别异常产生大量输出，后续构建依赖追踪需核验，不在 M0 修改工具链。

## 2. 20 个 CTest 的覆盖边界

18 个测试可执行文件：unit、properties、differential、snapshot、market_snapshot、market_manager、event_system、journal_replay、async_logger、monitor、market_data、strategy、async_journal、market_data_generator、feed_gateway、capacity_metrics、risk_engine、order_types。另两项是 V1/V2 demo 集成。

默认差分 25 种子×4,000 命令：逐命令比较状态码、完整事件和优化簿不变量；每 25 步及最终比较完整簿。随机域 quantity 最大合法值为100，capacity256，未覆盖默认配置允许的 uint64 极值或满池 Post-Only 组合。相同种子使用 STL distribution，不保证跨工具链字节流相同。

已有 Sink 溢出测试验证“已成交、输出不完整可检测”，并没有验证“输出能够补发”。Snapshot 测试验证格式/损坏拒绝及正常恢复，不代表强杀/断电一致性验证。

## 3. 新增审计探针

源码：[core_edge_probe.cpp](evidence/core_edge_probe.cpp)。仅调用公开接口；不改生产源、不注入未定义行为、不依赖异步调度竞争。

```powershell
cl /nologo /EHsc /std:c++17 /O2 /MD /utf-8 /W4 /I src docs/audit/evidence/core_edge_probe.cpp /Fobuild/m0-release/core_edge_probe.obj /Febuild/m0-release/core_edge_probe.exe
.\build\m0-release\core_edge_probe.exe
```

| 编号 | 最小输入 / 条件 | 当前实际观察 |
|---|---|---|
| R01 | capacity2，同侧同价先挂 UINT64_MAX，再挂1 | 两单 APPLIED；档位 total_volume 回绕0；check_invariants=false |
| R02 | capacity1 先挂卖单，再提交交叉买 Post-Only | 核心 POST_ONLY_WOULD_TRADE；参考模型 POOL_EXHAUSTED |
| R03 | 两档各1，买单吃2；FixedEventBuffer 容量1 | 已应用；产生2事件、仅交付1、保留0；权威簿空 |
| R04 | Monitor 预注册 AAPL；发布无 InstrumentId 的质量告警 | 全局 warnings=1，AAPL warnings=0（快照覆盖） |
| R05 | Gate 入口健康，在 ORDER 回调置为不健康 | 当前单仍 APPLIED；下一条才 SYSTEM_UNAVAILABLE；簿仍有订单 |

R05 用确定性 gate 模拟 Journal 健康状态变化，证明检查时点；不伪称做了真实磁盘满或 AsyncJournal 队列竞争故障注入。R03 同时已有正式测试覆盖。第一次探针设置曾把 raw price=101 用于默认 tick100，触发合法的 off-tick 拒单；改为200后重新编译并获得本报告五项结果，该设置修正不是生产修复。

## 4. M0 范围与回滚

本轮新增六份规定审计报告、`decisions.md`、上述探针与测试记录，以及 `benchmarks/results/2026-11-11-m0-baseline/` 八个证据文件。未修改原 README、CMake、`src/` 或 `tests/`；没有提交/推送远端。

业务动机是先冻结可解释基线，避免把常规测试全绿等同完整可靠性。生产性能影响为零（审计文件不进构建）；额外磁盘占用是诊断证据。回滚只需将本轮新增审计与结果目录移出仓库，无需改动 V1/V2 核心或重置原分支。

M0 审计交付已形成，但不等于 V2.1 升级完成。进入 M1 前先确认 [ADR](decisions.md) 的提交/输出策略，再将 R01/R02 等改成修复后不变量回归。R03/R05 在契约实施后应由“复现观察”转为故障恢复验收，不保留为正确性通过条件。
