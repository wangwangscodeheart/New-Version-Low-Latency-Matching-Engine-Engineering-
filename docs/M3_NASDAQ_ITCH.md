# M3：原始Nasdaq ITCH接收端（第一批）

后续状态：2026-11-22已完成合成数据驱动的[第二批目录路由/可见簿](M3_NATIVE_REBUILD.md)和[第三批历史元数据](M3_ITCH_METADATA.md)。本文保留11月21日第一批范围与证据，不作为最新未完成项清单；真实样本和M3整体验收仍未完成。

日期：2026-11-21，Asia/Tokyo。增量实现，不重写项目、不修改V1撮合核心。

## 范围与数据状态

主线选择Nasdaq TotalView-ITCH 5.0；首个**计划**验证日为2020-01-30，优先研究AAPL/MSFT。官方[历史目录](https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/)列有`01302020.NASDAQ_ITCH50.gz`，目录压缩大小5,597,158,940字节。这是数据选择，不是已下载、完整性已通过、品种覆盖已验证或许可已解决的声明。

本批只实现读取/解码，并使用完全自行生成的测试字节。没有下载该档案、引用真实行情片段、生成可逆衍生数据、联系提供方、创建账号或付费。来源审查状态见[结构化记录](../data/sources/m3_candidate_review.json)，原文件SHA-256保持null。

公开下载不自动授权个人研究、公开再分发、派生结果发布或云端AI处理。各用途适用条款仍待确认，代码MIT许可证不覆盖提供方数据。未获确认前不让开发工具读取真实记录。将来获授权后数据放受控本地目录；`.gitignore`忽略`data/local/`、`data/private/`和常见原始文件名，但ignore不是安全隔离或免审许可，推送前仍需检查暂存内容。禁止把真实字节放进测试黄金、错误消息或公开构建日志。

## 两条链路不能混用

```text
A：外部公开行情接收端（本批只完成前两格）
获授权历史文件 → BinaryFILE Reader → Itch50Decoder
                    [待实现] Stock Locate/Symbol Router
                        → Native Normalizer → 独立MarketDataBook
                        → BBO/深度/订单生命周期/独立参考验证

B：既有模拟撮合验证（保留）
确定性Command → V1 Matching Engine → EngineEvent
    → PublicFeedPublisher → 模拟FeedDecoder → 独立本地簿
    → 与撮合核心可见状态比较
```

ITCH是撮合后公开业务消息，不是全部原始入场指令，不含完整账户、隐藏委托或订单类型信息。本批不把ITCH Add转换成`NewOrderCommand`，也不宣称重演Nasdaq撮合。两条路径以后可以比较统计特征，但不可共享同一权威簿来“证明重建正确”。

## 这一批修改及设计原因

| 文件/模块 | 修改与原因 | 真实组件对应 / 面试介绍 |
|---|---|---|
| `src/itch_binary_reader.hpp` | 固定64KiB载荷存储，任意分片和有界istream读取；记录字节偏移及本地序号，严格区分完整/边界EOF/截断 | 历史feed framing；文件大不意味着全部读入内存 |
| `src/itch50_decoder.hpp` | 显式大端整数和定长自有文本、类型化variant；精确长度和支持范围检查，不反序列化C++对象布局 | Feed Handler协议层；结构解码与业务重建分层 |
| `tests/itch_binary_reader_tests.cpp`、`tests/itch50_decoder_tests.cpp`、CMake | 手工黄金字段、切点、流异常、长度和未知类型回归；两个独立CTest | 可复现协议测试，不拿真实记录或成对编码器充当唯一oracle |
| `.gitignore`、来源记录、文档 | 明确本地数据门禁、历史字段依据、未完成范围 | 数据治理、证据链和能力边界 |

接口独立位于`nasdaq_itch`命名空间，不改变旧`FeedMessage`、模拟帧头、MarketManager、OrderBook或单写者模型。

## BinaryFILE完整性契约

按官方[BinaryFILE v1.00](https://www.nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/binaryfile.pdf)：每条记录是2字节大端载荷长度，加指定长度载荷；长度包含ITCH类型字节、不含自身前缀。**零长度记录是会话终止标记**，不是非法空消息。默认`finish()`要求该标记；ITCH System Event的`C`业务结束事件不替代它。

- `consume()`同步交付`BinaryRecordView`，指针只在回调内有效；需要保留时调用方复制，解码器返回的字段自有存储。
- `read()`每次请求最多4096字节，读至物理EOF，终止标记后的任意字节也报错。
- 可配置载荷上限1..65535，超限在缓冲载荷前拒绝；不是全文件总大小上限。
- 默认缺终止标记、半前缀、半载荷、IO错误都失败；显式`ALLOW_BOUNDARY_EOF_ONLY`只允许完整记录边界，并返回`BOUNDARY_EOF_WITHOUT_TERMINATOR`，**仍然是不完整会话**。未来如果实际样本没有标记，必须报告该差异，不能悄悄改成完整。
- 解析/回调异常后reader终止失效，不从部分输出重试；错误只报告类型/位置，不dump载荷。此前成功回调不会被回滚，终止标记也不是业务应用事务。
- reader不可复制/移动，不允许并发调用或重入；回调及自定义streambuf重入都会使reader失效，即使调用方吞掉内层异常也不能复活。

`ordinal`是从1开始的文件记录计数；`offset`是从0开始的长度前缀字节偏移。ITCH的16位Tracking Number用于诊断，不是连续网络序号；48位时间戳是纳秒/午夜，不是Unix时间。文件会话日期/标识来自带外信息，不能从文件内推断时区偏移或当日夏令时。未来MoldUDP64包/消息序号必须另建类型，不套用既有模拟Gateway每品种sequence，也不把文件排序当成丢包证据。

## 历史布局及明确支持范围

依据官方[旧版5.0字段表](https://www.nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/NQTVITCHSpecification_5.0.pdf)（页脚03/06/2015），结合[主规范修订记录](https://www.nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/NQTVITCHSpecification.pdf)中2020年前的修订：2017增加J、2018增加小写h及版本号回归5.0。**没有取得独立冻结的2018版PDF**，也没有在2020原文件上验证覆盖；不使用2022杠杆语义更新、后续NOII扩展或2023直接上市新消息来解释旧数据。

本轮核对的是官方可检索字段表和修订记录。直接PDF抓取在本环境出现超时/503，未取得可归档并逐页视觉复核的完整历史PDF；真实样本验收前还需固定协议副本和覆盖清单，不把检索结果当作协议版本存档。

所有支持消息都有11字节公共头：type@0、Stock Locate:u16@1、Tracking Number:u16@3、timestamp:u48@5。以下长度均不含BinaryFILE前缀。

| wire类型 | 长度 | 类型化解码；尚未执行的业务语义 |
|---|---:|---|
| S / R / H | 12 / 39 / 25 | 系统事件 / 股票目录 / 交易状态；不自动建立路由或交易状态机 |
| A / F | 36 / 40 | 普通/带MPID公开Add；保留原始价格整数（刻度10000） |
| E / C | 31 / 36 | 执行量/Match号；C另有printable及执行价，不把执行价改成挂单价 |
| X / D | 23 / 19 | 部分取消数量 / 整单删除，不能统一成旧的全量Cancel |
| U | 35 | 原/新公开引用、新总量和新价格；没有symbol/side，未来从旧单继承 |
| P / Q / B | 44 / 40 / 19 | 非显示成交 / Cross / Broken Trade，不能伪造成可见单Add、扣减或恢复 |

共13个wire类型。其余9种已知历史消息仅识别长度：Y20、L26、V35、W12、K28、J35、h21、I50、N20；不解码其业务字段。没有ITCH5.0的T时间戳消息。

默认拒绝所有不支持类型；显式`REPORT_UNSUPPORTED`只返回类型/长度并继续，由调用方单独统计覆盖，**不表示全会话被完整解析或簿仍可信**。即使报告模式，已知历史类型长度不正确也必须拒绝。已支持类型长度必须精确匹配，不能吞尾字节来兼容新版本。方向B/S和C的Y/N有结构检查；数值零、全宽无符号数等保留原值，订单引用有效性、状态相关数量和跨消息一致性由下一层业务校验，不在解码器臆造。

P的历史方向字段不能当主动买卖方向；B不自动恢复可见数量；C即便不printable也会减少被执行挂单数量。这些目前只以不同payload表达，未对现有MarketDataBook实施变更。

## 验证与复现

用VS x64 Developer cmd（先设置UTF-8，确保Ninja依赖扫描正确）运行：

```bat
chcp 65001
cmake -S . -B build/itch-core-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/itch-core-release --parallel 6
ctest --test-dir build/itch-core-release --output-on-failure
cmake -S . -B build/itch-core-asan -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DENABLE_ASAN=ON -DENABLE_UBSAN=OFF
cmake --build build/itch-core-asan --parallel 6
ctest --test-dir build/itch-core-asan --output-on-failure
```

结果只以[验证记录](VALIDATION.md)及对应原始日志为准，不沿用旧27/28项证据扩大覆盖。本批没有真实市场数据或吞吐、延迟、gzip成本测量；旧纳秒级核心Benchmark不能代表原生接收端。

读取器与解码器的连接只需要在回调内解码，业务层以后单独接入：

```cpp
nasdaq_itch::BinaryFileReader reader;
// input: 调用方提供的二进制istream；真实数据需先核清使用权限。
const auto completion = reader.read(input, [&](const nasdaq_itch::BinaryRecordView& record) {
    auto message = nasdaq_itch::Itch50Decoder::decode(record.data, record.size);
    // message拥有字段；record是借用视图。这里尚不修改任何订单簿。
    (void)message;
});
// 默认只可能正常返回COMPLETE_WITH_TERMINATOR，否则抛错。
(void)completion;
```

## 下一批与验收门禁

1. 获确认后在受控环境取得档案：保存提供方校验值，独立计算本地SHA-256，验证gzip CRC/完整解压及BinaryFILE结束状态。MD5不是本地SHA-256的替代品，成功解压也不证明业务完整。
2. 增加目录路由和原生Normalizer/独立簿应用，覆盖A/F/E/C/X/D/U；过滤股票前仍解析目录，保持原文件顺序，不按时间戳重新排序。明确跨股票订单引用、替换身份、未知引用和交易状态规则，故障后簿失效。
3. P/Q/B及其他已知元数据有独立的成交/状态处理策略，不污染显示簿；未支持消息必须报告覆盖。禁止仅靠Add/Cancel通过就声称完整ITCH。
4. 对合成生命周期逐步黄金状态、BBO/深度不变量、分配失败/非法事件进行测试；真实集单独验证，与许可允许的LOBSTER或独立实现对照，再补第二交易日。
5. 独立测文件IO/解压、解码、簿更新及完整历史回放。网络层另行实现MoldUDP64、双路、重传/快照/重同步；人工删帧只叫模拟故障，不叫复现历史网络丢包。

当前M3未完成。上轮独立分配统计工具的在途文件保留，但未接入本批构建，也没有分配测量完成声明；不以未完成工具替代上述验收。
