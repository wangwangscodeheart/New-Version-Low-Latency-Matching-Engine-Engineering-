# M0：源码清单与基线身份

审计日期：2026-11-11（Asia/Tokyo）。本文描述实际源码，不把 V2.1 任务书目标当成已有能力。

## 1. 仓库与环境

| 项目 | 本次核验结果 |
|---|---|
| 工作目录 | `D:\Low-Latency-Matching-Engine-Engineering`，独立于项目三 |
| origin | `git@github.com:wangwangscodeheart/New-Version-Low-Latency-Matching-Engine-Engineering-.git` |
| 分支 / 基线提交 | `main` / `c690dfd8b008e06690e914e272173a7f4e55d164` |
| 起始工作树 | 干净；109 个 tracked 文件；本轮仅新增审计材料、诊断探针和基准证据 |
| 系统 | Windows 11 家庭中文版，版本 `10.0.26300`（系统 API 的 Windows 10 字符串不是另一台机器） |
| CPU | Intel Core i7-14650HX，16 核 / 24 逻辑处理器 |
| 编译器 | MSVC `19.44.35228.0`，x64，工具目录 `14.44.35207` |
| 构建工具 | CMake `4.4.2`（`C:\Program Files\CMake\bin\cmake.exe`），Ninja `1.13.2` |
| 标准 / 依赖 | C++17 required、extensions OFF；STL、Threads、平台文件刷新 API，无第三方运行库框架 |
| Release flags | `/DWIN32 /D_WINDOWS /W3 /GR /EHsc /O2 /DNDEBUG /MD -std:c++17 /W4 /utf-8` |
| Sanitizer | 单独的 MSVC ASan RelWithDebInfo 构建；本次没有 Linux/UBSan/TSan 执行证据 |

构建、测试命令及日志见 [测试基线](baseline_test_report.md)；性能环境与边界见 [Benchmark 基线](baseline_benchmark.md)。

## 2. 目录

```text
CMakeLists.txt / .github/workflows/ci.yml   构建与 CI 定义
src/                                      header-heavy 撮合核心、V2 外围、三个 main
tests/                                    18 个测试程序；另有两个 demo 集成测试
benchmarks/                               V1/V2 benchmark 与历史原始证据
scripts/                                  Windows 采集、Linux perf 辅助脚本
data/sample_synthetic_market_data.csv      合成报价样例，不是真实交易所数据
docs/                                     当前专题说明及 V1 历史编号文档
docs/audit/                               本轮 M0 事实、风险、决策与原始测试记录
```

## 3. 模块、所有权、线程与覆盖

以下路径均相对仓库根目录；行号以冻结基线为准。

| 模块 / 主要类 | 文件与事实定位 | 所有权与执行线程 | 现有测试 |
|---|---|---|---|
| Command / EngineEvent / 强类型 | `src/commands.hpp`、`events.hpp`、`types.hpp` | 按值；New/Cancel 输入，Trade/Rested/Cancelled/Rejected 输出 | unit、order_type、event_system |
| Matching / OrderBook | `src/orderbook.hpp:132` process；`:237` 校验；`:553`/`:604` 撮合 | 每簿拥有全部状态；调用者保证 Single Writer；无内部撮合线程 | unit、property、differential |
| OrderPool / Order / LimitLevel | `src/order.hpp:70`、`:143` | 簿拥有预分配 vector/free-list；链表借用池中 Order 指针 | unit、property、capacity_metrics |
| LimitLevelPool | `src/limit_level_pool.hpp:13`、`:54` | 簿拥有；档位 in-use 检查；目录与订单 parent_level 借用指针 | unit 的生命周期测试 |
| FixedOrderIndex | `src/fixed_order_index.hpp:47`、`:67`、`:102` | 固定开放寻址表借用 Order 指针；prepare/commit 之间不能修改表 | unit 的冲突/删除/复用 |
| PriceLadder / PriceLevelStore | `src/price_ladder.hpp:23`；`price_level_store.hpp:37` | 稠密指针表加有序 map；共享档位池；map 节点仍动态分配 | unit 的范围外回退、FIFO |
| EventSink | `src/event_sink.hpp`；`orderbook.hpp:168` | 每命令先撮合后逐条 push；临时 vector 由簿拥有；失败无后缀保留 | event_system 的 FixedEventBuffer 溢出 |
| ReferenceEngine | `src/reference_engine.hpp:17` | 独立 map/deque/unordered_map 参考模型，不调用优化簿 | differential 默认 25×4,000 |
| MarketManager | `src/market_manager.hpp:38` | unique_ptr 持有每个 OrderBook；symbol→InstrumentId；同线程路由 | market_manager、capacity_metrics |
| TradingEngine | `src/trading_engine.hpp:44` | 借用 manager/dispatcher/gate；调用线程发布输入、执行、发布结果 | event_system、journal_replay |
| SystemEvent / EventDispatcher | `src/system_events.hpp`；`event_dispatcher.hpp:107` | 同步回调、RAII 订阅、可 freeze；非跨线程总线 | event_system 的顺序/寿命/freeze |
| Journal / JournalProjector | `src/journal.hpp:65`、`:100` | 输入/Trade/结果投影；内存 vector 无历史上限，save 同步写盘 | journal_replay |
| AsyncJournalWriter | `src/async_journal.hpp:20`、`:35`、`:50` | 交易线程投影入队；独立 worker 写文件；停止时 drain/flush | async_journal 的溢出/gate/尾部 |
| ReplayEngine | `src/replay_v2.hpp:79` | 重提输入到同一正常核心；历史 Trade 只比较，不二次应用 | journal_replay、market_snapshot |
| Snapshot / SnapshotRecovery | `src/snapshot.hpp`；`snapshot_recovery.hpp:44` | 捕获必须由调用方保持一致切面；旁路构建后返回新簿 | snapshot |
| SnapshotFile / MarketSnapshotFile | `src/snapshot_file.hpp:106`；`market_snapshot.hpp:92` | 同步控制面文件 I/O；多簿同代文件先写、manifest 后提交 | snapshot、market_snapshot |
| AuditLog / AsyncLogger / BoundedQueue | `src/audit_log.hpp`；`async_logger.hpp:78`；`bounded_queue.hpp:30` | Audit 投影视图；Logger 独立 worker；队列是 mutex+deque，不是无锁 | async_logger、event_system |
| Monitor / CapacityMetrics | `src/monitor.hpp:69`；`capacity_metrics.hpp` | 回调线程更新、mutex 保护快照；pool/index/level 使用率 | monitor、capacity_metrics |
| CSV Loader/Parser/Validator/Replay | `src/market_data_loader.hpp`、`market_data_parser.hpp`、`data_validator.hpp`、`market_data_replay.hpp` | 同步读取报价、定点解析、校验并发布；不直接写权威簿 | market_data |
| Synthetic Generator | `src/market_data_generator.hpp` | mt19937_64 + STL 分布生成交错报价；不是原始订单命令 | market_data_generator |
| StrategyHost / Context | `src/strategy.hpp:30` | 非拥有策略指针；同线程回调；直接提交 TradingEngine | strategy |
| FeedDecoder / SequenceTracker | `src/feed_decoder.hpp:47`；`sequence_tracker.hpp:13` | 独立输入链路；decoder 拥有 pending，tracker 持有每 instrument 序号 | feed_gateway |
| MarketDataGateway / MarketDataBook | `src/market_data_gateway.hpp:37`；`market_data_book.hpp:38` | 与权威簿独立的逐订单本地状态；无同档 FIFO；缺口/非法内容→STALE | feed_gateway |
| PreTradeRisk / RiskCheckedTradingEngine | `src/risk_engine.hpp:146`；`risk_checked_trading_engine.hpp:25` | 可选模拟单账户包装器；仓位/参考价外部注入，不是交易所核心规则 | risk_engine |

## 4. 核验结论与边界

- V1 的对象池、Price Ladder、索引和 FIFO 撮合确实保留；没有重写必要。
- 命令序号是每簿“非零且递增”检查，允许跳号；当前没有多连接接入与独立 Sequencer 服务。
- 默认差分为 10 万命令；逐命令比较状态码/事件/优化簿不变量，每 25 步和最后比较完整状态。README“逐命令比较完整盘口”的表述比实现强。
- Pool 的正常撤单/成交释放顺序已检查，未发现业务路径 double-free/UAF 证据；不能把缺少防护等同已发生内存错误。
- 当前两条市场数据路径与权威撮合输出并未闭环，见 [实际架构](architecture_current.md)。
- 本轮不改 `src/`、`tests/`、构建脚本或现有 README；缺陷、候选风险和目标缺口分开列在 [风险登记](risk_register.md)。
