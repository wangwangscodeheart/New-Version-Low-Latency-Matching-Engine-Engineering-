# 面向量化回测的确定性低延迟限价订单簿与撮合引擎 — Version 2

## 1. 项目背景

2026-11-23第六批：改进CI配置覆盖与测试文件所有权，保留原43项测试及单写者生产实现；多测试进程并行不等于多线程撮合。不同提交/feature flag的真实Linux结果分别记录，见[第六批说明](docs/M3_CI_AND_TEST_ISOLATION.md)。

2026-11-23第五批：在原离线入口接入单遍原始源SHA-256、可选预期摘要与gzip CRC/ISIZE；zlib默认关闭，启用时不生成解压文件。保留单写者和晚到错误STALE边界，checksum不等于授权或来源认证；仍无真实市场记录，见[第五批说明](docs/M3_ITCH_INPUT_INTEGRITY.md)。

2026-11-22第四批：补显式配置的日内时间、系统阶段和市场报告账本，Broken只修正本口径统计、不复活显示单；离线检查器要求本地使用声明，未下载真实数据、未完成真实性/授权验收。旧默认API不自动获得新保证，见[第四批说明](docs/M3_ITCH_REPORT_VALIDATION.md)。

2026-11-22第三批：将Y/L/V/W/K/J/h/I/N从仅长度识别升级为类型化解码与元数据保存，目标历史wire种类增至22；K按股票正文路由、V/W保存为全市场状态、h按场所分开保留，不改变显示簿。仍无真实文件或完整业务/交易日验收，见[第三批说明](docs/M3_ITCH_METADATA.md)。

2026-11-22增量：增加`ItchBookRebuilder`，复用独立`MarketDataBook`的部分减量/替换能力，实现Stock Locate路由、全市场日内引用检查和原生可见订单生命周期。V1撮合与M2模拟发布不改；P/Q/B只做报告计数，不是完整成交账本。合成验证、状态含义和剩余门禁见[M3第二批](docs/M3_NATIVE_REBUILD.md)。

2026-11-21增量：开始M3原始Nasdaq ITCH接收端，只加入BinaryFILE读取器和核心生命周期消息解码器；尚无真实样本回放或订单簿应用。公开历史文件不是原始入场指令或UDP抓包，不能用来声称复现Nasdaq撮合。两条独立链路与授权边界见[M3说明](docs/M3_NASDAQ_ITCH.md)。

2026-11-19增量：原撮合核心保留，新增Journal整组接受后的公开行情投影、公共订单引用、二进制Encoder、有界整批输出和独立本地簿闭环；原V2 Demo已接线。最终MSVC Release/ASan均27/27，额外100万命令差分通过；设计、数据来源及故障边界见[M2记录](docs/M2_PUBLIC_FEED.md)。数据是项目合成报价/模拟订单，不是真实交易所逐笔数据；网络重传、durable prefix及新路径性能仍未验收。

Version 2 不是新项目，也没有重写 Version 1。它保留原有 `OrderBook`、Price Ladder、对象池、固定容量订单索引、价格时间优先撮合、Command Replay 和 Snapshot，在撮合核心外增加真实交易基础设施常见的路由、事件、恢复、审计和可观测性边界。

目标是把“能够正确撮合一组订单”的算法 Demo，升级为“能够管理多个品种、记录输入、复现状态并解释运行过程”的小型交易基础设施。

## 2. Version 1 与 Version 2

| 能力 | Version 1 | Version 2 |
|---|---|---|
| 撮合 | 单品种 OrderBook | 完整保留 |
| 优先级 | Price-Time / FIFO | 完整保留 |
| 内存 | Order/LimitLevel 池化 | 完整保留 |
| 确定性 | CommandSequence、EventIndex、Replay、Snapshot | 扩展到多品种系统层 |
| 品种 | 单 OrderBook | AAPL、TSLA、NVDA，多 OrderBook 路由 |
| 事件 | OrderBook 内部 EngineEvent | 带 timestamp、type、symbol、payload 的系统事件 |
| 恢复 | 单品种 Command Replay | 异步 Journal、多品种 Replay、命令结果与成交验证 |
| 审计 | 结果事件日志 | CREATE、SUBMIT、TRADE、CANCEL 生命周期 |
| I/O | 同步诊断 CSV | 有界队列和独立 Logger Thread |
| 监控 | 状态查询和 benchmark | 全局/品种指标、成交量、延迟、丢日志计数 |

## 3. 总体架构

```text
Market Data ───────────────────────────────────────────────┐
                                                          ▼
Order/Cancel ──► TradingEngine ──► EventDispatcher ──► Subscribers
                      │                    │               ├── Journal
                      │                    │               ├── AuditLog
                      │                    │               ├── Monitor
                      │                    │               └── AsyncLogger
                      ▼                    │
                MarketManager             │
          symbol → unique_ptr<OrderBook>  │
               │        │        │        │
             AAPL      TSLA     NVDA      │
               │        │        │        │
               └──── EngineEvent ─────────┘
```

`OrderBook` 仍然是单写者确定性状态机。V2 没有把日志线程、Monitor 或 symbol 字符串放进撮合数据结构。

## 4. 订单数据流

```text
SystemEvent(ORDER, timestamp, symbol, Command)
    ↓
EventDispatcher：先发布输入，供 Journal/Audit 观察
    ↓
MarketManager：根据 symbol 查找 OrderBook
    ↓
OrderBook::process(Command)：执行 V1 撮合
    ↓
Trade/Rested/Cancelled/Rejected EngineEvent
    ↓
包装成带 symbol 和 timestamp 的 SystemEvent
    ↓
Journal / AuditLog / Monitor / AsyncLogger
    ↓
ProcessingLatencyEvent
```

Market Data 可以进入同一 Dispatcher，但不会直接修改 OrderBook。只有订单命令可以改变撮合状态。

原Demo现另启用公开行情闭环：协调器先暂存完整内部事件，CommandJournal接受整组后才交给PublicFeedPublisher，显式drain后Decoder/Gateway更新独立本地簿。原始Dispatcher回调早于Journal提交，不能直接作为“已提交公开行情”；参考[新架构图](docs/ARCHITECTURE.md)。

## 5. 核心模块

### MarketManager

`MarketManager` 默认注册 AAPL、TSLA、NVDA，每个 symbol 拥有独立 OrderBook、订单 ID 空间、交易规则、容量和事件序列。同一个 OrderId 可以出现在不同 symbol 中，唯一键的业务语义是 `(symbol, order_id)`。

核心直接入口也验证Side与TimeInForce支持集合，不能绕过外围后把非法枚举当SELL/GTC。拒单保留序号/ID优先级，不改变盘口或FIFO；诊断方向非法时显示UNKNOWN。Journal预准入与核心业务拒绝属于不同层，边界和760条穷举验证见[M1第四批](docs/M1_BATCH_04.md)。

### Event System

`SystemEvent` 统一携带 `timestamp`、`event_type`、`symbol`、预解析 `InstrumentId` 和 `payload`。payload 复用 V1 的 `Command` 和 `EngineEvent`。Dispatcher 同步且按订阅顺序执行；组件连接完成后可调用 `freeze()` 固化订阅，发布路径直接遍历稳定槽位，不再逐事件复制回调数组。

V2 的 TradingEngine 使用可注入 Event Sink 接收单条命令产生的 EngineEvent。事件按 EventIndex 顺序交给 Dispatcher，全部接受后才清空 OrderBook 的临时批次；长期运行不保存全量事件历史。批次在构造时按容量预留并有硬上限。固定 Sink 返回false时，`event_output_complete=false`，整批与未接受游标保留，后续命令返回OUTPUT_BLOCKED；显式`retry_pending_events()`仅向同一逻辑输出流续送后缀，不重新撮合。Sink抛异常的接受状态不明，簿进入FAILED，禁止自动重试和快照；TradingEngine订阅异常也会停止该协调器后续接单。详见[M1事件输出契约与验证](docs/M1_BATCH_02.md)。

`NullEventSink`、`VectorEventSink` 和 `FixedEventBuffer` 分别用于无记录基准、Replay/测试及固定容量接收。V1无Sink诊断捕获保留原event reserve上限；它不是可靠投递API。Journal整命令接受现由独立CommandJournal标准接线处理，仍不等于fsync持久化或端到端Exactly-Once。

### Journal

Journal 保存 `ORDER_INSERT`、`CANCEL`、派生 `TRADE` 和 `COMMAND_RESULT`。结果区分 APPLIED、业务拒绝、sequence 拒绝和未知 symbol。恢复时只重新提交订单与撤单；Trade 必须由撮合核心重新生成，再与历史 Trade 对比。Journal sequence 解决相同 timestamp 下的稳定顺序问题。

标准接线是`TradingEngine(markets, dispatcher, journal)`（journal引用，不再attach）。`CommandJournal`在发布输入/修改簿前预留整命令记录预算，输入+Trade+最终结果组成一个批次入异步队列；预算包括stage、排队和worker正在写的vector容量。失败区分未准入和已应用但未完整接受，后者停止接单，不假装回滚。

新文件以`DME_JOURNAL,2,COMMAND_BATCHES`标头识别，读取与Replay先验证完整命令组。宽容EOF恢复丢整条未完成尾命令，不保留孤立输入；中间损坏和完整行身份错误仍拒绝。旧11/12/13列legacy读取及attach观察模式保留，但不提供相同的撮合前预算保证，且不能与标准模式混用。正常stop排空flush；无CRC、fsync或durable水位，不能称断电持久化。详见[M1整命令Journal契约与证据](docs/M1_BATCH_03.md)。

### Audit Log

Audit Log 回答“某个订单经历了什么”，记录 CREATE、SUBMIT、PARTIALLY_FILLED、FILLED 和 CANCELLED。它不承担系统恢复职责。

冷路径导出现在检查header/记录写入、flush及拥有文件的close；新借用ostream入口flush但不close，使用当前flags/locale（正常默认十进制）。失败抛给调用者，内存记录可重新导出，不回滚已发生撮合。filename仍为文本模式trunc写入，可能留下部分文件，不提供原子替换或断电保证；详见[M1第十批](docs/M1_BATCH_10.md)。

### Replay

V2 Replay 读取 Journal，复用正常的 TradingEngine → MarketManager → OrderBook 路径。完成后比较历史成交、每条命令 outcome、AAPL/TSLA/NVDA 的完整 `OrderBookState` 和 state hash，并报告首个不一致 Journal sequence。没有第二套撮合算法。

### Disk Snapshot

单 OrderBook 的 Snapshot 支持版本化二进制落盘。文件只保存显式整数配置和订单字段，不保存指针、对象池地址或 STL 容器布局；尾部 checksum 检测损坏和截断。写入流程为 `snapshot.tmp → flush/fsync → 重新读取校验 → 原子替换 current`。Linux 在 rename 后同步父目录，Windows 使用 `FlushFileBuffers` 与 write-through replace。多品种层使用代际文件：先完整写入并验证每个 symbol 的同代 Book 文件，最后以同样的刷新与替换流程提交带 checksum 的 manifest。恢复先在独立 MarketManager 中恢复全部品种，随后只回放 `journal_sequence > snapshot_sequence` 的记录。它仍不承诺跨设备 rename、磁盘控制器行为、复制或生产级高可用语义。

多品种保存现先检查整代Book/临时路径，再创建不自动清理的`.reserved`占位；一旦占位成功，同manifest下的同generation即使后续失败或状态相同也不能复用。占位前校验失败不消耗generation，旧v1布局照常加载。重试需真实有效的新截点或另一个manifest命名空间；没有自动GC，也不把保存异常一律解释为未发布，详见[M1第八批](docs/M1_BATCH_08.md)。

### Market Data 与数据质量

CSV 输入格式为 `timestamp,symbol,price,volume,bid,ask`。Parser 直接把十进制文本转换为四位定点整数，不经过 double。DataValidator 检查缺失字段、非法数值、时间倒退、bid 大于 ask、价格跳变和同一 symbol 长时间无行情。ERROR 行不发布，WARNING 行保留并产生 DATA_QUALITY 事件。

### Strategy

`Strategy::on_market_data()` 只能通过 `StrategyContext` 提交 Command，不能访问或修改 OrderBook。`StrategyHost` 按注册顺序同步调用策略。示例 `BuyBelowAskStrategy` 展示行情触发订单，但不代表真实投资策略。

### Subscription 生命周期

Dispatcher 返回可移动、不可复制的 RAII Subscription。Journal、Audit、Monitor、Strategy、Logger 和 Replay 都持有自己的 token；token 析构或 reset 会自动取消回调，Dispatcher 先析构也安全。

### Async Logger

```text
Trading Thread → try_push（不等待空位）→ Bounded Queue
                                            ↓
                                      Logger Thread
                                            ↓
                                      format/write/flush
```

队列满时不等待空位，而是增加dropped counter；mutex仍可能等待，不能称无锁或严格非阻塞。新增健康状态及accepted/written/failed/dropped计数；写失败不再虚增written，flush/close错误也可见。healthy只表示未观察到流/worker错误，不能替代dropped检查，更不是fsync证明。诊断Logger仍best-effort，不替代Journal完整组契约或生命周期AuditLog。

生产者/Dispatcher静止后，由一个控制线程stop：拒绝新入队，worker处理/标记失败尾部并flush及关闭拥有的文件，控制线程join。只支持顺序重复stop，不承诺并发stop都等待完成。可借用ostream但不负责close，流须活到join结束且无并发访问；header写入和首次flush属于冷初始化。详见[M1第九批及故障证据](docs/M1_BATCH_09.md)。

### Monitor

Monitor 统计全局及每个 symbol 的订单、成交、撤单、拒绝和成交量。已注册品种通过 `InstrumentId` 更新预分配指标槽位，避免逐事件字符串哈希；未知品种和独立数据质量事件走兼容回退表。`TradingEngine` 使用 `steady_clock` 测量预解析InstrumentId的MarketManager/OrderBook处理及同步回调时间，输出平均值和最大值；symbol解析发生在该计时段之前。Async Logger 的accepted/written/failed/dropped及健康状态也进入snapshot/CLI；运行中这些原子计数不是事务快照。当前仍以互斥锁保护 CLI 并发快照，这一点应计入端到端性能口径。

mutex也用于每次`on_event`更新，不仅用于CLI读取。运行时Monitor没有P50/P99/P99.9直方图；当前计时包围`MarketManager::process`及其同步事件回调，不覆盖后续Journal整组提交、公开投影入队和消费者应用完成。它不是完整交易链路延迟，也不能用Benchmark离线分位数冒充运行时监控指标。

## 6. Journal 与 Audit 为什么必须分离

Journal 是状态恢复输入，要求顺序稳定、格式严格、可重新执行。Audit Log 是面向订单生命周期、调试和审计的业务视图。同一份文件同时承担两种职责，会导致输出事件被误当成输入、格式升级困难，并可能在恢复时重复应用成交。

## 7. 为什么采用 Event Driven

撮合核心只负责订单状态转换。Journal、Audit、Monitor 和 Logger 通过事件订阅接入，不需要修改撮合算法。新增风险检查、策略网关或外部行情适配器时，可以继续沿用这一边界。

事件驱动不等于所有模块都异步。撮合和事件分发保持同步确定性；只有磁盘 I/O 放到独立线程。

## 8. 构建与运行

```bash
cmake -S . -B build-v2 -DCMAKE_BUILD_TYPE=Release
cmake --build build-v2 --config Release
ctest --test-dir build-v2 -C Release --output-on-failure
```

运行 V1 Demo：

```bash
./build-v2/matching_engine_demo
```

运行 V2 Demo：

```bash
./build-v2/matching_engine_v2_demo
```

V2 Demo 会生成：

- `trading_v2.journal.csv`：恢复输入和成交校验证据
- `trading_v2.audit.csv`：订单生命周期
- `trading_v2_async.log`：异步统一事件日志

运行 V2 基准：

```bash
./build-v2/matching_engine_v2_benchmarks
```

原 `matching_engine_benchmarks` 继续用于 V1 撮合核心性能口径。原七个V2 benchmark场景使用同一确定性挂单/撤单负载，分别输出预解析 `InstrumentId` 的 Core、字符串 Routing、冻结 Dispatcher、Monitor、AsyncLogger、AsyncJournal 和 Full V2。每层包含独立预热、5 个全新 fixture 的 batch 中位数，以及与 batch 分离的逐笔 Median/P99/P99.9/Max；非零 checksum 验证 batch 与采样运行得到一致结果。输出同时包含 commit、CPU、操作系统、编译器和 Build Type 元数据。

这里的`full_v2`不包含M2新增的PublicFeedPublisher/Encoder/公开队列/Decoder/MarketDataBook，不能代表现在的完整公开行情链路。新链路的独立分层、分配及受控吞吐/分位数验收仍待完成；下述合成工程初测、旧纳秒微基准和CI smoke均不能替代正式门禁。见[分阶段性能验收](docs/BENCHMARK.md)。

同日后续已新增`journal_public_feed_sync`、`full_v2_public_feed_sync`、`full_v2_public_feed_burst16`三个独立合成工程场景，不替换原七场景。新场景检查实际本地消费后再形成延迟样本，排除无公开增量命令，并报告消息/字节/队列高水位；保留3进程原始结果和二进制身份。它是初步工程基线，分配与分层成本、真实数据和受控性能仍未验收；边界、负载及复现命令见[BENCHMARK](docs/BENCHMARK.md)。

一次交互运行只用于冒烟，不能作为正式性能结论。正式结果必须在固定机器、电源模式和后台负载下运行多个全新进程并保存完整原始输出。当前五进程 Windows 基线、环境 JSON、逐轮 CSV 与完整 stdout 位于 `benchmarks/results/2026-08-07-v2-baseline/`；`scripts/run_benchmark_windows.ps1` 可重新生成一组不覆盖旧证据的新目录。Batch Average 与逐笔采样使用不同计时边界，不能把两者数值直接混用。

## 9. 测试范围

V2 新增测试覆盖：

- Reference Engine 差分验证：25 个固定随机种子、每个 4,000 条混合命令，比较 ProcessStatus、逐条 EngineEvent 与完整 OrderBookState；

- 默认品种注册、未知品种和品种隔离
- 同一 OrderId 跨品种共存
- 确定性事件订阅和事件类型映射
- Journal CSV 往返、损坏日志拒绝和多品种恢复
- 磁盘 Snapshot 往返、截断和 checksum 损坏检测
- 多品种一致性 Snapshot、manifest 损坏拒绝及 Snapshot 后增量 Journal Replay
- CSV 行情解析和 DataValidator warning/error
- Strategy 经正常订单路径提交及非法行情隔离
- 历史 Trade 与重放 Trade 一致性
- 命令 outcome、首个差异 sequence、完整状态和 state hash
- Audit 生命周期与剩余数量
- Audit 的业务拒绝、sequence 拒绝和未知 symbol
- 有界队列、异步排空、flush/close 和日志计数
- 异步 Journal 与不完整尾记录恢复
- Dispatcher RAII 取消订阅和析构顺序
- 全局/按品种 Monitoring 与处理延迟

所有 V1 Unit、Property、Snapshot 和 Replay 测试仍然保留。

## 10. 当前边界

这是教学和面试用途的基础设施化实现，不宣称已经是交易所或券商生产系统。目前仍未实现真实交易所网络协议、持久化二进制 WAL、完整崩溃一致性 fsync 策略、FOK、市价单、订单修改、自成交保护、多线程撮合分片、高可用复制和生产级告警系统。

Async Logger 使用有界互斥队列而非 lock-free ring buffer。该选择优先保证代码边界清晰和行为可验证；如性能数据证明队列竞争成为瓶颈，再替换实现而不改变上层接口。

## 11. 面试介绍版本

> Version 1 实现了确定性的限价订单簿和价格时间优先撮合，并用对象池、固定容量索引、Command Replay、Snapshot 和属性测试保证正确性。Version 2 没有重写撮合核心，而是在外层增加 MarketManager，把单品种状态机扩展为多品种；再用统一事件信封连接 Journal、Audit、Replay、异步日志和 Monitoring。Journal 保存可重放输入，Audit 保存订单生命周期，二者职责分离。撮合仍保持单写者和确定性，只有磁盘 I/O 通过有界队列异步执行。这样项目从撮合算法 Demo 演进为一个具备路由、恢复、审计和可观测性思维的小型量化交易基础设施。

## 12. 确定性多品种模拟行情

项目不包含交易所真实行情，也不会联网抓取数据。`MarketDataGenerator` 使用固定随机种子和整数价格刻度，生成时间交织的 AAPL、TSLA、NVDA 合成 CSV；相同配置会产生逐字节相同的文件。数据只用于验证多品种路由、行情校验、策略接入、监控与回放，不代表真实市场分布。

## 13. 简化二进制行情接入

`FeedDecoder` 按网络字节序解析固定 24 字节帧头以及 Add/Cancel/Trade 消息，流式处理半包、多包、非法长度、未知类型、非法方向和不完整尾包。`SequenceTracker` 按 `InstrumentId` 检测 gap、重复和倒退；`MarketDataGateway` 拒绝非法品种，重复与乱序消息不会更新状态。

外部行情由独立 `MarketDataBook` 重建，绝不直接送入撮合 `OrderBook`。Add 建立订单，Cancel 删除订单，Trade 携带被动订单 ID 并扣减剩余数量和价格档。Sequence Gap 不推进已确认序号、不应用缺口消息，并将对应品种置为 `STALE`；连续序号下若 Cancel/Trade 等业务内容无法应用，也会立即置为 `STALE`，避免在已不可信的簿上继续处理。显式恢复完整 checkpoint 后才重新接受连续消息。`MarketDataGateway` 的内存检查点同时捕获各品种行情簿、各通道最后序号、FeedState 和容量指标，恢复先完整校验再替换在线状态。该协议仍不包含真实交易所组播、重传通道和外部快照服务。

## 14. 容量观测与盘前风控

`OrderBook::capacity_metrics()` 暴露当前和峰值活动订单/价位、订单与价位容量、索引负载以及事件缓冲容量，用于压测后判断预分配是否合理；指标读取不参与撮合决策。

`PreTradeRisk` 位于 `TradingEngine` 之前，使用 `InstrumentId` 索引的增量状态。冷启动绑定时可重建一次已有挂单；热路径不再调用 `symbols()` 或 `capture_state()`，而由 Rested/Trade/Cancel 事件 O(1) 更新活动订单、买卖挂单量和 pending notional。仓位上限使用 `position + open_buy + new_buy` 或 `position - open_sell - new_sell` 的保守最坏成交投影；Kill Switch 拒绝新订单但允许降低风险的撤单。主动成交后的真实持仓仍由外围账户组件注入，因此这是盘前风险骨架，不是完整实时持仓/PnL/保证金系统。

该包装器是可选上游接入层；直接调用TradingEngine的订单不会自动经过它。账户盘前控制应归属券商/交易接入或策略执行侧，不应宣传成交易所撮合核心的一部分。现有挂单预算更新不等于已完成“成交→账户持仓→资金”的自动、幂等核算链路。

价格偏离与仓位现用精确整数比较，覆盖MIN/MAX及全宽数量；跨档累计量无法表示或导入/回调失败后，风险状态失效，新单返回STATE_UNAVAILABLE。风险层仍放行撤单，但FAILED的Engine不会因此恢复；完整重绑也不清除Engine失败。金额/敞口仍是浮点估算，详见[M1第六批及证据](docs/M1_BATCH_06.md)。

## 15. IOC 与 Post Only

`NewOrderCommand` 新增 `TimeInForce`，旧调用默认保持 GTC。IOC 按正常价格时间优先级立即撮合，未成交余量生成取消事件且不进入价格档位；Post Only 在修改状态前检查是否会立即成交，会穿价时以 `POST_ONLY_WOULD_TRADE` 拒绝，否则作为普通被动限价单挂入。Journal 新增 `time_in_force` 列并继续兼容旧 11/12 列文件。端到端测试覆盖 Journal 落盘、重新加载、TIF 字段保真和 Replay 后订单簿一致性，确保 IOC/Post Only 不会在恢复时退化为 GTC。

## 16. CI 与 Sanitizer

CI包含Linux Release、ASan+UBSan、异步组件与CommandJournal的TSan配置及缩小负载benchmark smoke。2026-11-20，`b889136`的[真实runner](https://github.com/wangwangscodeheart/New-Version-Low-Latency-Matching-Engine-Engineering-/actions/runs/44668927537)已通过Release 27/27、ASan+UBSan 27/27、TSan子集4/4及smoke；手动压力任务未触发。CI延迟不是正式性能结论，TSan子集不是完整引擎的并行撮合证明。此前本机结果见[M2记录](docs/M2_PUBLIC_FEED.md)和[M1第十批](docs/M1_BATCH_10.md)；独立分配故障程序替换new/delete的ASan覆盖限制仍见[M1第五批](docs/M1_BATCH_05.md)。

普通 CTest 的差分测试使用 25 × 4000 条命令以保持反馈速度。`workflow_dispatch` 提供手动压力任务，运行 100 × 100000 条确定性命令；测试程序也接受 `--seeds`、`--commands` 和 `--seed-base`。失败会报告 seed 和首个不一致 sequence，可将 commands 限制为该 sequence 重放最短失败前缀。

仓库中的 `data/sample_synthetic_market_data.csv` 是一份可直接查看和加载的最小合成样例。

```bash
./build-v2/matching_engine_market_data_generator synthetic_market_data.csv 100 20260731
```

参数依次为输出文件、每个品种的 tick 数、随机种子。上例生成 300 行。随后可由 `MarketDataLoader` 读取；V2 Demo 已完整执行“生成 CSV → DataValidator → EventDispatcher → TradingEngine”链路，并输出生成、发布和拒绝行数。
