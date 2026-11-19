# M0：四栏能力矩阵

2026-11-19后续进展：已新增可选的Journal接受后公开投影→FeedEncoder→有界整命令批次→Decoder/Gateway→独立本地簿闭环，见[M2记录](../M2_PUBLIC_FEED.md)。仅为进程内模拟，真实网络、持久化前缀、重传/快照同步及受控性能仍开放；下表保留M0冻结事实，不将当时“无encoder/publisher”误读为当前状态。

基线：`c690dfd8b008e06690e914e272173a7f4e55d164`；2026-11-11。矩阵以源码与本轮运行结果为准，不是需求完成清单。“待核验”不表示已经实现。

| 源码已证实 | 文档声称 | 待核验 | 目标新增 / 修正 |
|---|---|---|---|
| `OrderBook` 价格优先、FIFO、被动价成交、部分/跨档成交；unit/property 通过 | README 撮合规则 | 目标市场规则不等价于真实交易所全规则 | 补人工黄金状态机与拒单优先级契约；不重写算法 |
| GTC/IOC/Post-Only；无 Market/FOK/Replace | README 已明确边界 | 极端容量与组合拒绝规则 | 修正满池 Post-Only 参考模型差异；不新增订单类型 |
| 整数 price/quantity、tick/lot/单笔校验 | README 强类型、规则检查 | Risk 极值 signed arithmetic；非法 enum 的 API 边界 | 同档累计量溢出防护、非法枚举策略 |
| 每簿 sequence 严格递增但可跳号；EventIndex 命令内顺序 | README CommandSequence 准入 | 命令/会话/成交 ID 的生产业务身份 | 明确序列域与幂等策略，不宣称已有 Sequencer 服务 |
| 订单池、档位池、固定索引、Ladder+map | README 固定容量与快速访问 | bad_alloc 中途失败；ObjectPool 非法释放防护 | 先做失败注入/生命周期不变量，不先换数据结构 |
| 独立 ReferenceEngine；默认 25×4,000 差分通过 | README 称逐命令比较完整盘口 | 实际全簿是每25步/最后；未执行千万命令 stress | 黄金边界、失败命令持久化与反例缩减 |
| AAPL/TSLA/NVDA 独立簿、InstrumentId 路由 | README 多品种支持 | 多生产者并发准入没有实现 | 明确单簿单写者；分片须等基准证据 |
| 同线程 EventDispatcher、RAII token、freeze | README 事件驱动 | 订阅异常、重入提交与跨线程生命周期 | 明确回调失败契约、禁止破坏有序性的重入 |
| Sink false 可检测，但状态已应用、输出后缀被清空 | 代码注释称不静默丢弃 | 下游是否能恢复；现有接口没有补发保留 | M1 P0：完整命令的提交与可靠输出边界 |
| 异步 Journal 溢出可见；gate 只阻止后续命令 | README_V2 对“下一条”语义诚实 | 本条记录不完整时恢复到哪里 | 整命令日志框架、提交边界与故障测试 |
| CSV Journal 输入/Trade/结果分离；Replay 重新执行输入并比较输出 | README Journal/Replay | 尾部整命令完整性、崩溃恢复 | durable prefix、版本/长度/校验/同步策略；不冒称 WAL |
| 单文件版本+checksum；多簿 manifest 最后替换；损坏测试通过 | README 明确不保证严格断电一致性 | 同 generation 重用、kill -9、目录同步失败 | 唯一 generation、完整命令截点、恢复编排 |
| Audit 生命周期投影与 Journal 分责 | README Audit 追踪 | 丢弃日志时生命周期完整性；无磁盘失败测试 | Audit 的完整/降级标记、写失败可观测性 |
| 有界 mutex+deque 队列、Logger worker、满队列丢弃计数 | README_V2 图称 non-blocking try_push | mutex 等待、deque 分配与 written 成功语义 | 收窄非阻塞表述；明确队列容量/磁盘错误策略 |
| Monitor 订单/成交/撤单/拒绝/时延；质量全局统计 | README 按 symbol 指标 | 已复现 registered-symbol 质量计数在 snapshot 被覆盖 | 合并或统一指标槽；明确定义时延边界 |
| CSV 报价定点解析、时间/跳价/缺失检查 | README Market Data | “长时间无行情”主要依赖后续输入触发，不是独立定时心跳服务 | 后续失联检测与事件时间约定 |
| 合成报价生成器与少量样例 | README 明确非真实数据 | 跨 STL 同种子字节一致性未证明；没有 manifest | 保存版本化输入、哈希、参数；真实数据先过许可门禁 |
| 简化 binary Add/Cancel/Trade 解码，含 passive_order_id | README 明确不是交易所协议 | 未接任何真实 ITCH/NYSE 源 | 明确 L3-like 可见性；协议/数据适配单独验收 |
| Gateway 缺口与非法内容→STALE；checkpoint 原子内存替换 | README Feed + 恢复 | 首序号任意接受；无实时快照增量拼接/重传窗口 | 快照基准序号、恢复状态机、重传/重同步 |
| MarketDataBook 独立于 OrderBook，不保留 FIFO | docs/ARCHITECTURE 图易误读为共同入口 | 无权威簿→公开 feed encoder/publisher | M2：公开 Builder/Publisher→独立 Feed→本地簿一致性闭环 |
| 风控包装器含在途订单仓位预算，测试通过 | docs 限定单账户教学模型 | 策略直连可绕过；成交不自动更新账户仓位 | 明确模拟账户模型与统一入口，不混称交易所业务 |
| MSVC Release 20/20；本次 ASan 20/20 | 历史文档有 2026-08-07 记录 | Linux/UBSan/TSan runner 本轮未执行 | 保留真实日志；CI 配置≠验证完成 |
| 当前提交五进程×七场景 Benchmark、35 行结果、七个文件哈希通过 | 历史窄场景 26.7 ns，不代表当前端到端 | 当前无 P95/排队/RSS/perf counters/置信区间 | M4 同输入负载与分层测量；不先制定虚假 SLA |

本轮诊断探针实际复现 R01–R05，详情和原始输出见 [测试基线](baseline_test_report.md)。矩阵的“新增”均为未来工作，不表示本轮已实现。
