# 当前验证记录

## 2026-11-23：M3第六批CI覆盖与旧夹具隔离

本轮仅改`.github/workflows/ci.yml`和测试设施，不改生产或CMake。Release及ASan+UBSan增加显式OFF/ON矩阵、仅ON安装系统zlib、全量后verbose记录五项完整性测试；严格GCC警告、TSan子集、Benchmark smoke及手动差分门禁保留。测试侧给八份旧程序增加原子预留的RAII目录和登记子文件清理，业务断言不删；数量仍43，辅助类不宣称敌对文件系统安全或全项目统一非递归清理。见[第六批范围](M3_CI_AND_TEST_ISOLATION.md)。

三套既有MSVC构建各重编译/链接16步，未设置每配置隔离TEMP，三个CTest调用并行运行后分别43/43：默认OFF Release9.99s、ON Release5.59s、ON ASan14.38s；不是全新92步编译或性能对比。八个受影响程序分别`/O2 /W4 /WX`编译并执行通过。[本机原始输出与命令](validation/2026-11-23-m3-ci-and-isolation/README.md)保留。

[共享临时根探针](validation/2026-11-23-m3-ci-and-isolation/parallel_fixture_probe.ps1)启动三配置×八程序×两份共48个独立进程，子进程共享TEMP/TMP/TMPDIR、各自工作目录。[最终输出](validation/2026-11-23-m3-ci-and-isolation/parallel-shared-temp-final-probe.txt)全部exit0且共享根残留0。首次未进入MSVC环境时32个Release通过、16个ASan报`-1073741515`（缺运行时DLL，测试未执行）；[失败输出](validation/2026-11-23-m3-ci-and-isolation/parallel-shared-temp-probe.txt)保留，随后通过VsDevCmd启动同一脚本/源码成功，不修改机器PATH/执行策略。这不是撮合核心并发、强杀恢复或性能证据。

前一代码提交`05f003b404b4ccdff8db8b559440842b0492d834`的[真实GitHub run45181504978](https://github.com/wangwangscodeheart/New-Version-Low-Latency-Matching-Engine-Engineering-/actions/runs/45181504978)已全部完成：Ubuntu26.04.1/GNU15.2.0，Release43/43（0.82s）、ASan+UBSan43/43（28.02s）、TSan异步子集4/4（0.79s），Benchmark smoke成功，手动10M差分因push事件跳过。四份解码后完整job日志归档，不把smoke当正式性能；GCC原有`-march=native`相关warning和checkout Node版本提示保留。**该run默认OFF，没有gzip ON正向执行**，不把它冒充本轮矩阵或夹具新源码通过。新矩阵在本节首次采集时尚未发布/运行，发布后的结果在同目录按准确代码提交单独归档。

源码身份清单包含前114项重算及新测试辅助头/探针脚本，共116项构建/验证输入。第三方zlib内部未插桩、两项替换new/delete故障程序的限制、无真实市场文件/独立参考/网络恢复/原生性能和M3未整体验收的边界均保留；四个未接线统计工具不纳入本次发布。旧批“没有新Linux结果”为当时事实，本节不重写旧证据。

## 2026-11-23：M3第五批单遍源SHA-256与可选gzip完整性

新增固定块SHA、默认关闭的zlib适配和VerifiedInputReader，接到既有离线CLI；不改V1撮合或M2发布。单次打开/读取原始流，同时hash与可选解压，物理EOF/全部member CRC和ISIZE/可选expected摘要/BinaryFILE/会话检查成功才完成；晚到错误整体STALE，连预先消费一个前缀字节也拒绝接续认证。不是来源/日期认证或文件原子快照；所有输入为自造，无原始市场记录。见[范围与用法](M3_ITCH_INPUT_INTEGRITY.md)。

MSVC19.44.35228.0/CMake4.4.2/Ninja：三套全新构建均92/92，初次默认无zlib Release、启用zlib Release、启用zlib ASan均43/43（15.70s / 14.95s / 19.65s）。补CLI选项和精确4096/8192解压边界后，三配置同时复验触发旧测试的固定Temp文件冲突：两个Release分别41/43、42/43，ASan43/43。失败记录保留，不删除或当通过；各配置设置独立TEMP/TMP后无源码规避，最终三套均43/43（2.34s / 2.43s / 9.98s）。测试耗时受构建/并行负载影响，不是Benchmark。

原始[默认完整构建](validation/2026-11-23-itch-input-integrity/release-no-zlib-build-and-ctest.txt)、[gzip完整构建](validation/2026-11-23-itch-input-integrity/release-zlib-build-and-ctest.txt)、[ASan完整构建](validation/2026-11-23-itch-input-integrity/asan-zlib-build-and-ctest.txt)，以及最终[默认复验](validation/2026-11-23-itch-input-integrity/release-no-zlib-isolated-retest.txt)、[gzip复验](validation/2026-11-23-itch-input-integrity/release-zlib-isolated-retest.txt)、[ASan复验](validation/2026-11-23-itch-input-integrity/asan-zlib-isolated-retest.txt)保留；中间冲突输出见[本批归档说明](validation/2026-11-23-itch-input-integrity/README.md)。新SHA、gzip ON/OFF、协调器 ON/OFF和CLI ON/OFF共7个独立`/O2 /W4 /WX`编译/执行[通过](validation/2026-11-23-itch-input-integrity/strict-integrity-tests.txt)，既有全工程D9025/C4702警告不掩盖。

[实际CLI烟测](validation/2026-11-23-itch-input-integrity/cli-synthetic-final-smoke.txt)使用中文路径、.NET自造gzip及独立Get-FileHash：两种构建的普通文件、开启gzip均exit0且expected摘要验证为true；未启用gzip、坏CRC、错摘要均exit1；缺声明exit2。三次成功均6条S、0股票/订单/成交，压缩源49字节与解码86字节摘要域分别核验，不是真实交易日验收。最初直接运行.ps1被执行策略拒绝，原输出也保留；后续直接执行已审阅脚本块，没有修改机器策略。

可选依赖zlib1.4仅手动下载/校验后在ignored build/本地编译，未vendoring或机器安装，项目不联网拉依赖。第一次官网URL返回HTML，摘要拦截后未解包；官方GitHub发布包1,507,140字节SHA匹配官方值，详见[依赖来源与限制](validation/2026-11-23-itch-input-integrity/dependency-provenance.md)。ASan仅覆盖C++封装/测试，链接的Release C库未插桩；既有两项替换new/delete程序的诊断局限保留，其余41项无该替换。

[114项规范化Git blob清单](validation/2026-11-23-itch-input-integrity/source_git_blobs.txt)为旧108项重算加本批3头文件/3测试，旧证据不覆盖。没有新的Linux/UBSan/TSan、真实授权源/独立参考/网络恢复/原生性能，M3仍未整体验收。未接线分配统计在途文件不计入测试完成项或本次发布；下文“未提交/推送”为各历史采集时状态，后续发布以Git记录为准。

## 2026-11-22：M3第四批显式报告/时间profile与离线检查入口

新增`itch_session_validation.hpp`、`itch_trade_ledger.hpp`、离线CLI及重建器显式配置；旧默认仍是簿/元数据模式，不自动获得新保证。严格选项校验时间域、完整系统码序列、Match/Broken关联，CLI默认启用并要求本地使用声明后才访问文件。V1撮合、MarketDataBook/M2发布在本批不改；所有输入为自造报告/字节/临时文件，原始市场记录0，日期标签不认证来源。范围与非目标见[第四批说明](M3_ITCH_REPORT_VALIDATION.md)。

两套全新MSVC x64构建均86/86，初次全量Release/ASan均40/40（13.39s / 20.00s）。随后补成交节点分配与输出故障/Unicode入口，重建受影响目标再全量40/40（3.69s / 9.26s）；独立复核指出help开启ostream异常时返回码边界后，补统一失败返回及倒退warning，再重建入口和其测试，全量最终仍40/40（3.14s / 9.79s）。保留各轮原始输出，不把更早二进制/源码当最终状态。

原始[完整Release](validation/2026-11-22-itch-report-validation/release-build-and-ctest.txt)、[完整ASan](validation/2026-11-22-itch-report-validation/asan-build-and-ctest.txt)、[中间Release](validation/2026-11-22-itch-report-validation/release-final-retest.txt)、[中间ASan](validation/2026-11-22-itch-report-validation/asan-final-retest.txt)、[最终Release](validation/2026-11-22-itch-report-validation/release-io-retest.txt)、[最终ASan](validation/2026-11-22-itch-report-validation/asan-io-retest.txt)均保留。新四测试与故障程序以`/O2 /W4 /WX`[独立重编译运行](validation/2026-11-22-itch-report-validation/strict-contract-and-fault-tests.txt)；最终IO修正后[再次严格编译测试和MSVC Unicode入口](validation/2026-11-22-itch-report-validation/strict-final-io-tests.txt)通过。全项目既有D9025/C4702保留，未宣称全工程零警告。

[最终CLI烟测](validation/2026-11-22-itch-report-validation/cli-final-synthetic-smoke.txt)使用中文路径下自行生成的六条系统消息与BinaryFILE标记：成功exit0、records6、所有市场/订单/报告计数0；未给本地使用声明时exit2。其PowerShell NativeCommandError文本是预期stderr封装，退出码已核验，不是真实市场数据验收。较早[同类烟测](validation/2026-11-22-itch-report-validation/cli-synthetic-smoke.txt)保留，最终以final文件为准。

时间/ledger/严格会话/CLI测试分别5/6/7/2组，含输出短写/flush/开启异常与warning、范围/倒退策略、缺阶段、价格/数量差异、重复/错股票/未知/重复B、Q-break拒绝和零量Q不入账。故障程序原簿20组38次，R/A/U三组16次，L一组1次，新增相关执行一组1次，共25组56次；账本分配失败保留旧显示量/账本/时间/统计，合法B实测分配尝试0且不恢复显示数量。不是全链路零分配或穷尽异常点证明。

[108项Git blob源码身份](validation/2026-11-22-itch-report-validation/source_git_blobs.txt)与[归档说明](validation/2026-11-22-itch-report-validation/README.md)对应本批工作区，不覆盖旧100项身份。两个独立故障程序替换new/delete的ASan覆盖限制保留；其他38个CTest不带替换。无本轮Linux/UBSan/TSan、实际授权样本、gzip/文件hash、真实参考对账、全业务/账户/官方tape、网络恢复或原生性能验收。未暂存、提交或推送；M3仍未整体验收。

## 2026-11-22：M3第三批历史元数据与22类型合成覆盖

将Y/L/V/W/K/J/h/I/N升级为typed解码，并在重建器保存最新股票/市场元数据；K头Locate固定0按股票正文路由，V/W全市场，h按Q/B/X场所独立保存。V1核心、MarketDataBook/M2发布在本批不改，新消息不应用显示订单更新。历史版本、特殊值、MPID容量和业务非目标见[第三批说明](M3_ITCH_METADATA.md)。原始市场记录仍为0，未知类型仍拒绝，不把22种字段覆盖当作完整业务或真实2020会话通过。

两套全新MSVC x64构建目录均76/76完成，初次Release/ASan全量35/35（12.50s / 14.35s）。随后补独立L元数据故障扫描，只重建该测试并全量复验，最终均35/35（3.09s / 11.27s）。最终核心decoder、新元数据decoder/会话及独立故障四份测试以`/O2 /W4 /WX`重新编译运行通过；完整构建保留既有D9025/C4702警告，没有全项目零警告声明。

原始[完整Release](validation/2026-11-22-itch-metadata/release-build-and-ctest.txt)、[完整ASan](validation/2026-11-22-itch-metadata/asan-build-and-ctest.txt)、[最终Release](validation/2026-11-22-itch-metadata/release-final-retest.txt)、[最终ASan](validation/2026-11-22-itch-metadata/asan-final-retest.txt)、[严格编译与故障](validation/2026-11-22-itch-metadata/strict-contract-and-fault-tests.txt)、[100项源码Git blob](validation/2026-11-22-itch-metadata/source_git_blobs.txt)及[归档说明](validation/2026-11-22-itch-metadata/README.md)保留，不覆盖旧批证据。新增元数据decoder四组、会话七组均为手工/程序自造字节，含物理短缓冲区、非对齐/所有权、完整22类混合流任意切点/逐字节/istream、显示簿不变、合法/非法flags、K取消零值及未知/截断STALE。

独立故障程序原簿20组38次、R/A/U三组16次、L一组1次，共24组55次失败；L已有项更新实测分配尝试0。新增metadata map使MSVC的R构造多一个分配点，故上轮15次/本轮16次会话计数不同，并非修改旧输出。每次注入均核对旧显示簿/索引/统计或metadata不变，错误后拒绝继续；这些次数不是全系统分配测量或跨STL固定常数。

两个独立故障程序带new/delete替换，ASan分配族诊断局限保留，其他33个CTest不带替换。尚无本轮Linux/UBSan/TSan、许可真实集、冻结2018规范、独立参考、gzip检查、完整时段/成交账本、网络恢复或原生性能结论。`COMPLETE`及metadata可读取不是完整性/可交易/监管合规证明。未暂存、提交或推送；未接线的分配测量在途文件继续保留但不算验收项。

## 2026-11-22：M3第二批目录路由与可见订单簿重建

新增`ItchBookRebuilder`，组合已有BinaryFILE/ITCH解码并应用到独立MarketDataBook；后者只增加原生减量/替换、查单、BBO及不变量接口，原Feed apply行为保持。V1撮合、TradingEngine、M2发布与旧测试源码未改。目录/全市场日内Seen引用、U继承MPID、C按挂单价扣量、H状态与P/Q/B计数、容量/终止失效范围见[第二批说明](M3_NATIVE_REBUILD.md)。所有市场字节为自行生成，真实记录下载数仍为0。

两套全新MSVC x64目录均72/72构建成功，初次全量Release/ASan各33/33（10.17s / 16.44s）。收口时Release重建受最终header注释影响的两个目标，再全量33/33（3.87s）；ASan确认无需重建后全量33/33（10.18s）。三份新增测试独立以C++17 `/O2 /W4 /WX`重编译运行通过；完整项目保留已有D9025及ASan故障Sink C4702警告，未宣称全项目零警告。

原始[完整Release](validation/2026-11-22-itch-rebuild/release-build-and-ctest.txt)、[完整ASan](validation/2026-11-22-itch-rebuild/asan-build-and-ctest.txt)、[最终Release](validation/2026-11-22-itch-rebuild/release-final-retest.txt)、[最终ASan](validation/2026-11-22-itch-rebuild/asan-final-retest.txt)、[严格编译及故障扫描](validation/2026-11-22-itch-rebuild/strict-contract-and-fault-tests.txt)保留；[98项源码Git blob身份](validation/2026-11-22-itch-rebuild/source_git_blobs.txt)与[归档说明](validation/2026-11-22-itch-rebuild/README.md)绑定本轮工作区，不覆盖上轮94项身份或旧Benchmark证据。

原生簿7组、重建9组合成回归通过；包含全切点/逐字节/istream、零量Q、精确结束边界、S/E后D/B、吞掉重入异常/设备IO失败仍STALE。故障程序原簿20组扫描38次失败，R/A/U适配器3组15次失败；每次检查订单/深度/BBO、不变量与Seen/Active/目录索引，失败后不重启同一会话。MSVC运行时探得扩容边界为8单，不把该数或分配次数写成跨STL性能结论。

该新故障程序与既有allocation_failure分别在自身可执行文件替换new/delete，ASan分配族诊断覆盖受限；其他31个CTest不带这些替换。没有本批Linux/UBSan/TSan、真实样本/独立参考、gzip完整性、全22类型、网络恢复或性能验收。`COMPLETE`不是完整交易日/真实数据无缺口证明。上轮独立分配测量工具仍未接线；当前未暂存、提交或推送。

## 2026-11-21：M3原始ITCH读取与核心解码（合成测试）

新增BinaryFILE固定存储流式读取、严格结束标记/边界EOF区分，以及13种核心wire类型的字段解码；其余9种历史类型只校验长度，默认仍拒绝。未改V1撮合或M2发布实现，没有真实记录、原生簿重建或M3整体完成声明；数据选择、协议依据及授权门禁见[M3说明](M3_NASDAQ_ITCH.md)。

两个新目录MSVC Release/ASan均66/66完整构建成功，初次全量30/30（10.33s / 12.90s）。独立复核后补禁用reader复制/移动及streambuf重入失效，并加强精确短分配和`S/C`非终止符测试；最终Release重建两目标后30/30（4.71s），ASan检查无待构建项后30/30（7.63s）。另将最终两测试以`/std:c++17 /W4 /WX /O2`独立重编译运行通过；没有关闭警告。完整构建仍保留既有D9025和ASan故障Sink的C4702警告，不宣称整个项目零警告。

原始[初次Release](validation/2026-11-21-itch-core/release-build-and-ctest.txt)、[初次ASan](validation/2026-11-21-itch-core/asan-build-and-ctest.txt)、[最终Release](validation/2026-11-21-itch-core/release-final-retest.txt)、[最终ASan](validation/2026-11-21-itch-core/asan-final-retest.txt)、[严格编译及契约测试](validation/2026-11-21-itch-core/strict-contract-tests.txt)均保留；[归档说明](validation/2026-11-21-itch-core/README.md)与[94项Git blob身份](validation/2026-11-21-itch-core/source_git_blobs.txt)对应本轮构建输入与相关脚本，不是原始市场数据SHA或旧89项清单的覆盖改写。

来源清单已计划Nasdaq 2020-01-30，但下载数为0、SHA为空，所有测试为自造字节。尚无真实样本完整性、2020全会话覆盖、原生簿/独立参考、gzip/IO/解码性能或网络恢复结果。新增代码未取得Linux/UBSan/TSan runner结果；旧`b889136`的CI不覆盖它。既有allocation_failure程序替换new/delete的ASan局限保留，两项新测试没有这种替换。上轮未接线的独立分配统计工具不计入本批构建或测试完成项。当前未提交/推送。

## 2026-11-20：合成公开行情工程基线（待正式M5验收）

保留股票范围，M3等待获授权样本；本轮未接入任何外部原始数据，也未实施M4恢复。仅增Benchmark辅助fixture、对应测试、CMake目标和采集脚本，`src/`撮合/发布实现未改。原七场景保留，新增Journal/公开消费、完整诊断/公开消费及16命令串行burst三场景；精确范围见[BENCHMARK](BENCHMARK.md)。

两个新目录MSVC Release/ASan各62/62构建成功，初次全量均28/28（8.81s / 13.54s）；之后仅修正Benchmark元数据把单进程fixture误标为process runs的表述，重建该目标并全量复验，最终仍均28/28（2.13s / 9.76s）。原D9025及ASan故障Sink C4702警告保留。新fixture参数/采样/状态对照测试通过，额外100种子×10,000命令差分通过。采集脚本的mock验证和真实多进程采集分开，不把mock吞吐当实测。

原始[完整Release](validation/2026-11-20-public-feed-benchmark/release-build-and-ctest.txt)、[完整ASan](validation/2026-11-20-public-feed-benchmark/asan-build-and-ctest.txt)、[最终Release](validation/2026-11-20-public-feed-benchmark/release-final-retest.txt)、[最终ASan](validation/2026-11-20-public-feed-benchmark/asan-final-retest.txt)、[fixture及差分](validation/2026-11-20-public-feed-benchmark/fixture-and-stress.txt)保留。当前工作区尚未提交/推送；下节`b889136`的Linux/UBSan/TSan通过不能覆盖新增目标。

[本轮源码增量patch](validation/2026-11-20-public-feed-benchmark/source_changes.patch)以`b889136`为基线，和[89项规范化Git blob清单](validation/2026-11-20-public-feed-benchmark/source_git_blobs.txt)绑定项目构建输入；独立临时index重建89/89匹配，真实暂存未变。两次采集的12项文件SHA-256和最终二进制身份已核对，30条结果/9条公开计数的字段、分位数顺序和三进程覆盖通过校验，见[归档检查](validation/2026-11-20-public-feed-benchmark/archive-check.txt)。清单不是任意checkout换行字节哈希，也不是全部系统依赖的可复现构建证明。

最终3进程×10场景stdout、30条结果和9条公开计数行位于[工程初测v2](../benchmarks/results/2026-11-20-public-feed-engineering-v2/)，旧版元数据的初测目录保留不覆盖。每个新场景默认每进程60,000命令/67,500消息/3,210,000 wire字节；逐笔样本52,500条，无公开增量的7,500条只计输入吞吐，不虚构消费延迟。验证所有日志完整排空、消费者连续序号及空簿后才报告成功；异步stop和昂贵最终对账不计入batch时间，不声称durable延迟。

环境为Windows Balanced、未绑核/未冻结频率的3进程工程初测，计时空测仅报告不相减；没有新分配次数/字节统计、总堆预算、逐层性能、真实市场负载、网络/并发到达或恢复成本证据。P50/P99/P99.9/Max及wire高水位不能据此扩大成生产SLA、零分配、优化收益或M5全部完成。

## 2026-11-20：GCC比较修复与真实Linux CI

提交`b889136644264e61ff9b8106330283330d7cb7a4`仅把编码测试中的`24 + uint16_t`改为`FeedEncoder::HEADER_SIZE + uint16_t`，消除`size_t`与`int`比较造成的`-Werror=sign-compare`；没有关闭严格警告或修改撮合逻辑。本机受影响目标重建后，MSVC Release/ASan各27/27通过（6.42s / 9.72s）。

[GitHub Actions run 44668927537](https://github.com/wangwangscodeheart/New-Version-Low-Latency-Matching-Engine-Engineering-/actions/runs/44668927537)实际完成并成功：Linux Release 27/27（0.87s）、ASan+UBSan 27/27（20.05s）、异步组件TSan子集4/4（0.81s）、benchmark smoke通过；`differential-stress`因仅允许workflow_dispatch而跳过。TSan仅覆盖配置的异步组件子集，不证明多线程撮合；故障程序替换new/delete的覆盖限制保留。CI时间和smoke不是新公开链路的正式性能证据。

本节记录的是上述提交的实际运行，不把11月19日归档或下面历史“未执行Linux”措辞改写成当时已通过。当前M2收口仅同步文档与后续验收门禁，没有重跑程序或下载真实数据；M3→M6仍是待实施路线，见[M2收口记录](M2_PUBLIC_FEED.md)。

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

