# 当前验证记录

## 2026-11-19：M2提交后公开行情闭环

新增可选四参数协调器：Journal整命令接受后才向PublicFeedPublisher移交完整事件；公共引用投影、固定大端编码、有界整批wire队列、严格Gateway适配及独立本地簿已接到原V2 Demo。Consumer补非法Side/档位溢出/分配异常保护及检查点事务替换。未重写V1核心。

两套全新MSVC Release/ASan构建均60/60成功，初测均27/27（9.50s / 14.91s）；随后修正新测试的临时文件隔离、加强全量可见订单比较，并补Threads链接，重新构建受影响目标，最终同为27/27（1.71s / 8.94s）。最终证据是重建后的结果，不把初测源码冒充最终源码。原有D9025、Benchmark C4244及ASan故障Sink C4702警告保留。

额外100种子×10,000条命令差分通过。Consumer七轮分配点扫描覆盖50次bad_alloc和7个成功边界；编码黄金/37条非法组合/138切点、提交边界11组和公开链路5组通过。完整命令公开消费、拒收/异常失效及可见订单/深度比较不等于FIFO恢复或网络Exactly-Once。独立分配故障可执行文件替换new/delete，其ASan覆盖仍有既有限制，其余26个CTest程序不受该替换影响。

详见[M2设计与边界](M2_PUBLIC_FEED.md)。原始[初次Release](validation/2026-11-19-m2-public-feed/release-build-and-ctest.txt)、[初次ASan](validation/2026-11-19-m2-public-feed/asan-build-and-ctest.txt)、[最终Release](validation/2026-11-19-m2-public-feed/release-final-retest.txt)、[最终ASan](validation/2026-11-19-m2-public-feed/asan-final-retest.txt)、[定点及差分](validation/2026-11-19-m2-public-feed/targeted-and-stress.txt)均保留。累计源码patch及47项Git blob身份经独立临时index重建47/47匹配，见[归档检查](validation/2026-11-19-m2-public-feed/archive-check.txt)，真实暂存内容未变。未提交/推送；Linux/UBSan/TSan、真实磁盘故障、durable prefix及新路径受控性能仍未验收。

## 2026-11-18：M1第十批Audit冷路径导出错误报告

实际复现坏流下导出仍正常返回，随后补header/记录写入、flush和拥有文件close检查。Journal/Audit/Replay target保留原五组并增五组：黄金CSV、12种写失败、4种flush失败及open错误等通过；两套全新54步构建成功，MSVC Release/ASan均24/24通过（8.23s / 10.43s），额外100万命令差分通过。原始红/绿灯与38项源码身份见[M1第十批](M1_BATCH_10.md)。close检查未做真实故障注入，失败仍可能留下部分文件，没有冷导出性能/原子替换/断电或Linux sanitizer证明。

## 2026-11-18：第九批归档一致性复核（未重跑程序）

当前36项源码Git blob与第九批清单全部匹配；以独立临时Git index从HEAD应用累计patch，重建源码36/36匹配，真实暂存内容不变。Benchmark 3/3哈希及7场景完整；归档中两套24/24、差分和耗时仍属于11月17日实测，不冒充今日重跑。记录见[归档复核](validation/2026-11-17-m1-logger/archive-review-2026-11-18.txt)，组件语义与限制见[第九批](M1_BATCH_09.md)。

## 2026-11-17：M1第九批异步事件日志健康与计数

修复AsyncLogger坏流仍递增written、线程错误未隔离、flush/close失败不可见的问题，增加Monitor展示及Demo/Benchmark收尾检查，不改变恢复Journal/Audit职责。Logger 9组、分配故障9组、Monitor 5组通过，MSVC Release/ASan最终全量均24/24通过（7.56s / 9.71s），额外100万命令差分通过。本轮归档的是已有构建目录复验，不冒充新的完整编译日志；36项源码身份、14日红灯摘录和17日绿灯及单进程Benchmark见[M1第九批](M1_BATCH_09.md)。未实际注入owned-close或worker内部基础设施错误；没有真实磁盘满/断电或Linux sanitizer结果，不扩大为全部日志持久化保证。

## 2026-11-14：M1第八批快照generation非复用

实际复现同代Book已覆盖、manifest打开失败后，旧manifest字节未变但恢复状态已变。修复增加整代路径预检和保留式占位，拒绝复用generation；原v1格式/布局继续加载。多品种快照target扩至8组，两套全新54步构建成功，MSVC Release/ASan均24/24通过（8.05s / 13.22s），额外100万命令差分通过。原始红/绿灯、34项源码身份及边界见[M1第八批](M1_BATCH_08.md)。本批未测快照性能，未执行真实强杀/断电/磁盘满或Linux sanitizer；提交后刷新异常不保证旧manifest仍是当前版本，不能声称完整崩溃恢复。

## 2026-11-14：M1第七批订单池归还与移动所有权

先实际复现池API重复/跨池归还污染空闲表，再增加预分配槽状态和地址检查，禁止池拷贝，移动保持活动对象地址。新增七组池回归与故障程序第八组，MSVC Release/ASan最终24/24通过（7.91s / 12.05s），100万命令差分通过。原始记录、33项源码身份和两份各1进程的七场景观察见[M1第七批](M1_BATCH_07.md)。两次性能采集相隔数小时，不是受控A/B，core数值增大且尚未归因；不宣称性能无退化。没有裸指针代际或全部池API安全承诺，本轮Linux/UBSan/TSan未执行。

## 2026-11-14：M1第六批模拟风控整数边界

先以MAX-1持仓加BUY2复现MSVC浮点投影漏判，随后移除MIN绝对值/有符号价差危险表达式，精确化价格偏离和仓位，累计挂单量超界时锁定风险失效。新增33,936条小范围独立oracle、极值和失败集成回归；两套全新52步构建成功，MSVC Release/ASan均23/23通过（5.38s / 11.74s），额外100万撮合命令差分通过。原始输出、两层源码patch及30项blob见[M1第六批](M1_BATCH_06.md)。本轮未测风险性能；既有Benchmark不包含PreTradeRisk。金额仍用浮点，没有Linux/UBSan/TSan或全域UB证明。

## 2026-11-14：M1第五批分配失败结构清理

实际注入新价格map节点bad_alloc，先复现空簿及部分成交后的孤儿索引，再增量清理未挂簿主动单；已有成交不回滚，Book/Engine仍FAILED，Journal不commit。新增独立故障程序七组回归后，MSVC Release/ASan最终均23/23通过（2.07s / 8.20s），enum_boundary_v2的100万命令差分通过。原始输出、累计源码身份、单进程Benchmark及恢复边界见[M1第五批](M1_BATCH_05.md)。故障程序的可执行文件内new/delete替换会限制ASan部分分配族诊断覆盖，其余22个程序不带替换。没有本轮Linux/UBSan/TSan、磁盘满、进程强杀或真实系统OOM证据，不宣称M1整体完成。

## 2026-11-12：M1第四批直接入口枚举保护

修复OrderBook/ReferenceEngine直接API的非法Side/TIF接收，补INVALID_SIDE/INVALID_TIME_IN_FORCE及UNKNOWN诊断方向。穷举760条非法枚举输入及五组组合回归，差分生成器升级enum_boundary_v2（同seed不等于旧输入）。全新MSVC Release/ASan均22/22通过（7.57s / 10.94s），新生成器100万命令差分通过。红/绿灯、原始日志、累计源码和未归因的单进程性能观测见[M1第四批](M1_BATCH_04.md)。未执行Linux/UBSan/TSan、bad_alloc或磁盘崩溃注入，不据此宣称M1全部完成。

## 2026-11-12：M1第三批整命令Journal

标准CommandJournal在修改簿前预留记录槽，整条命令批次接受；新v2日志/Replay前置验证完整分组，宽容EOF丢弃整条未完成命令。MSVC Release与ASan各22/22通过（2.71s / 10.64s），另100万命令差分通过。Demo/Benchmark已接入标准路径，新并发target加入TSan配置但Linux/UBSan/TSan未执行。原始输出、累计patch、单进程检查及兼容/持久化限制见[M1第三批](M1_BATCH_03.md)。

## 2026-11-12：M1第二批进程内事件输出屏障

在第一批基础上新增有界待发批次、false后的显式suffix续送、异常FAILED停止、重入拒绝及单/多簿快照屏障。新增event_delivery集成target后，MSVC Release与MSVC ASan最终各21/21通过（1.73s / 6.58s）；另100万命令差分通过。原始日志、累计源码patch、单进程基准检查与未完成边界见[M1第二批记录](M1_BATCH_02.md)。R05 Journal整命令接受、durable prefix、Linux/UBSan/TSan不因本批通过而完成。

## 2026-11-11：M1第一批工作区修复

基于`c690dfd`的增量工作区版本，修复累计档位数量溢出、参考模型Post-Only拒单优先级和Monitor按symbol计数丢失。新目录MSVC Release与MSVC ASan各20/20通过（6.54s / 9.47s）；额外100种子×10,000命令差分通过。已新增九组核心边界及两组监控回归。完整源码身份、命令、原始输出、风险与回滚见[M1第一批记录](M1_BATCH_01.md)。Linux/UBSan/TSan与事件提交契约不在本批完成范围。

## 2026-08-07：提交 `a99dd15e6cad`

### Windows Release

- 工具链：MSVC 19.44、CMake/Ninja、C++17、Release。
- 结果：20/20 CTest 通过。
- 差分规模：25 个固定种子 × 4,000 条命令，包含 GTC、IOC 和 Post Only。

### Windows AddressSanitizer

- 配置：`RelWithDebInfo -DENABLE_ASAN=ON -DENABLE_UBSAN=OFF`。
- 构建：46/46 目标成功。
- 结果：20/20 CTest 通过，总测试时间 10.46 秒。
- 结论只覆盖 MSVC AddressSanitizer；MSVC 不提供本项目 CI 所配置的 GCC/Clang UBSan 与 TSan 等价验证。

### Linux / UBSan / TSan 状态

本机检查结果：没有已安装的 WSL Linux 发行版、Docker、GCC 或 Clang。因此本轮没有执行 Linux Release、UBSan 或 TSan，不能将 `.github/workflows/ci.yml` 的存在视为通过证据。它们仍需在真实 Ubuntu runner 上运行，保留 job URL/日志后再更新本节。

### Benchmark 证据

提交 `a99dd15e6cad` 的五个独立进程原始输出、环境元数据、逐轮汇总和 SHA-256 清单位于 [`benchmarks/results/2026-08-07-v2-baseline`](../benchmarks/results/2026-08-07-v2-baseline/)。采集环境为 Windows Balanced 电源计划、未绑核；结果展示环境抖动，不用于跨机器结论。

