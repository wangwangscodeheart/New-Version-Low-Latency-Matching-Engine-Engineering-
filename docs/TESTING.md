# 当前测试体系（V2）

- Unit 与属性测试：交易规则、FIFO、撮合、撤单、容量和不变量。
- Reference Differential：默认 25 个固定种子 × 4,000 条命令，GTC/IOC/Post Only 比例为 80%/10%/10%，逐命令比较状态、事件与完整盘口。
- Recovery：Journal、单品种磁盘 Snapshot、多品种一致性 Snapshot、损坏拒绝和增量 Replay。
- Infrastructure：事件订阅、异步 Journal/Logger、Monitor、风控、Feed Decoder、Sequence Gap、Trade 扣减和行情检查点。
- CI 配置：Linux Release、ASan+UBSan、TSan 子集及 benchmark smoke；只有实际 runner 结果才算 CI 证据。

2026-08-07 已实际运行 MSVC ASan RelWithDebInfo 全量测试并通过 20/20。Linux Release、UBSan 与 TSan 因本机没有 Linux 发行版或容器工具而未执行，详见 [`VALIDATION.md`](VALIDATION.md)。

本地基线以根 README 中最近一次明确记录为准，不沿用 V1 历史文档中的 4/4 结果。
