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

基线仍为`c690dfd8b008e06690e914e272173a7f4e55d164`加未提交工作区增量；[累计源码patch](validation/2026-11-19-m2-public-feed/source_changes.patch)与[47项Git blob清单](validation/2026-11-19-m2-public-feed/source_git_blobs.txt)冻结构建/源码/相关脚本，不包括不断增长的说明文档或任意checkout换行字节。补丁针对基线，不要重复应用到当前工作区；独立index重建47/47匹配、真实暂存未变。没有提交或推送。

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

没有对新发布路径做受控吞吐/P95/P99验收；原full_v2 Benchmark不包含这条公开路径，不能拿旧延迟当新链路结果。Linux/UBSan/TSan、真实磁盘满/强杀/断电、durable prefix、快照重同步及生产账户系统仍未整体验证。M1针对性修复保留，但不能据此关闭整个M1/M3/M4或全部升级大纲。

回滚只撤本批可选发布接线、encoder/consumer保护及对应测试/文档；保留此前十批成果。不要把累计补丁直接反向应用到已有用户修改的工作区。
