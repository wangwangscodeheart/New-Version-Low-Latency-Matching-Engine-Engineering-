# M3第二批：原生ITCH目录路由与可见订单簿重建

后续状态：同日[第三批](M3_ITCH_METADATA.md)补元数据，[第四批](M3_ITCH_REPORT_VALIDATION.md)补可选报告/时间/系统阶段检查及离线入口。本文保留第二批范围和33项证据，不是当前未完成项清单；真实验证集仍未取得。

日期：2026-11-22（Asia/Tokyo）。沿用[第一批读取器/解码器](M3_NASDAQ_ITCH.md)，不重写项目，不修改V1撮合算法、MarketManager、TradingEngine或M2模拟公开发布器。

## 结果与真实数据边界

本批接通了**自造BinaryFILE → 13种typed ITCH消息 → 日内目录路由/归一化 → 每股票独立MarketDataBook**。这是接收端的可见状态重建，不是原始订单入场重放；没有把ITCH Add送入MatchingEngine。

全部测试字节由测试代码自行生成，原Nasdaq 2020-01-30档案仍未下载，原文件SHA-256为空；研究、再分发、派生输出和云端AI处理权限未因实现适配器而解决。来源记录见[manifest审查状态](../data/sources/m3_candidate_review.json)，不是已经取得数据的完整性证明。

```text
BinaryFILE Reader → Itch50Decoder → ItchBookRebuilder（单所有者）
                                      ├─ R：日内Locate ↔ Symbol
                                      ├─ 全局Active/Seen公开订单引用
                                      ├─ A/F/E/C/X/D/U归一化
                                      └─ 每股票独立MarketDataBook → 订单/深度/BBO

既有Command → Matching Engine → PublicFeedPublisher → 模拟本地簿
仍是另一条独立验证链路，不与上面共享订单状态或伪造sequence。
```

## 修改、原因与真实组件对应

| 文件/模块 | 改了什么、为什么 | 真实组件 / 面试如何介绍 |
|---|---|---|
| `src/market_data_book.hpp` | 增加显式部分减量、身份/总量/价格替换、只读查单、BBO和冷不变量检查；旧Feed apply行为不改 | 消费端可见簿，不是私有撮合簿；业务扣量不需要伪造成Trade |
| `src/itch_book_rebuilder.hpp` | 组合已有Reader/Decoder，维护目录、引用作用域、归一化与终止失效状态 | Feed Handler协调层；区分协议字段、业务状态与文件结束 |
| 三个新增测试目标 | 普通簿边界、原生端到端合成生命周期、独立分配故障扫描 | 黄金状态与失败注入约束实现，不拿撮合核心当外部真实行情oracle |
| 来源清单、README、限制/验证文档 | 更新已实现和未完成边界，保留历史批次证据 | 数据治理与可核查的阶段验收 |

路由/Normalizer以适配层内部函数实现，没有为框图增加无职责的框架类。MarketDataBook不接触文件会话，也不知道Tracking Number；适配器不接触撮合OrderBook。

## 路由与订单生命周期

R建立非零日内Stock Locate与非空Symbol的双向映射。允许标点；拒绝NUL/内嵌空格和不可见字符、同股票不同Locate、同Locate不同股票或不同元数据。完全相同的R可以重复。A/F/H/P/Q的股票文本必须与目录一致；E/C/X/D/U必须属于其Locate下仍活动的公开引用。不是拿symbol字符串或旧InstrumentId注册顺序猜测交易所Locate。

| 消息 | 本批应用 |
|---|---|
| A/F | 新建显示订单；F保留MPID，方向和数量独立校验 |
| E | 按被执行引用的挂单价减量，归零删除 |
| C | 与E同样扣可见量；执行价可以不同，printable=N也扣量，不挪价格档 |
| X | 减少指定数量，不把部分撤销变成整单取消或成交 |
| D | 删除整笔剩余量 |
| U | 新引用、新价格、**新总剩余量**；继承股票、方向与MPID，不是增量量或新的撮合入口 |
| H | 保存H/P/Q/T交易状态，不自动清空簿，也不因停牌/仅报价而丢弃簿更新 |
| P/Q/B | 校验部分历史profile字段后分类型计数；不创建、扣减或恢复显示订单 |

参考[官方2015字段规范](https://www.nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/NQTVITCHSpecification_5.0.pdf)：U继承侧/股票/MPID；Q可以是零成交量通知，故接受零量及其零价格/Match，不伪造成成交。Q支持旧O/C/H/I；B属于具体股票Locate；目标历史P使用引用0和方向B，不能用它推断主动方向。

Decoder仍保留全宽原始整数；适配层对实际显示订单、替换与非零执行价执行正值及历史Price(4)上限2,000,000,000 raw（200,000.0000）校验。这不是对任意年份ITCH定义的泛化。

P/Q/B目前**不是完整成交流、去重账本、总成交量或Broken修正统计**。未检查Match跨消息唯一性、已报成交是否存在或B是否重复；不能用这些计数宣称完成成交对账。H也不是完整交易时段状态机。

## 日内身份、容量和异常

Order Reference是全市场日内身份，不按股票重新起一个空间，也不假设数值单调。适配器保留Active引用的所属Locate/MPID，以及所有出现过的Add/Replace引用的Seen集合；订单删光后旧引用也不能重用。没有将P的0引用放入这些索引。

默认上限：65,535个目录项、1,000,000个活动单、10,000,000个Seen引用，均可配置；上限0和非法目录上限在构造时拒绝。取消只释放活动计数，不清除日内Seen；达到上限终止，不驱逐身份历史后继续“验证”。这些是逻辑数量上限，不是总堆字节预算、预分配保证或对整个真实交易日内存容量足够的承诺。实现仍含map/unordered容器和动态分配。

MarketDataBook的替换先检查净档位数量溢出，再暂存新价格档/订单节点，全部成功后才删旧单；同价按`aggregate - old + new_total`计算，避免把合法替换误判溢出。新节点可能使unordered_map rehash，不能继续使用旧iterator。分配失败撤销新建空档，原订单/数量保持不变。适配器同时暂存并回滚Seen/Active节点，避免簿与全局索引失配；R反向映射失败也回滚。

接收/解析/应用过程的任意错误（含未知类型、容量、IO、回调重入、分配异常）使整个会话进入STALE，后续输入拒绝，错误计数只增加一次，`book()`返回空。没有自动重试/重同步、清除错误标志、快照恢复或跨文件续接。前序记录不回滚；失败的新事件是否保留诊断状态不能代替健康声明。

只读指针仍是借用视图：STALE不能撤销之前取得的C++指针。调用方必须检查会话状态，不得缓存旧指针/BBO当作故障后可信行情；只允许单所有者操作，不支持并发提交、移动/复制、回调重入。`check_invariants()`是可能分配的冷诊断，不代表实时测量成本或可交易状态。

## 会话结束不是网络恢复

```text
AWAITING_START --S/O--> REBUILDING --S/C + BinaryFILE终止 + finish--> COMPLETE
       任意接收/解析/应用故障 -------------------------------------> STALE
```

S/O必须首先出现，系统码按O/S/Q/M/E/C向前推进；此阶段允许省略中间系统码，所以不是完整交易日时间表验证。S/E不是结束：仍允许后续D/B。只在S/C后拒绝任何后续业务载荷。没有完整验证S/E后所有消息的营业时间合法性，也没有使用时间戳决定撮合或排序。

`read(istream)`检查物理EOF、结束标记、尾随垃圾和流错误；`consume()`支持任意分片，调用方必须提交完整输入后调用`finish()`，不能从“尚未提交尾部”推断文件完整。finish成功后可顺序重复；新输入或错误后不能复活会话。自定义streambuf吞掉重入异常也不能把STALE改成COMPLETE。

`COMPLETE`仅表示**声明的13类型profile已按所提交输入完成**，不是全ITCH支持、会话真实性、数据无遗漏或可上线交易的证明。9种已知但未实现的元数据消息仍会终止重建；不提供跳过后维持可信簿的开关。局部文件ordinal仅来自Reader，不能检测文件中被完整删除的消息，更不是ITCH Tracking Number或MoldUDP64序号。相同/倒退时间戳和Tracking变化不会被伪报成网络丢包。

## 使用与验收

调用方取得二进制输入流后：

```cpp
nasdaq_itch::ItchBookRebuilder session;
session.read(input); // 实际文件须先核清使用权限；没有自动下载或gzip支持。
if (session.state() == nasdaq_itch::RebuildState::COMPLETE) {
    if (const auto code = session.locate("AAPL")) {
        const auto* book = session.book(*code);
        // book->best_bid(), best_ask(), capture_state(); prefix期间只代表重建进度。
        (void)book;
    }
}
```

复现（VS x64 Developer cmd）：

```bat
chcp 65001
cmake -S . -B build/itch-rebuild-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/itch-rebuild-release --parallel 6
ctest --test-dir build/itch-rebuild-release --output-on-failure
cmake -S . -B build/itch-rebuild-asan -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DENABLE_ASAN=ON -DENABLE_UBSAN=OFF
cmake --build build/itch-rebuild-asan --parallel 6
ctest --test-dir build/itch-rebuild-asan --output-on-failure
```

新增普通簿7组和原生重建9组，覆盖逐事件订单/深度/BBO、跨股票隔离、引用/目录冲突、数量/价格边界、MPID继承、零量Cross、容量、各种分片、结束与错误失效。独立故障程序扫描原簿20组与R/A/U会话3组，在本机捕获38+15次分配失败；扩容边界由运行时探索，不写死到其他STL。它替换new/delete，仅链接自身，有ASan分配族诊断局限；不能把它等同于穷尽全部分配点。

结果与源码身份以[验证记录](VALIDATION.md)和[原始输出](validation/2026-11-22-itch-rebuild/)为准；没有新增性能或真实数据结论。

面试可介绍：**我保留撮合核心，把交易指令和公开行情接收分开；原生接收端按日内目录及公开引用重建独立显示簿，用合成生命周期与失败注入验证应用边界，并明确已支持profile、完成状态和授权数据缺口。**

## 下一步（尚未完成）

- 补其余历史元数据/交易状态规则、完整session profile，以及成交报告与Broken账本的明确策略，不能跳过消息就宣称全ITCH。
- 授权确认后取得/校验gzip、固定历史协议副本，再用真实第一/第二交易日及许可允许的独立参考验证；仍不提交原始或可逆数据。
- 分开测IO/解压、解码、簿更新和完整历史回放；旧撮合微基准不能作为本链路性能。
- 网络封装、双路、模拟丢失/重传/快照重同步另行实施，不把文件错误或删帧测试叫历史真实网络故障。
