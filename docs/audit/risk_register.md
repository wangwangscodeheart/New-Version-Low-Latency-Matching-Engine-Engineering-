# M0：风险登记与第一批变更范围

M2进展（2026-11-19）：新增Journal整组接受后的公开批次接线、公共引用映射、严格完整批次适配和独立本地簿验证；补消费者非法方向/数量溢出/分配异常屏障，见[M2闭环](../M2_PUBLIC_FEED.md)。队列/消费失败后STALE与停止准入，不代表撮合回滚、durable prefix或真实网络重同步；下表保留历史审计事实。

第十批进展（2026-11-18）：独立补Audit冷导出错误报告，已复现并修复坏流正常返回，新增CSV黄金/12种写失败/4种flush失败等回归，见[M1第十批](../M1_BATCH_10.md)。它与S04中第九批AsyncLogger修复分开；不扩大为原子导出、真实close故障、容量预算或崩溃恢复保证。下表继续保留M0事实。

第九批进展（2026-11-17）：S04中AsyncLogger坏流仍written、错误不可见的路径已实际复现并修复，新增健康/failed指标及Demo/Benchmark收尾检查，见[M1第九批](../M1_BATCH_09.md)。Logger仍为可选诊断输出，不替代恢复Journal或生命周期Audit；Audit导出、真实磁盘故障、全部worker错误注入和并发关闭仍未整体验收。下表保留M0时S04事实。

第八批进展（2026-11-14）：C03已实际复现“旧manifest字节未变、但同代Book被覆盖且恢复状态改变”，并通过整代路径预检/保留式占位拒绝generation复用，见[M1第八批](../M1_BATCH_08.md)。新代提交前失败保留旧代的定点测试通过，但提交后刷新错误、durable prefix、真实强杀/断电和GC/磁盘预算仍开放，不把该修复扩大为全部恢复保证。下表保留M0时C03候选口径。

第七批进展（2026-11-14）：C01已通过直接池API用例复现重复/跨池归还的空闲表污染，并增加订单池预分配状态/归属检查、不可拷贝及活动槽位移动验证，见[M1第七批](../M1_BATCH_07.md)。不据此声称正常撮合曾发生double-free/UAF；裸指针代际、挂链对象误归还和其他池复制规则仍不在完整保证内。24/24本机测试及差分通过不关闭这些边界，也不证明性能无退化。下表保留M0口径。

第六批进展（2026-11-14）：C05危险表达式已移除，模拟风控的价格偏离/仓位使用精确整数，跨档累计量溢出时锁定不可用。已实际复现并修复MSVC仓位舍入漏判，33,936条小范围oracle及极值/集成回归通过，见[M1第六批](../M1_BATCH_06.md)。金额仍为浮点，未执行UBSan，不将其扩大为全部风控/全域UB验收。下表保留M0时C05事实。

第五批进展（2026-11-14）：C02的新价格map节点bad_alloc已实际复现孤儿索引，并完成未挂簿主动单结构清理、部分成交保留及FAILED边界验证，见[M1第五批](../M1_BATCH_05.md)。这不是回滚/全部分配异常强保证；池误用、极值风控、durable prefix及公开行情闭环仍开放。下表保留M0冻结事实，C02“未做注入”仅描述当时状态。

第四批进展（2026-11-12）：S10核心直接入口非法Side/TIF已修复，独立参考及760条穷举回归通过，见[M1第四批](../M1_BATCH_04.md)。分配失败、持久化、完整公开行情链路等剩余风险不因该修复关闭；下表继续保留M0冻结事实。

最新进展（2026-11-12第三批）：R05标准接线的进程内整命令预算/接受与v2回放前置验证已在[M1第三批](../M1_BATCH_03.md)落实，兼容observer失败也不再静默；持久化、legacy文件完整性和磁盘故障等风险仍开放。下文“待实现”保留M0/早期批次历史口径。

最新状态（2026-11-12）：R03的OrderBook/Sink拒收后保留续送及异常停止已在[M1第二批](../M1_BATCH_02.md)实现，R05异步Journal整命令提交仍开放；不能把进程内outbox等同崩溃恢复。以下M0表格保留旧基线事实。

后续状态（2026-11-11）：R01/R02/R04已在[M1第一批](../M1_BATCH_01.md)增量修复并验证，R03/R05仍开放。下表保留`c690dfd`的审计事实；不要用修复前probe作为新版本正确性测试。C07头依赖漏追踪已被实际观察，本批通过新目录和UTF-8 console code page规避，详见交付记录。

基线 `c690dfd`；2026-11-11。“已复现”只指 [诊断探针](evidence/core_edge_probe.cpp) 或已有测试实际观察到的行为；“静态缺口”是接口/代码事实，不伪称已经发生生产事故；“候选”必须继续验证。P0表示进入相应升级承诺前必须解决，不表示已有系统在普通输入下一律不可用。

## 1. 已复现 / 现有测试覆盖的缺陷或契约缺口

| ID / 优先级 | 事实、证据与影响 | 最小增量方向 / 验收 |
|---|---|---|
| R01 / P0 M1 | 同档 `UINT64_MAX+1` 回绕，合法新单APPLIED但簿不变量失败；`order.hpp:143`、`instrument_config.hpp:16`；probe R01 | 加入剩余挂单累积量的安全准入与明确拒绝语义；先无副作用拒绝，参考模型也独立校验；同价/跨档/部分成交后剩余量边界回归 |
| R02 / P1 M1 | 满池且Post-Only会成交时，核心与参考拒绝原因不一致；`orderbook.hpp:276` 对比 `reference_engine.hpp:109`；probe R02 | 先定义拒绝规则优先级，仅修对应分支/参考模型；黄金案例、差分重新通过 |
| R03 / P0 M1 | Sink容量1但产生2事件，状态已成交、只交付前缀，批次随即清空；`orderbook.hpp:168`、`event_system_tests.cpp:166`；probe R03 | 采用完整命令输出契约，不接受“标志false就足够”；输出预留或有界待发批次+阻止后续输入，并定义恢复；故障后不能继续把下游标为可信 |
| R04 / P1 M1 | 已注册symbol的质量告警进入fallback，在snapshot被预分配槽覆盖；全局1、AAPL0；`monitor.hpp:71`、`:112`；probe R04 | 统一映射或合并指标，测试订单/质量混合与未知symbol；不得简单丢弃fallback |
| R05 / P0 M1 | ORDER回调中gate失效，当前单仍撮合，下一单才拒绝；`trading_engine.hpp:44`、`:52`、`:72`；probe R05 | 定义Journal接受与本条命令提交关系；现有gate只保障后续fail-stop，不解释为事务或持久化 |

R03/R05在旧文档的收窄语义下不算“隐藏实现违规”，但不满足本轮可靠交易基础设施目标。R01/R02/R04是独立边界/指标缺陷。修复后 probe 不再以旧观察退出成功，应把新断言纳入正式测试。

## 2. 已确认静态缺口

| ID / 优先级 | 源码事实与边界 | 下一步 |
|---|---|---|
| S01 / P0 M1–M3 | AsyncJournal written/accepted不是durable；CSV无命令commit frame、CRC/长度/版本；只orderly-stop flush；`async_journal.hpp:20`、`:35`，`journal.hpp:47` | ADR定义完整命令/写入/持久化水位与尾部损坏规则；不能声称WAL/Exactly-Once |
| S02 / P0 M2 | 无权威簿→公开编码/发布→独立本地簿链路；Feed测试手拼wire，`feed_gateway_tests.cpp:13` | 先定L3-like可见性、私有/公开隔离，再补闭环一致性测试 |
| S03 / P1 M1 | Dispatcher void回调，异常会打断后续订阅者；Sink无条件true；`event_dispatcher.hpp:124`、`trading_engine.hpp:64` | 订阅失败、重入与不可用状态契约；用确定性抛异常注入测试 |
| S04 / P1 M3 | AsyncLogger 写失败仍递增written且无healthy反馈；`async_logger.hpp:83` | 区分尝试/成功/失败，磁盘写失败回归；Audit降级可见 |
| S05 / P1 M1 | V2 demo没把AsyncJournal接为gate，只结束时查healthy；`v2_main.cpp:25`、`:54` | 新契约落地后修演示接线，避免演示与恢复承诺脱节 |
| S06 / P1 M2–M3 | Feed首序号任意接受；STALE后无重传窗口/外部快照服务；`sequence_tracker.hpp:15`、`market_data_gateway.hpp:37` | 首序号基线、快照版本/增量衔接和恢复状态机 |
| S07 / P1 M1–M4 | V2事件vector可扩容、map新层分配；decoder pending/本地簿/tracker map无显式总上限；`orderbook.hpp:210`、`price_level_store.hpp:37`、`feed_decoder.hpp:48` | 定义资源预算与失败行为，先注入验证再优化；不称全热路径零分配 |
| S08 / P1 M4 | benchmark默认隐藏RESULT_CSV，采集脚本不强制且不校验行数；`v2_perf.cpp:378`、`run_benchmark_windows.ps1:40` | 强制机器输出并验证7场景×进程数；当前手动环境变量已绕过，不修改旧证据 |
| S09 / P2 M4 | `Process Runs`实际是同进程批次；checksum不是全状态/日志健康校验，drain不计时；`v2_perf.cpp:244`、`:394` | 修标签、记录测量边界、分离durable latency与submit latency |
| S10 / P1 M1 | 核心直接C++入口未完整拒绝非法Side/TIF枚举；`orderbook.hpp:237` | 明确API契约并增加显式拒绝，解析器校验不替代内部边界 |
| S11 / P2 M3 | Risk是可选包装器，Strategy直连TradingEngine；仓位/参考价依赖外部注入；`strategy.hpp:30`、`risk_checked_trading_engine.hpp:25` | 明确模拟账户模型和入口范围；不把它宣传为核心交易所规则/完整账户系统 |

## 3. 尚未复现的候选风险

| ID | 候选与依据 | 不能先声称什么 / 验证方式 |
|---|---|---|
| C01 | ObjectPool::deallocate没有归属/重复释放检查，`order.hpp:98` | 未发现正常引擎double-free/UAF；取消/成交已按摘链→删索引→归池审查，ASan全绿；用误用API/槽位复用回归评估硬化 |
| C02 | map或事件vector bad_alloc可能发生在已部分改变状态之后 | 未做分配失败注入，不断言已经损坏；定点fail allocator与不变量/输出检查 |
| C03 | 多簿Snapshot使用相同snapshot_sequence可能覆盖旧manifest引用的同代文件，`market_snapshot.hpp:110` | 未做保存中断注入；重复generation+中断写后验证旧快照仍可加载 |
| C04 | CSV只有输入而无完整outcome的合法前缀可能被当成可回放，`replay_v2.hpp:122`；宽容尾行缺乏checksum | 未定义durable边界前不能称恢复了已提交前缀；逐字节截断/翻位/完整行篡改矩阵 |
| C05 | Risk `llabs(INT64_MIN)`和先int64相减再转long double的极值，`risk_engine.hpp:72`、`:198` | 普通风控测试不证明全域安全；先安全回归与UBSan验证，不执行UB冒充有效结果 |
| C06 | Dispatcher subscribe/reset/stop与producer并发、回调重入提交未定义 | 不因BoundedQueue有mutex就声称应用层并发安全；约束生命周期并测试重入/竞争 |
| C07 | 中文MSVC showIncludes前缀探测异常，编译输出未被Ninja过滤 | 本次干净构建成功，但未证明增量头文件依赖追踪正确；头文件变更触发重编译验证后再调整环境 |

## 4. 本轮未证明的目标

无 kill -9/断电/磁盘满/慢消费者整链路恢复证据；无真实行情许可和数据manifest；无网络行情发布；无NUMA/跨分片架构；无本轮Linux/UBSan/TSan结果。`.github/workflows/ci.yml`存在不等于runner通过。原项目的诚实限制大多仍有效。

## 5. 下一批变更与回滚策略

1. M1首批：将R01/R02/R04变为独立回归，增量修`orderbook/order/reference_engine/monitor`必要分支；明确非法枚举与拒单优先级，不换池/索引/撮合算法。
2. M1第二批：按[ADR-001/002](decisions.md)实现命令级输出/Journal接受契约，改动`event_sink/process_status/trading_engine/async_journal`及对应测试；故障命令不能未经定义继续撮合。
3. M2才新增公开事件Builder/Publisher和权威/本地簿对照；M3才对外承诺具体durable prefix。

每批先回归、再有限生产diff、全量Release/ASan、同口径基准，单独提交便于revert；不得把20/20当作上表所有风险已消除。M0仅添加证据，无生产性能/行为变化；本轮回滚不涉及核心文件。
