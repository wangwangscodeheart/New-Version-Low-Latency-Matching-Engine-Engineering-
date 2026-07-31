# 面向量化回测的确定性低延迟限价订单簿与撮合引擎 — Version 2

## 1. 项目背景

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

## 5. 核心模块

### MarketManager

`MarketManager` 默认注册 AAPL、TSLA、NVDA，每个 symbol 拥有独立 OrderBook、订单 ID 空间、交易规则、容量和事件序列。同一个 OrderId 可以出现在不同 symbol 中，唯一键的业务语义是 `(symbol, order_id)`。

### Event System

`SystemEvent` 统一携带 `timestamp`、`event_type`、`symbol` 和 `payload`。payload 复用 V1 的 `Command` 和 `EngineEvent`。Dispatcher 同步且按订阅顺序执行，避免多线程回调改变事件先后关系。

### Journal

Journal 保存 `ORDER_INSERT`、`CANCEL`、派生 `TRADE` 和 `COMMAND_RESULT`。结果区分 APPLIED、业务拒绝、sequence 拒绝和未知 symbol。恢复时只重新提交订单与撤单；Trade 必须由撮合核心重新生成，再与历史 Trade 对比。Journal sequence 解决相同 timestamp 下的稳定顺序问题。

`AsyncJournalWriter` 使用独立有界队列和 writer thread 顺序追加记录。正常停止时会取消订阅、排空、flush 和 close，并通过 accepted/written/dropped/write-failed 指标报告健康状态。恢复模式只能忽略最后一条不完整尾记录；文件中间损坏始终失败。它仍不宣称具备每条记录 fsync 的断电持久性。

### Audit Log

Audit Log 回答“某个订单经历了什么”，记录 CREATE、SUBMIT、PARTIALLY_FILLED、FILLED 和 CANCELLED。它不承担系统恢复职责。

### Replay

V2 Replay 读取 Journal，复用正常的 TradingEngine → MarketManager → OrderBook 路径。完成后比较历史成交、每条命令 outcome、AAPL/TSLA/NVDA 的完整 `OrderBookState` 和 state hash，并报告首个不一致 Journal sequence。没有第二套撮合算法。

### Market Data 与数据质量

CSV 输入格式为 `timestamp,symbol,price,volume,bid,ask`。Parser 直接把十进制文本转换为四位定点整数，不经过 double。DataValidator 检查缺失字段、非法数值、时间倒退、bid 大于 ask、价格跳变和同一 symbol 长时间无行情。ERROR 行不发布，WARNING 行保留并产生 DATA_QUALITY 事件。

### Strategy

`Strategy::on_market_data()` 只能通过 `StrategyContext` 提交 Command，不能访问或修改 OrderBook。`StrategyHost` 按注册顺序同步调用策略。示例 `BuyBelowAskStrategy` 展示行情触发订单，但不代表真实投资策略。

### Subscription 生命周期

Dispatcher 返回可移动、不可复制的 RAII Subscription。Journal、Audit、Monitor、Strategy、Logger 和 Replay 都持有自己的 token；token 析构或 reset 会自动取消回调，Dispatcher 先析构也安全。

### Async Logger

```text
Trading Thread → non-blocking try_push → Bounded Queue
                                            ↓
                                      Logger Thread
                                            ↓
                                      format/write/flush
```

队列满时撮合线程不等待，而是增加 dropped counter。`stop()` 会拒绝新日志、排空队列、join、flush 并关闭文件。

### Monitor

Monitor 统计全局及每个 symbol 的订单、成交、撤单、拒绝和成交量。`TradingEngine` 使用 `steady_clock` 测量 MarketManager 路由加 OrderBook 处理时间，输出平均值和最大值。Async Logger 丢弃数也进入监控快照。

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

原 `matching_engine_benchmarks` 继续用于 V1 撮合核心性能口径。V2 benchmark 测量多品种路由、事件分发和 Monitor 接入后的系统路径，两个数字不能直接混为同一种延迟。

## 9. 测试范围

V2 新增测试覆盖：

- 默认品种注册、未知品种和品种隔离
- 同一 OrderId 跨品种共存
- 确定性事件订阅和事件类型映射
- Journal CSV 往返、损坏日志拒绝和多品种恢复
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

这是教学和面试用途的基础设施化实现，不宣称已经是交易所或券商生产系统。目前仍未实现网络行情协议、持久化二进制 WAL、崩溃一致性 fsync 策略、订单修改、IOC/FOK、市价单、多线程撮合分片、高可用复制和生产级告警系统。

Async Logger 使用有界互斥队列而非 lock-free ring buffer。该选择优先保证代码边界清晰和行为可验证；如性能数据证明队列竞争成为瓶颈，再替换实现而不改变上层接口。

## 11. 面试介绍版本

> Version 1 实现了确定性的限价订单簿和价格时间优先撮合，并用对象池、固定容量索引、Command Replay、Snapshot 和属性测试保证正确性。Version 2 没有重写撮合核心，而是在外层增加 MarketManager，把单品种状态机扩展为多品种；再用统一事件信封连接 Journal、Audit、Replay、异步日志和 Monitoring。Journal 保存可重放输入，Audit 保存订单生命周期，二者职责分离。撮合仍保持单写者和确定性，只有磁盘 I/O 通过有界队列异步执行。这样项目从撮合算法 Demo 演进为一个具备路由、恢复、审计和可观测性思维的小型量化交易基础设施。

## 12. 确定性多品种模拟行情

项目不包含交易所真实行情，也不会联网抓取数据。`MarketDataGenerator` 使用固定随机种子和整数价格刻度，生成时间交织的 AAPL、TSLA、NVDA 合成 CSV；相同配置会产生逐字节相同的文件。数据只用于验证多品种路由、行情校验、策略接入、监控与回放，不代表真实市场分布。

仓库中的 `data/sample_synthetic_market_data.csv` 是一份可直接查看和加载的最小合成样例。

```bash
./build-v2/matching_engine_market_data_generator synthetic_market_data.csv 100 20260731
```

参数依次为输出文件、每个品种的 tick 数、随机种子。上例生成 300 行。随后可由 `MarketDataLoader` 读取；V2 Demo 已完整执行“生成 CSV → DataValidator → EventDispatcher → TradingEngine”链路，并输出生成、发布和拒绝行数。
