# 当前测试体系（V2）

2026-11-23第六批：仅改八份旧测试夹具与CI，新增原子预留/登记清理辅助类，CTest仍43项。本机三配置在不分离每配置TEMP时并行复验均43/43；八程序`/W4 /WX`通过；三配置×八程序×两份共48个独立进程共享同一个专用临时根，全部exit0且无夹具残留。首次直接启动ASan因未进入MSVC环境有16个进程报缺DLL（未执行测试）；原输出保留，修正启动环境后48/48，不改源码规避。该证据不是并行撮合或性能测试。`05f003b`实际Linux默认OFF已取得43/43、ASan+UBSan43/43、TSan子集4/4；新增ON/OFF矩阵按后续提交分别记录，见[第六批](M3_CI_AND_TEST_ISOLATION.md)。

2026-11-23第五批：新增SHA、可选gzip和单遍输入协调器三个CTest，保留原40项并扩展CLI测试。最终默认无zlib Release、启用zlib Release、启用zlib ASan均43/43；新三个测试及CLI以`/W4 /WX`独立编译执行通过（gzip相关分别ON/OFF）。默认构建只执行禁用拒绝测试，不能冒称CRC测试通过。覆盖原始hash域、独立向量、拼接/可选头/截断/CRC/ISIZE、限额、晚到IO/吞异常重入、预先消费前缀不能冒充fresh、输出失败；七种实际CLI退出已验证，输入全为自造。两次并行配置复验中旧固定临时文件冲突的失败原始输出保留；最终为各配置隔离TEMP/TMP后结果。zlib C静态库未插桩，两项new/delete替换程序原有限制保留，其余41项无该替换；没有真实文件、新Linux/UBSan/TSan或性能验收，见[第五批说明](M3_ITCH_INPUT_INTEGRITY.md)。

2026-11-22第四批：新增报告ledger、日期/时间、严格会话、离线入口四个测试程序及CLI help，最终MSVC Release/ASan均40/40。原35项继续保留；默认兼容与显式检查分开验证，CLI默认严格关联/日内域/六阶段且要求本地使用声明。新时间5组、ledger6组、关联7组、CLI2组覆盖时间/身份/不可逆B、界限/溢出、分片、权限声明前置、gzip拒绝及输出write/flush/异常；中文合成路径实际exe验证通过。独立程序现25组扫描56次失败，含报告节点失败不扣量及合法B无分配；两个替换new/delete的程序ASan限制保留，其余38个CTest不带替换。无真实源/新Linux/性能验收，见[第四批说明](M3_ITCH_REPORT_VALIDATION.md)。

2026-11-22第三批：新增`matching_engine_itch50_metadata_decoder`和`matching_engine_itch_metadata_session`，最终MSVC Release/ASan均35/35。新decoder四组、元数据会话七组合成测试覆盖22种历史类型混合流、K/V/W特殊路由、h按场所保存、零/全宽值、MPID容量与未知/非法消息失效；更新原核心测试，不删除先前订单生命周期测试。定向L节点失败/已有MPID无分配更新通过；当前独立故障程序共24组扫描55次注入，R构造含新增map后其分配点随STL实现变化，不沿用上轮53次作为当前计数。两项带new/delete替换程序的ASan局限保留，其他33个CTest不带替换。没有真实行情或新Linux/性能证据，见[第三批范围](M3_ITCH_METADATA.md)与[验证记录](VALIDATION.md)。

2026-11-22：M3第二批新增原生簿、ITCH重建、独立分配故障三个CTest；MSVC Release/ASan最终均33/33，三份新增测试均以`/W4 /WX`独立编译运行通过。合成数据覆盖跨品种目录/引用、A/F/E/C/X/D/U、MPID、H状态、零量Cross、结束/失效；故障程序23组扫描捕获53次bad_alloc并检查回滚/STALE。当前共有两个带new/delete替换的独立故障程序，其ASan分配族覆盖局限须保留，其他31个CTest不带这些替换。没有本批Linux/UBSan/TSan、真实行情或性能结论，见[第二批范围](M3_NATIVE_REBUILD.md)和[验证记录](VALIDATION.md)。下面30/28项均为历史批次。

2026-11-21：M3首批新增`matching_engine_itch_binary_reader`和`matching_engine_itch50_decoder`，最终MSVC Release/ASan均30/30；另以`/W4 /WX`独立重编译并运行两项新测试通过。覆盖13种typed消息、9种长度识别、精确短缓冲区、逐切点/逐字节、流EOF/异常和重入失效；所有字节为自造。没有真实ITCH回放、原生簿应用、Linux/UBSan/TSan新结果或性能结论，见[M3范围](M3_NASDAQ_ITCH.md)和[本轮验证](VALIDATION.md)。下面28项为11月20日历史批次，不覆盖本批新增代码。

后续合成公开行情Benchmark增量的本机Release/ASan均28/28通过；新增`matching_engine_public_feed_benchmark`测试混合成交/挂撤负载、公开消息数/字节/队列高水位、逐批可见状态、采样/批量checksum一致性及生命周期/溢出参数，不断言机器相关耗时阈值。源码`src/`未改；新目标尚未取得Linux runner复验，不能把下面`b889136`的27项CI证据扩大成28项，见[本轮验证](VALIDATION.md)。

2026-11-20补充：`b889136`已在真实GitHub runner通过Linux Release 27/27、ASan+UBSan 27/27、异步组件TSan子集4/4及benchmark smoke，手动差分压力任务未触发，见[VALIDATION](VALIDATION.md)。下文“本机未执行Linux”保留为此前Windows本机/历史口径，不表示没有这次runner证据；不据此宣称多线程撮合、故障重同步或新公开链路性能已验收。

2026-11-19最新验证为M2公开行情闭环：新增encoder、committed_handoff、public_feed三个CTest，最终MSVC Release/ASan均27/27，额外100万命令差分通过；消费者分配扫描50次失败注入通过，见[M2记录](M2_PUBLIC_FEED.md)及[原始证据](validation/2026-11-19-m2-public-feed/)。独立allocation_failure程序带new/delete替换的ASan限制继续保留，其余26个程序不受该替换影响。下文24/24是11月18日历史口径。

- Public Feed：手写大端黄金、137字节帧串的138个切点、非法输出、提交后完整批次、多品种投影、公开引用、部分/全部成交/IOC/Post-Only/撤单、队列满及消费异常失效。
- Consumer Allocation：两侧新/已有档位Add回滚、sequence observe/apply异常后STALE、检查点全部分配点的事务恢复；不声称已穷尽Publisher及全系统分配故障。

2026-11-18最新验证补Audit冷路径导出错误报告，Journal/Audit/Replay target由5组增至10组；MSVC Release/ASan仍各24/24通过，enum_boundary_v2生成器100万命令差分通过，详见[M1第十批与原始记录](M1_BATCH_10.md)。前批Logger、快照、池和风险回归保留。[第五批](M1_BATCH_05.md)引入的独立分配故障程序含new/delete替换，ASan分配族诊断覆盖有限；其余23个程序不受替换影响。历史结果保留原口径；Linux/UBSan/TSan本机未执行。

- Unit 与属性测试：交易规则、FIFO、撮合、撤单、容量和不变量。
- Reference Differential：默认25个固定种子×4,000条命令，enum_boundary_v2从原GTC分段中取部分输入注入非法Side/TIF；逐命令比较状态、事件、不变量，每25步及最终比较完整盘口。旧80%/10%/10%为此前未注入非法枚举的分布口径。
- Recovery：Journal、单品种磁盘 Snapshot、多品种一致性 Snapshot、损坏拒绝和增量 Replay。
- Snapshot Generation：同代拒绝、legacy最后路径碰撞、占位独占、manifest提交前失败保留旧代、失败代不复用；不等于强杀/断电验收。
- Infrastructure：事件订阅、异步 Journal/Logger、Monitor、风控、Feed Decoder、Sequence Gap、Trade 扣减和行情检查点。
- Logger Failure：门控短写/抛异常、flush/header失败、精确满队列、借用流/关闭职责、生产者复制bad_alloc、Monitor错误状态；不等于真实磁盘满或并发stop验收。
- Audit Export：原生命周期CSV黄金输出，12种写入/4种flush错误、open错误、重新导出及后续撤单；实际close失败未注入，不等于原子保存或持久化验收。
- Risk Boundaries：精确整数bps/仓位区间、MIN绝对值与敞口重算、同侧跨档累计量、状态失效及可信来源完整重绑。
- Allocation Failure：新价格节点定点bad_alloc、池/索引清理、部分成交保留与FAILED、Journal abort，以及预留事件批次/续送不分配。
- Order Pool：重复/跨池/非槽位归还、容量与LIFO、不可拷贝、带活动槽位移动，以及Book移动后成交/撤单/补满容量。
- CI 配置：Linux Release、ASan+UBSan、TSan 子集及 benchmark smoke；只有实际 runner 结果才算 CI 证据。

2026-08-07 已实际运行 MSVC ASan RelWithDebInfo 全量测试并通过 20/20。Linux Release、UBSan 与 TSan 因本机没有 Linux 发行版或容器工具而未执行，详见 [`VALIDATION.md`](VALIDATION.md)。

本地基线以根 README 中最近一次明确记录为准，不沿用 V1 历史文档中的 4/4 结果。
