# M2：提交后公开行情与独立本地簿闭环

日期：2026-11-19（Asia/Tokyo）。保留V1撮合核心，增量完成进程内模拟行情链路；不是交易所接入、生产WAL或整个升级计划的最终验收。

## 改了什么，为什么

| 文件/模块 | 增量和真实组件对应 | 面试说明 |
|---|---|---|
| `committed_command_sink.hpp` / `trading_engine.hpp` | Journal接受完整命令后才移交事件；单写者协调与发布边界 | APPLIED、日志接受、行情入队、下游应用是不同阶段 |
| `public_feed_publisher.hpp` | 公开引用映射、事件投影、整命令有界字节队列 | 主动单私有ID不进入公共Trade；只公开被动剩余挂单变化 |
| `feed_encoder.hpp` | 与既有Decoder配对的显式大端编解码 | 不发送C++结构体内存；固定帧成功编码无堆分配 |
| `GatewayFeedSink` / `market_data_gateway.hpp` | 完整批次解码、序号检查和消费者失效保护 | 缺口/非法消息/异常后本地簿不再可信，不能仅打印warning后继续 |
| `market_data_book.hpp` | Add非法方向、档位溢出及分配失败的事务保护 | 独立重建本地深度，不能共享权威簿内存来“证明一致” |
| `v2_main.cpp` / 新增测试 | AAPL/TSLA/NVDA实际撮合→二进制→独立本地簿 | CSV报价与撮合生成的公开订单流是两条不同数据路径 |

## 接线与使用

```cpp
MarketManager markets(1024);
EventDispatcher dispatcher;
AsyncJournalWriter journal("session.journal.csv", 4096);
MarketDataGateway local(markets.symbol_count());
GatewayFeedSink consumer(local);
PublicFeedPublisher publisher(markets, consumer, 65536);
TradingEngine engine(markets, dispatcher, journal, publisher);
// engine.submit(...); 检查APPLIED/拒单、event_output_complete
// publisher.drain(); 检查返回值和consumer的健康状态
// 停止输入后journal.stop()并检查healthy；stop不等于断电持久化。
```

原V2 Demo已使用这条链路并保留原来的CSV、Audit、异步日志和Journal Replay演示。运行`build/m2-public-release/matching_engine_v2_demo.exe`，应出现`Public feed: ... result=PASS`。日志文件写在进程工作目录；不要在有重要同名证据的目录运行Demo。

### 会话约束

初始化要求空、ready且开启事件记录的撮合簿，以及空、HEALTHY、序号0的消费者。公开会话冻结品种及簿身份；全部命令必须经过同一个TradingEngine。注册/恢复/替换簿、绕过协调器直接撮合或回调中修改簿不属于支持操作。所有对象由同一个写线程拥有，队列不是跨线程总线，回调与对象移动/销毁必须遵守单所有者生命周期。

构造时预留完整内部事件暂存；标准CommandJournal拒绝提交时不进入公开发布器。公开批次入队后由显式`drain()`应用；成功入队不等于消费者已经应用，更不等于磁盘durable。旧三参数及无Journal构造保持原功能，不自动获得该保证。

## 模拟wire schema

所有整数大端。Header固定24字节：type(u16)、body_length(u16)、InstrumentId(u32)、sequence(u64)、timestamp(u64)。

| 消息 | body | 完整字节 |
|---|---|---|
| Add(1) | public_order_ref:u64, price:i64正值, remaining:u64, side:u8(0/1) | 49 |
| Cancel(2) | public_order_ref:u64 | 32 |
| Trade(3) | public_trade_ref:u64, passive_public_order_ref:u64, price:i64正值, quantity:u64 | 56 |

只发送`EncodedFeedFrame::size()`字节，不发送固定数组尾部或对象布局。无magic/version/session/symbol字典/校验和；InstrumentId注册顺序须由带外共享。不兼容任何真实ITCH/NYSE协议。

每品种公开序号从1开始，独立于CommandSequence；无公开变化的拒单/IOC剩余撤销不消耗公开序号。Trade引用取其公开消息序号；订单引用由公开映射分配，撤单/全部成交后复用私有OrderId会分配新引用。它不复制私有ID，但数值偶然相等不代表匿名化或加密保证。Trade不携带主动单ID。

Rested映射为Add；Trade扣减公开被动单；真实撤单映射为Cancel；从未公开挂簿的IOC主动剩余撤销不发Cancel；内部拒单不广播。比较可见订单的价格/方向/剩余量和全部档位聚合，不比较消费者不存在的FIFO、账户和私有字段。

## 容量与故障契约

字节预算约束未消费完整批次的精确wire缓冲（含drain中批次），不声称覆盖deque节点、映射、解码暂存或总堆内存。公共订单映射按源簿容量限制。投影/映射/队列仍可分配，并不是零分配热路径；只有成功单帧编码采用固定存储。没有同步磁盘I/O，也没有无锁/异步网络承诺。

超预算、分配异常、输出拒收、解码异常或本地业务非法均终止公开会话并使消费者STALE。队列不静默丢弃后继续交易。若失败在Journal接受后，核心可能已经APPLIED且Journal保留命令；返回`event_output_complete=false`并停止后续准入，绝不把已撮合命令当未执行重投。投影或消费可能已经产生部分状态，终止后不再使用这些状态作为可信盘口。

GatewayFeedSink接收完整命令批次，不是任意网络分片接口；每次consume末尾检查finish，截断立即失效。底层Decoder仍支持分片，但回调异常后可能保留已投递前缀；适配器因此终止并重置，不对部分批次自动重试。新会话恢复需要可信空基线/独立重建；自动快照拼接、重传及崩溃后的公开序号续接未实现。

既有直接Gateway保持重复/旧序号丢弃且健康、缺口STALE的语义；严格整批适配器将任何拒收视为该闭环失败。普通Gateway首次可接受任意序号；适配器显式锚定0，避免首次丢失前缀仍健康。

## 验证与边界

编码测试含手写大端黄金值、37条非法输出组合、137字节三帧串的138个切点及三品种重建。新增提交边界、公开投影/有界队列和消费者分配故障回归。最终全量结果与源码身份统一写入[验证记录](VALIDATION.md)，原始日志位于`validation/2026-11-19-m2-public-feed/`。

最终MSVC Release **27/27**（1.71s）、MSVC ASan **27/27**（8.94s），额外100万命令差分通过。新增消费者7轮扫描、50次分配失败及7个成功边界通过；独立故障程序的分配替换限制部分ASan诊断，不能将其当成全分配点sanitizer证明。初次60/60完整构建与最终受影响目标重建的日志分别保留，没有把测试隔离修正前的构建冒充最终证据。

本轮数据来源明确：CSV仍由已有MarketDataGenerator合成；公开Add/Cancel/Trade由Demo/测试输入的模拟订单经真实项目撮合生成。没有下载或接入真实AAPL/TSLA/NVDA历史逐笔数据，不宣称样本代表真实市场分布。

11月19日归档时的基线为`c690dfd8b008e06690e914e272173a7f4e55d164`加未提交工作区增量；[累计源码patch](validation/2026-11-19-m2-public-feed/source_changes.patch)与[47项Git blob清单](validation/2026-11-19-m2-public-feed/source_git_blobs.txt)冻结当时的构建/源码/相关脚本，不包括不断增长的说明文档或任意checkout换行字节。补丁针对基线，不要重复应用到当前工作区；当时独立index重建47/47匹配、真实暂存未变。该增量随后已于`4f32187`入库并推送；11月20日`b889136`只修正编码测试的GCC类型比较，因此旧清单不是要求之后每个HEAD都匹配的实时清单。

VS x64 Developer cmd复验：

```bat
chcp 65001
cmake -S . -B build/m2-public-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/m2-public-release --parallel 6
ctest --test-dir build/m2-public-release --output-on-failure
cmake -S . -B build/m2-public-asan -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DENABLE_ASAN=ON -DENABLE_UBSAN=OFF
cmake --build build/m2-public-asan --parallel 6
ctest --test-dir build/m2-public-asan --output-on-failure
build\m2-public-release\matching_engine_allocation_failure_tests.exe --feed-consumer-only
build\m2-public-release\matching_engine_differential_tests.exe --seeds 100 --commands 10000 --seed-base 0xD1FF0000
```

没有对新发布路径做受控吞吐/分位数验收；原full_v2 Benchmark不包含这条公开路径，不能拿旧延迟当新链路结果。11月20日已补真实Linux Release、ASan+UBSan、TSan子集及smoke通过证据，见[VALIDATION](VALIDATION.md)；真实磁盘满/强杀/断电、durable prefix、快照重同步及生产账户系统仍未整体验证。M1针对性修复保留，但不能据此关闭全部升级大纲。

回滚只撤本批可选发布接线、encoder/consumer保护及对应测试/文档；保留此前十批成果。不要把累计补丁直接反向应用到已有用户修改的工作区。

## M2收口后的建设顺序（2026-11-20评审）

M2作为阶段性节点保留，不立即启动第三轮大规模模块开发。下面采用本次评审的**新路线编号**；M0和早期M1归档中的“M3持久化/M4性能”等旧编号保留历史含义，不全局替换，也不把旧测试重复算成新阶段验收。每轮必须包含范围/非目标、源码提交、确定性测试、原始证据及限制；计划不是完成声明。

### M3：真实公开历史行情验证集

第四批已增加显式报告/时间profile及需本地使用声明的离线BinaryFILE检查入口，仍未取得真实验证集；见[M3第四批](M3_ITCH_REPORT_VALIDATION.md)。

同日第三批已覆盖22种历史wire消息的合成解码/可见簿/元数据保存，未知类型仍终止；这不替代授权真实验证集，见[M3第三批](M3_ITCH_METADATA.md)。

2026-11-22增量已加入原生目录路由/可见订单生命周期适配器，只使用自造ITCH帧验收，仍没有真实验证集或完整会话覆盖；见[M3第二批](M3_NATIVE_REBUILD.md)。

**2026-11-21方向更新**：主线改为Nasdaq官方原始ITCH 5.0，首个计划交易日2020-01-30；LOBSTER仅作许可允许时的独立参考。先用自行构造的帧实现BinaryFILE读取和核心字段解码，不等待授权才编写解析逻辑，但未确认使用条件前不下载/处理真实样本。本批范围和两条独立链路见[M3说明](M3_NASDAQ_ITCH.md)。下面小切片候选审查保留为11月20日历史记录，不代表已取得样本或完整原生接收端已完成。

先审数据，再写适配器。首轮只选择一个许可清晰的数据源及一个小型验证切片；不要求先拿到AAPL/TSLA/NVDA，更不以随机构造或合成CSV冒充真实订单流。

- **输入门禁**：记录提供方/原始URL、取得日期、许可文本与版本、再分发条件、原文件大小/SHA-256；无明确许可不得因“公开可下载”就把原始数据入库。需要用户协议或新账号/付费时先取得方向，不自动扩大授权。
- **语义门禁**：明确交易场所、品种/交易日、时区/时间戳单位与来源、价格刻度、序号域/文件顺序、订单引用作用域和事件种类。只有成交或L2快照的数据不得宣称能重建真实L3 Add/Cancel/Trade；不支持的消息列明拒绝/降级规则，不能静默补造缺失订单。
- **验收**：合成回归继续保留；真实集独立manifest与可复现取得方式，许可允许时保存最小样本，不能再分发时不提交原始字节；人工核对小型黄金事件序列、订单/成交计数和最终可见深度，并报告缺失/非法/未支持数量。未完成许可审查就停止接入，不臆造“真实数据已验证”。

#### M3首轮来源审查结果（2026-11-20）

状态：**已完成本轮候选资料核验，尚无数据集通过全部接入门禁，M3未完成。** 结构化审查结果见[候选记录](../data/sources/m3_candidate_review.json)。没有取得市场原始记录，故原文件SHA-256和本地fixture保持null；这不是已经下载或已授权数据的manifest。

| 官方来源 | 本轮查证 | 不接入的原因 |
|---|---|---|
| [LOBSTER样本](https://php.lobsterdata.com/info/DataSamples.php) | 提供小型股票message/orderbook文件 | [条款页](https://php.lobsterdata.com/termsAndConditions.php)未给出可据以确认样本再分发的授权；下载入口不等于可入公开仓库 |
| [Nasdaq原始ITCH样本说明](https://data.nasdaq.com/databases/NTV) | 官方FAQ给出ITCH样本目录 | 本轮未核清原始样本再分发条件，未选文件/下载/计算哈希；不能借解析器代码许可证授权数据 |
| [IEX历史数据](https://iextrading.com/trading/market-data/) | [历史条款](https://www.iex.io/legal/hist-data-terms)明确了使用要求 | 当前官方页面说明历史PCAP不可直接下载；其股票DEEP为聚合行情，不能替代逐单DEEP+验证 |
| [Hyperliquid作者发布数据集](https://zenodo.org/records/18184441) | [作者README](https://zenodo.org/records/18184441/preview/README.md?include_deleted=0)明确标注CC BY 4.0，列有真实订单生命周期、簿变更和成交 | 非股票而是永续合约；可见簿档案49.6GB，未列小型事件样本。一次有界Range请求在本环境得到403，未读取数据体或下载整包 |

还有两项不可隐去的语义门槛：LOBSTER规范含部分删除、隐藏成交、竞价/停牌等消息，现有整单Cancel/可见Trade模型不能直接覆盖；其受限档位文件也不能凭空恢复完整L3起始订单。Hyperliquid的[字段规范](https://zenodo.org/records/18184441/preview/SCHEMA.md?include_deleted=0)将状态、簿差分和成交分为不同流；簿差分本身无时间戳，remove不能单独区分取消与完全成交。未经真实小切片核验，不把remove统一伪造成Cancel、不用文件序号冒充交易所原生连续序号，也不从聚合快照编造订单。

用户已于2026-11-20确认保持股票范围：M3等待获授权股票样本及使用条件，Hyperliquid仅保留为未选用的研究候选，不接入加密永续。若使用已有获授权的ITCH/LOBSTER小样本，应一并提供取得方式、使用/再分发条件和字段说明；不要求公开密钥或账号。未为此创建账号、付费、联系提供方或下载多GB档案；尚未编写数据适配器，也没有真实订单回放通过声明。数据等待不阻止使用现有合成负载做明确标注局限的工程基准，但不把它计作真实数据或M5正式验收。

### M4：消费者恢复与重同步

先实现有界、可解释的恢复协议，再讨论真实网络。当前STALE和终止会话是正确安全边界，手动`restore_checkpoint`并不自动恢复已经终止的GatewayFeedSink/Publisher或FAILED的TradingEngine。

- **输入门禁**：可信快照必须绑定会话/品种、格式/配置、完整性与公开行情基准序号S；公开序号、CommandSequence、Journal sequence不能混用。先约定快照与增量来自同一来源、同一会话及一致截点。
- **恢复契约**：恢复中不暴露HEALTHY盘口；校验快照后只应用S之后的连续增量，明确重复、乱序、丢失及重放窗口的处置，补齐至目标水位才切回HEALTHY。缓存/等待窗口有上限，快照不可信、补齐失败、超时/队列溢出或恢复中再次故障回到STALE，不靠清除失败标志继续交易。
- **验收**：丢失12后收到13、重复/乱序、错会话快照、损坏/截断、恢复窗口溢出、消费者重启和进程强杀等分别给出故障点、来源水位、恢复/人工介入结果及逐状态对照；需要恢复的sink/decoder/gateway/发布会话必须形成完整接线，而不是只测checkpoint方法。行情簿重同步与权威撮合/Journaling崩溃恢复分开验收；没有durable prefix时不得承诺重启后零丢失，已APPLIED命令不能当新单重投。

### M5：新公开链路性能证据

分别测量核心、投影、编码、队列、解码、本地簿和完整消费闭环；至少输出吞吐、P50/P99/P99.9/最大延迟、分配次数/字节、队列峰值及失败计数。同一输入、环境和计时边界保留多进程原始结果，公开消息数与命令数分列；详细门禁见[BENCHMARK](BENCHMARK.md)。现有full_v2、CI smoke和旧纳秒微基准不能充当新路径证据。

M3/M4未通过时的实验只标为局部冒烟；不提前认定unordered_map/deque必然是瓶颈，也不先承诺优化收益。按品种分片、跨线程交接和无锁队列不在本阶段默认范围；先用证据决定是否需要，仍保留单簿单写者。

### M6：上游风险控制与可观测性

保持风险控制在交易接入/券商或策略执行侧，不塞进交易所撮合核心。先明确哪些入口必须经过风险层、哪些测试/回放入口允许绕过；再定义订单/执行标识、成交到持仓/资金的幂等更新和回放对账，不能把外部注入position当成已完成账户账本。

可观测性按品种/会话暴露缺口/STALE/恢复进度、队列积压/峰值/拒收、Journal健康及已接受/已写入水位；未实现的durable水位不可伪造。补运行时直方图前先定义延迟起止点和样本归属，测量锁/采样自身成本，并测试重复成交、重放、状态失效及统计一致性。账户风险和撮合业务规则保持分层；TSan子集通过不等于撮合已可并行提交。

此前M2收口与M3候选审查只同步文档/来源信息，没有新增真实数据源、恢复服务、账户系统或并行撮合。用户选择等待股票授权样本后，同日后续另补3个合成公开行情工程基准及测试，详见[BENCHMARK](BENCHMARK.md)；它不通过M3真实数据或M5正式验收，也不带来故障恢复能力声明。
