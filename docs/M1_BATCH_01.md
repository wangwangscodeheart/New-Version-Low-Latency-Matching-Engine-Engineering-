# M1 第一批：边界正确性与监控修复

日期：2026-11-11（Asia/Tokyo）。仓库：`D:\Low-Latency-Matching-Engine-Engineering`。

本批仅处理 M0 的 R01、R02、R04，保留 V1 撮合循环、FIFO、池、索引、Ladder 与单写者。R03/R05 的事件/Journaling 提交契约尚未实施；本文不是完整 M1 或 V2.1 完成声明。

## 1. 修改文件与动机

| 文件 | 增量修改 | 业务原因 / 对应组件 |
|---|---|---|
| `src/orderbook.hpp` | 挂簿前检查同侧同价剩余容量 | 交易所入口的簿资源/整数边界保护，避免合法单笔组合损坏档位统计 |
| `src/events.hpp` | 追加 `LEVEL_VOLUME_OVERFLOW=11` 及字符串，不重排旧值 | 给调用者明确且稳定的拒单原因 |
| `src/reference_engine.hpp` | 从订单独立推导剩余预算；Post-Only业务拒绝优先于池满 | 参考模型与公开规则一致，不共用优化簿的累计字段掩盖错误 |
| `src/replay.hpp` | 诊断CSV解析支持新拒因及此前遗漏的Post-Only拒因 | 保留新增业务结果的可读日志往返；不是把诊断CSV改成恢复协议 |
| `src/monitor.hpp` | attach时建立symbol→slot；无ID但已注册事件进入同一槽 | 统一交易事件和数据质量事件的按品种可观测性 |
| `tests/order_type_tests.cpp` | 九组黄金边界回归、逐命令参考对照、拒因日志往返 | 修复后用不变量、事件、全盘口和拒因共同验收 |
| `tests/monitor_tests.cpp` | 两组混合ID/无ID与fallback回归 | 验证告警不消失、未知品种保留、读取快照不重复计数 |

没有改 `CMakeLists.txt`、Benchmark程序或采集脚本，没有新增交易功能。

## 2. ADR-007：档位数量准入与拒单顺序

状态：本批已实现并验证。

数量范围约束按**单个价格档位**定义，不是整个买卖盘总数量：同一档 `total_volume` 必须可由uint64表示，不同档位合计超过UINT64_MAX仍然合法。判断使用 `qty > UINT64_MAX - existing_volume`，不先做可能回绕的加法。

核心检查位于已有字段校验和Post-Only规则之后、订单池分配/索引提交/撮合之前。拒绝只产生一个拒单事件并推进命令序号，不改变原订单、FIFO、档位、池或索引；不是“整个状态连序号都完全不变”。新规则优先级：

```text
既有ID/价格/数量规则
 -> POST_ONLY_WOULD_TRADE
 -> LEVEL_VOLUME_OVERFLOW
 -> POOL_EXHAUSTED / INDEX_EXHAUSTED
 -> 撮合 / 剩余挂单
```

为何不预扫描撮合或先成交后回滚：合法非交叉簿满足最高买价<最低卖价。如果同侧已有相同限价档，则新单不可能跨对侧；若新单可成交，同侧相同限价档必不存在，剩余量进入新档且不超过单笔uint64。故只对**非IOC且非交叉**输入检查已有档位即可；IOC余量直接撤销，不占挂簿量预算。

查询保留 side cache→Ladder→map 路径；回退依据 `contains_price`，包含“价格在范围内但不对齐Ladder tick”的合法细tick情况。共享Ladder的同价对侧档位不会被误当同侧，因为非交叉前提排除了该情形。ReferenceEngine以deque中各订单做独立减法预算，不读取优化累计量。

失败默认动作是整条业务拒绝，不改变撮合算法，不加锁，不做饱和统计或隐式截断。旧枚举值保持稳定，但新版本才理解新增拒因；需要读取新诊断日志时应使用本批解析器。

## 3. 监控设计与边界

有效InstrumentId仍直接索引预分配槽，不增加字符串哈希或额外锁。只有无有效ID的事件查attach时建立的symbol映射；已注册品种归并同槽，真正未知品种仍走fallback。初始化新增O(symbol数)内存和工作量。

测试覆盖同品种有ID订单+无ID订单、warning/error、未知MSFT、未传MarketManager的Monitor和重复snapshot。注册表固定、单次attach是本批契约；运行期动态注册、重复attach后的历史计数迁移没有新增承诺。

## 4. 红灯、绿灯与全量验证

核心回归先在尚未修复的实现上重新编译，失败于 `optimized.check_invariants()`，原始输出见 [core-red.txt](validation/2026-11-11-m1-batch-01/core-red.txt)。修复后同测试重新编译通过：[core-green.txt](validation/2026-11-11-m1-batch-01/core-green.txt)。监控新测试在旧实现失败于混合订单计数断言；修复后直接cl重新编译通过，最终全量CTest再次覆盖。

| 验证 | 本批实际结果 | 原始记录 |
|---|---|---|
| 全新MSVC Release / Ninja构建 | 46/46编译链接，20/20 CTest；总时间6.54s | [Release](validation/2026-11-11-m1-batch-01/release-ctest.txt) |
| 全新MSVC ASan RelWithDebInfo / Ninja构建 | 46/46编译链接，20/20 CTest；总时间9.47s | [ASan](validation/2026-11-11-m1-batch-01/asan-ctest.txt) |
| 扩展差分 | 100种子×10,000命令=100万命令通过，seed_base=0xD1FF0000 | [差分输出](validation/2026-11-11-m1-batch-01/differential-stress.txt) |

九组核心回归包括买卖两侧、cache/dense/map与区间内off-grid、exact UINT64_MAX、拒绝后槽位与ID可复用、取消/部分成交释放预算、IOC不占预算、同价对侧巨量成交、跨档成交后剩余挂簿、满池Post-Only/overflow拒因优先级，以及诊断日志往返。每条边界命令比较参考状态码、完整事件、完整簿及优化簿不变量。

本批没有执行Linux/UBSan/TSan或kill -9/磁盘故障测试。原有编译告警D9025与Benchmark C4244仍存在，不能称零警告。

## 5. 构建复现与本地依赖追踪问题

在VS 2022 x64 Developer **cmd**中，配置前执行 `chcp 65001`。本机只有中文编译器资源，单设VSLANG=1033不会切换英文。旧M0构建目录曾在修改monitor头文件后报告 `ninja: no work to do` 并执行旧二进制；这把M0 C07候选提升为实际工具链问题。

本批使用全新目录、统一UTF-8 console code page后，生成规则的 `msvc_deps_prefix` 为正确的 `注意: 包含文件:  `，`ninja -t deps` 已列出orderbook/reference/events/replay/monitor头文件，不再靠旧目录的增量构建证明修复。

```bat
chcp 65001
cmake -S . -B build/m1-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/m1-release --parallel 8
ctest --test-dir build/m1-release --output-on-failure
cmake -S . -B build/m1-asan -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DENABLE_ASAN=ON -DENABLE_UBSAN=OFF
cmake --build build/m1-asan --parallel 8
ctest --test-dir build/m1-asan --output-on-failure
build\m1-release\matching_engine_differential_tests.exe --seeds 100 --commands 10000 --seed-base 0xD1FF0000
```

## 6. 源码身份与证据口径

验证基于`c690dfd8b008e06690e914e272173a7f4e55d164`加本批七文件工作区diff，尚未创建提交。因此Benchmark编译元数据中的Git commit仍为基线，不能将本批数据误称为干净c690dfd实测。环境JSON已记录 `tracked_worktree_clean=false`。

[source_git_blobs.txt](validation/2026-11-11-m1-batch-01/source_git_blobs.txt)记录七个修改文件及未变的Benchmark/CMake/采集器Git blob ID，使用 `git -c core.autocrlf=true hash-object --path=<path> <path>` 计算。它是文本规范化后的Git对象身份，不是原始字节SHA-256；避免源码checkout换行导致错误校验。性能证据仍采用原始字节SHA-256和现有`-text`属性。

进入第二批前另冻结了[第一批完整源码patch](validation/2026-11-11-m1-batch-01/source_changes.patch)，对原HEAD的index执行`git apply --cached --check`通过（没有修改index），可在独立干净c690dfd副本重现第一批；不要在当前已改工作区重复应用。

## 7. 性能与交付边界

核心新增非交叉挂单的档位查询与一次减法比较，没有预扫描对侧、没有新动态分配；Reference的deque遍历不进入生产核心。Monitor有效ID路径不新增哈希；无ID已注册事件有一次查表。不能把微基准当持久化完成延迟。

使用未修改的采集器与相同BUY1/Cancel三品种循环，5个新进程复测本批Release；每进程仍为5批中位数，未更改输入、种子或场景。原始证据见[本批五轮结果](../benchmarks/results/2026-11-11-m1-batch-01/)（35条汇总，7/7 SHA-256匹配）。命令：

```powershell
$env:MATCHING_ENGINE_RAW_OUTPUT = '1'
powershell -NoProfile -ExecutionPolicy Bypass -File ./scripts/run_benchmark_windows.ps1 -Executable 'build/m1-release/matching_engine_v2_benchmarks.exe' -OutputDirectory 'benchmarks/results/2026-11-11-m1-batch-01' -Runs 5
```

首次比较发现full_v2比稍早M0基线明显变慢，故没有挑掉异常轮次或宣布性能无退化，而是随后以**原M0二进制**再跑2个独立进程。控制组证据见[旧二进制当前环境对照](../benchmarks/results/2026-11-11-m1-baseline-control/)（14条汇总，4/4 SHA-256匹配）：同命令把Executable改为`build/m0-release/matching_engine_v2_benchmarks.exe`、OutputDirectory改为该控制组目录、Runs改为2。

| 场景 | 稍早M0五进程中位 ns/op | 本批五进程中位 ns/op | 随后原M0二进制两轮 ns/op | 本批采样P99 / P99.9 ns |
|---|---:|---:|---:|---:|
| core_pre_resolved_instrument | 48.78 | 68.61 | 47.43 / 56.21 | 200 / 300 |
| core_plus_routing | 51.53 | 49.69 | 51.62 / 51.18 | 200 / 200 |
| dispatcher | 124.74 | 123.55 | 121.75 / 122.32 | 200 / 300 |
| dispatcher_monitor | 166.24 | 168.57 | 167.21 / 167.06 | 300 / 400 |
| dispatcher_async_logger | 581.80 | 564.87 | 555.20 / 552.41 | 2,600 / 52,400 |
| dispatcher_async_journal | 388.19 | 385.31 | 366.50 / 384.21 | 4,600 / 52,900 |
| full_v2 | 950.50 | 3,866.82 | 4,137.61 / 3,978.59 | 7,400 / 233,100 |

表中P99/P99.9是各进程分位数再取中位，并非合并样本分位数。全部P50/max/吞吐在各目录summary.csv；本批full采样最坏3,247,600ns（约3.25ms），尾延迟明显，不能省略。预解析core本批批次范围46.53–85.12ns，亦有较大波动。

原二进制当前也出现约4倍full变慢，因此历史差异**不能直接归因此次补丁**；这不等于查明根因，也不证明补丁零成本。仍为Balanced、未绑核/固定频率、普通后台环境，没有ETW线程/锁等待证据；两组并未交错运行，不能做严格因果A/B。编译和全量测试均在性能采集前结束，采集中只有普通文档/诊断活动。

控制二进制是之前M0干净源码编译保留的程序，执行时工作树已修改，故控制组environment.json也显示dirty，这不是把它当新源码重编。两者原始文件SHA-256用于区分本次执行对象（不承诺不同机器编译获得同字节）：

```text
e2952bbd96ee93eae267b5632867030f516dc2eb1cfa33098b839db8a8123ca4  build/m0-release/matching_engine_v2_benchmarks.exe
f84f3aacb03c1f1e7256a7e3dda4c08cf59cf01d299b73a86e78b8096dc0d587  build/m1-release/matching_engine_v2_benchmarks.exe
```

结论：本批正确性门禁通过；性能数据已如实留证，尚无受控无回归/收益结论。仍缺异步接受/写入健康校验、P95、排队、CPU/RSS/perf计数和持久化延迟。后续按M0 ADR/基准规范补受控测量，不为压低这组数字删除检查、隐藏异常或贸然添加无锁/绑核等生产设计。

## 8. 回滚与下一批

本批源码改动仅上述七文件。回滚应只反向应用本批diff（或单独提交后revert该提交），不要reset整个仓库，不要丢弃M0与历史证据；回滚会重新暴露R01/R02/R04，应同时撤回修复声明。保留新失败回归能够说明旧行为不可接受。

下一批是M0 ADR-001/002：完整命令的事件输出、可靠订阅者接受与Journal故障契约。R03/R05仍然开放；durable prefix、公开行情闭环、快照增量拼接仍分别属于后续阶段。非法枚举和Risk极值等风险也没有因本批全绿而关闭。

面试可介绍：我没有换撮合算法，而是用非交叉簿不变量证明准入检查可以在撮合前完成，独立参考模型和极值黄金案例约束拒单语义；监控统一ID与symbol两种输入路径；同时验证构建依赖，避免用旧二进制制造假通过。对应真实系统中的入口保护、确定性正确性验证和可观测性数据一致性。
