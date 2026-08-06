# 当前架构（V2）

本文描述当前主分支；V1 演进过程见编号历史文档。

```text
Command / Binary Feed
        |
        +--> PreTradeRisk (InstrumentId 增量状态)
        |        |
        |        v
        +--> TradingEngine --单写者--> MarketManager --> OrderBook[symbol]
                 |                         |
                 v                         v
          EventDispatcher             EngineEvent
            |   |   |   |
         Journal Audit Monitor Strategy/Logger
```

撮合核心保持价格时间优先和单写者。外围模块通过事件订阅组合；外部行情使用独立 `MarketDataBook`，不污染本系统委托 `OrderBook`。

