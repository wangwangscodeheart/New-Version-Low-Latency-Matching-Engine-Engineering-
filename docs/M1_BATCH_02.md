# M1 第二批：有界事件待发批次与失败屏障

后续进展：[M1第三批](M1_BATCH_03.md)已在标准接线实现Journal整命令接受，本文保留第二批验证时的边界与待办作为历史记录。

归档日期：2026-11-12（Asia/Tokyo客户端日期）；原始工具时间标签保留，不改写日志。基于`c690dfd`及[M1第一批](M1_BATCH_01.md)继续增量修改，保留V1撮合循环与单簿单写者。

本批解决M0 R03的进程内EventSink拒收/异常处理边界，**不代表R05 Journal整命令提交、崩溃持久化或整条公开行情链路已经完成**。

## 1. 源码变更与业务动机

| 文件 | 本批修改 | 对应真实系统职责 |
|---|---|---|
| `src/process_status.hpp` | 追加OUTPUT_BLOCKED及READY/PENDING/FAILED输出状态，保留旧枚举值 | 区分命令应用与输出完成 |
| `src/orderbook.hpp` | 有界预留事件批次、未接受游标、续送、防重入、失败停止、快照屏障、移动状态转移 | 单写者内存outbox/背压；不是磁盘WAL |
| `src/event_sink.hpp` | 明确true/false/throw以及同一逻辑下游的重试约定 | 输出接收契约 |
| `src/trading_engine.hpp` | 发布ORDER前检查簿状态；拒绝重入；订阅异常停止该协调器全部品种 | 应用入口准入与不确定输出后的fail-closed |
| `src/market_snapshot.hpp` | 写任何generation文件前预检全部簿输出状态 | 防止未交付批次被不含outbox的快照绕过 |
| `tests/event_system_tests.cpp` | 更新旧溢出预期，新增八组确定性故障/容量测试 | 验证不丢后缀、不重复已确认事件、不重新撮合 |
| `tests/event_delivery_integration_tests.cpp` | 新增六组跨模块测试 | 入口、Dispatcher、跨品种、快照控制面的组合验证 |
| `CMakeLists.txt` | 注册并安装新增测试程序，CTest总数20→21 | 新能力进入标准构建/测试入口 |

另外增量更新README、验证/恢复/限制文档；`.gitattributes`令归档源码patch按原字节保存，防止checkout换行改变补丁。上一批数量溢出、参考模型和监控修复全部保留。

## 2. ADR-008：接受明确时可续送，接受不明时停止

状态：本批的OrderBook/Sink与TradingEngine异常屏障已实现；Journal部分仍待后续实施。

| 场景 | 当前命令 | 后续准入 / 恢复 |
|---|---|---|
| 全部push返回true | 状态按原规则应用，交付完整 | 清批次，回到READY |
| push返回false | 当前事件未被接收；状态可能已经APPLIED，拒单事件也可能待发 | PENDING；保留全批次及未接受游标，阻断新命令 |
| 显式retry后缀全部成功 | 只交付旧事件，不执行Command，不推进sequence | 清批次，解除屏障 |
| push或撮合抛异常 | 接受/状态变化可能不明确，不宣称回滚 | FAILED；保留诊断内容，禁止自动retry/新命令/快照 |
| 同一调用栈内重入 | 拒绝嵌套process/retry/submit | 不发新事件、不消耗命令序号 |

核心返回值仍保留原命令结果：第一次APPLIED但输出只接受前缀时，`result.applied()==true`且`event_output_complete==false`。后续被屏障阻断的命令才返回`OUTPUT_BLOCKED`，事件数0、输出完整标志false，序号不前进。业务拒单已消耗的序号不因续送回退。

恢复API：`event_output_state()`、`ready_for_commands()`、`pending_event_count()`、`retry_pending_events(sink)`。检查能否接单必须用ready_for_commands，它同时覆盖正在执行/投递的processing屏障，而不只检查READY枚举。

retry必须归属**同一逻辑输出流**：缓冲可以排空、复用或换存储，但新独立消费者并没有已交付前缀，不能仅收到后缀就声称重建完整。Sink不得先修改接收状态再返回false；抛异常的接受状态不明，不能盲目重试来制造重复。当前没有FAILED清零API，外部需要对账/重新同步/重建；单纯new一个TradingEngine不是已验证恢复流程。

Dispatcher一个事件可能只送达部分订阅者，因此其异常不做逐订阅者重试；TradingEngine记录failed并重抛，后续所有品种不再接单。正常准入拒绝（gate不可用、簿待发、重入）不发布ORDER，也不发布额外latency/outcome，避免递归和孤立结果记录。健康策略的行情回调仍可正常提交订单，原strategy测试保留。

单簿被直接Sink拒收只阻断该簿，其他簿仍可工作；同一TradingEngine遇到订阅异常则整个协调器停止。所有操作仍要求单写者/控制面静止点，不因这些bool状态变成跨线程安全。

## 3. 容量证明与性能影响

现有算法在撮合前先分配aggressive订单槽。capacity=C时被动活单P≤C−1，每次Trade至少填完一个被动订单或结束aggressive，最多P次Trade，再最多一个Rested或IOC剩余撤销，所以单命令最多C个事件；拒单/撤单各1，C=0仍需1个槽。

构造时预留`max(旧诊断预算, C, 1)`，V2独立硬上限`max(C,1)`，不会在命令内部扩容事件vector。容量大于UINT32_MAX在任何池分配前拒绝，防止EventIndex范围不一致。若未来新增订单类型导致超过证明上限，抛错并FAILED，不静默截断。

正常路径增加状态/游标检查，不增加线程、锁、磁盘I/O。异常/满Sink时每簿最多保留一条命令批次；长期慢消费者通过停止准入而不是无限积压处理。仍有map档位分配等其他内存路径，不能称整个引擎完全零分配。V1无Sink诊断捕获保留旧上限与不完整标志，它不是可靠投递API；record_events=false不生成或交付事件。

移动仅允许在调用结束的静止点进行，PENDING/FAILED及游标一起转移；活动回调中移动会terminate，源对象移动后不可再用于接单。诊断capture_state/check_invariants仍可读取；create_snapshot在非ready时拒绝。多簿写前预检保证不会先写AAPL文件、再因后面的TSLA待发才失败。

## 4. 测试与原始证据

| 验证 | 实际结果 | 原始记录 |
|---|---|---|
| 新目录MSVC Release / Ninja | 初次全量48/48编译链接；补充行情异常测试后重编，全量21/21通过，最终1.73s | [Release](validation/2026-11-12-m1-output/release-ctest.txt) |
| 新目录MSVC ASan RelWithDebInfo | 初次全量48/48；同样重编补充测试，全量21/21通过，最终6.58s | [ASan](validation/2026-11-12-m1-output/asan-ctest.txt) |
| 扩展差分 | 100种子×10,000命令=100万通过 | [差分](validation/2026-11-12-m1-output/differential-stress.txt) |

回归包括首/中/末拒收、多次false、两种process阻断、拒单事件待发、零容量Sink与OrderBook、后缀与oracle逐字段比较、PENDING移动、接受前/接受后抛异常、重试中异常、回调重入、最大跨档+GTC/IOC剩余事件不扩容、无记录模式、跨品种屏障、ORDER/Rested/Outcome/MarketData订阅异常、快照预检失败不写文件。

M0的R03探针记录了旧行为“已应用、输出前缀、后缀清空”，保留为修复前证据，不改旧记录来伪装通过。未做撮合内部bad_alloc确定性注入；该异常路径仅审查fail-closed实现，不冒称通过内存失败恢复。Linux/UBSan/TSan、真实磁盘故障和kill -9本批未执行。D9025、Benchmark C4244及ASan编译永远抛异常的故障Sink时产生的C4702告警如实保留，不声称零警告。

VS x64 Developer cmd中复现（新目录配置前统一代码页，避免M0中文头依赖漏追踪）：

```bat
chcp 65001
cmake -S . -B build/m1-output-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/m1-output-release --parallel 8
ctest --test-dir build/m1-output-release --output-on-failure
cmake -S . -B build/m1-output-asan -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DENABLE_ASAN=ON -DENABLE_UBSAN=OFF
cmake --build build/m1-output-asan --parallel 8
ctest --test-dir build/m1-output-asan --output-on-failure
build\m1-output-release\matching_engine_differential_tests.exe --seeds 100 --commands 10000 --seed-base 0xD1FF0000
```

## 5. 运行性基准检查，不作优化结论

[单进程七场景检查](../benchmarks/results/2026-11-12-m1-output-check/)使用原Benchmark/输入，每场景60,000命令、warmup+5批和一次采样；summary共7行，3个证据文件SHA-256全部匹配。预解析core 47.87ns/op，routing 50.92ns/op，dispatcher 124.84ns/op，full_v2 831.03ns/op；full采样P99=3,800ns、P99.9=57,500ns、max=448,200ns。完整数据保留，不从单进程得出稳定尾延迟或优化收益。

仍为Balanced、未绑核/固定频率、非隔离后台环境，队列排空/flush不在计时内，未测持久化完成。上一批同一旧二进制也曾从约0.95us变为约4us，不能拿现在回落的数值归功本补丁。R05尚开放，Benchmark的结果checksum不验证Journal完整提交/持久化健康。

采集命令为现有Windows脚本，显式设置`MATCHING_ENGINE_RAW_OUTPUT=1`，Executable=`build/m1-output-release/matching_engine_v2_benchmarks.exe`、OutputDirectory=`benchmarks/results/2026-11-12-m1-output-check`、Runs=1。

## 6. 源码身份、回滚与后续门禁

尚未创建Git提交或推送；源码身份是`c690dfd8b008e06690e914e272173a7f4e55d164`加两批增量，环境JSON正确记录dirty，不把Git宏值冒充干净基线。[累计源码patch](validation/2026-11-12-m1-output/source_changes.patch)包含CMake、src/tests的两批改动及新测试文件，已对原HEAD的index执行`git apply --cached --check`通过（check不修改index）；可在另外的干净c690dfd工作副本重现，不要在当前工作区重复应用。[Git blob清单](validation/2026-11-12-m1-output/source_git_blobs.txt)记录规范化文本身份，不是原始字节SHA-256。

回滚本批只反向处理上述R03文件/接口、测试target和文档状态；`orderbook.hpp`还包含第一批数量保护，不能整文件恢复到c690dfd从而删掉上一批成果。保留所有原始证据。回滚会重新暴露事件丢后缀问题，应撤回可靠输出声明。

下一批仍必须处理R05：Journal在ORDER回调入队失败后当前命令可能继续执行，现有DispatcherSink也不能把无异常的异步拒收变成false。必须建立完整命令的记录/接受契约、恢复前缀和确定性故障测试，不能只追加第二次gate检查；否则输入已记录但未应用的命令也可能在Replay中被错误执行。

本批outbox不跨崩溃保留；Engine.failed不进入订单簿快照，控制面还须检查协调器和Journal状态，不能仅凭book.ready就声称整体系统恢复。内部FAILED可能只保留诊断前缀；没有自动对账/快照重同步服务。公开Feed闭环、durable prefix、唯一快照generation和网络恢复仍未完成。

面试介绍：保持单写者撮合，把“状态已应用”和“输出已接受”拆开，用有界一命令outbox与游标处理明确拒收；对接受不明的异常选择停止而非重试，避免重复成交回报。用参考事件逐字段、重入和快照故障测试验证边界，而不是把一个成功标志包装成Exactly-Once。
