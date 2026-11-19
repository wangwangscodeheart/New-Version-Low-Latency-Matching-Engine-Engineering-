# M0：当前真实架构与数据流

基线 `c690dfd`，2026-11-11。系统定位：交易所侧连续限价簿的工程模拟/研究平台；已有单账户 Risk 是可选外围模拟，不把系统改称买方实盘 OMS。

## 1. 当前链路（不是目标图）

```text
进程内 Command / StrategyContext
             |
  [可选 RiskCheckedTradingEngine]    非所有入口必经
             |
             v
       TradingEngine ---------------------> EventDispatcher
             |                        ORDER / EngineEvent / outcome+latency
             v                                  |   |   |   |
       MarketManager                        Journal Audit Monitor Logger
       symbol -> InstrumentId                  |               |
             |                             有界队列         有界队列
             v                                 |               |
   独立 OrderBook[AAPL/TSLA/NVDA]          Journal worker     Logger worker
   OrderPool / Index / Levels / Ladder          |               |
                                               v               v
                                            CSV file         Log file

独立报价输入：
CSV -> Loader -> Parser -> DataValidator -> MARKET_DATA -> StrategyHost
                                     \-> DATA_QUALITY -> Monitor/Logger

独立 binary feed 输入（当前未连接上面权威簿）：
人工 wire -> FeedDecoder -> MarketDataGateway -> SequenceTracker
                                         \-> MarketDataBook + VALID/STALE

恢复控制面：
SnapshotFile / MarketSnapshotFile -> 新建 OrderBook / MarketManager
Journal 的输入记录 -> ReplayEngine -> 正常 TradingEngine 路径
历史 Trade/Outcome -> 比较，不再次修改簿
```

`TradingEngine` 持有外部引用，不拥有簿；`MarketManager` 的 unique_ptr 拥有各簿。`OrderBook` 拥有对象池、索引、档位目录、Ladder 和命令临时事件；链表/索引/Ladder 借用池内指针。文件控制面与回放不应和 live writer 并发访问簿。

## 2. 当前一次命令的时间线

```text
入口 gate 检查
 -> 发布 ORDER（Journal 可能在这里入队失败）
 -> 每簿 sequence gate / 业务校验
 -> 修改权威簿并生成命令事件批次
 -> 推送各事件到 Sink / Dispatcher
 -> 发布 outcome+处理时延
 -> 返回 MarketProcessResult
                 ... worker 在另一线程写入，停止时 flush
```

出处：`src/trading_engine.hpp:44`、`:52`、`:64`、`:72`；`src/orderbook.hpp:168`；`src/async_journal.hpp:35`。

目前有四个不能混用的完成状态：

1. `ProcessStatus::APPLIED`：进程内核心已经应用；不表示日志/公开行情成功。
2. Journal 入队/ostream written：不等于 fsync durable；也不证明该命令所有记录完整。
3. 公开行情已发布：尚无权威簿公开发布模块，不能声称实现。
4. 独立消费者已应用：当前 Feed 的本地状态，只覆盖收到的简化行情，不证明与本系统权威簿一致。

## 3. 并发与顺序

- MarketManager、OrderBook、Dispatcher 在同一调用线程执行。只有文件 worker 异步；Event Driven 不等于异步撮合。
- 每簿 CommandSequence 非零递增、允许跳号，不是全市场全局排序；JournalSequence 用于记录全序。timestamp 本身不是唯一顺序标识。
- BoundedQueue 使用 mutex、deque 和 condition_variable；try_push 不等待空位，但可能等待锁/分配，不能称 wait-free。
- subscribe/reset/freeze/stop 必须服从单线程控制约定；队列线程安全不代表 Dispatcher 或簿支持多生产者调用。
- Monitor 的 mutex 及同步订阅回调也会进入交易线程耗时。TradingEngine 内测时不包括入口 ORDER 回调、最终 latency 回调或日志排空。

## 4. 三种簿 / 数据身份

| 状态 | 当前含义 | 可以比较什么 |
|---|---|---|
| 权威 OrderBook | 进程内订单入口命令形成的全量 FIFO 簿 | 同规则参考模型的完整状态与事件 |
| CSV MarketDataEvent | price/volume/bid/ask 报价，不是逐订单流 | 校验、策略输入；不能反推原始 New/Cancel |
| MarketDataBook | 简化 L3-like Add/Cancel/Trade 按 order_id 重建；不含同档 FIFO | 可见订单及聚合深度；不能声称恢复真实优先队列 |

真实 ITCH 是公开市场数据，不是订单入口协议；本轮没有下载真实数据，也没有添加协议兼容声明。

## 5. 增量改造接缝

M1 先补提交/输出可靠性与边界回归，保留既有匹配算法和每簿单写者。M2 才从已确认簿变化生成明确可见性的公开事件，经独立编码/解码驱动本地簿；私有执行回报与公开行情分开。M3 在已定义完整命令边界上补 durable prefix、快照增量拼接和故障恢复。

这些接缝的状态、选择理由和门禁见 [首批 ADR](decisions.md)。
