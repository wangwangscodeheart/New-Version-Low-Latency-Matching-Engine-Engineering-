# 当前架构（V2）

本文描述当前主分支；V1 演进过程见编号历史文档。

```text
Command --> [可选 PreTradeRisk] --> TradingEngine --> MarketManager --> OrderBook[symbol]
                                      |                                  |
                                      +<---------- EngineEvent ---------+
                                      |
                           CommandJournal整命令接受（非durable）
                                      |
                      CommittedCommandSink / PublicFeedPublisher
                                      |
                     公共订单引用映射 -> FeedEncoder -> 有界整批队列
                                      |
                      drain -> GatewayFeedSink -> FeedDecoder
                                      |
                       SequenceTracker -> 独立MarketDataBook

EventDispatcher同步观察输入/内部事件 -> Audit / Monitor / Strategy / Logger
CSV报价 -> DataValidator -> MarketDataEvent（不会直接修改撮合簿）
```

撮合核心保持价格时间优先和单写者。外围模块通过事件订阅组合；外部行情使用独立 `MarketDataBook`，不污染本系统委托 `OrderBook`。

公开链路是选择启用的进程内模拟闭环，不是网络协议或持久化确认。原构造方式保留；四参数TradingEngine在Journal接受后才移交完整事件批次，不能把提前触发的Dispatcher回调当作已提交行情。发生拒收/异常后停止新命令，已应用撮合不回滚；本地簿失效为STALE，不能自动重试部分消费批次。协议、运行及验收边界见[M2闭环记录](M2_PUBLIC_FEED.md)。

