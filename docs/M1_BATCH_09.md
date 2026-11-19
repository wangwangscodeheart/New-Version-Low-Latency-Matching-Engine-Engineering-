# M1 第九批：异步事件日志的失败可见性

归档/最终复验：2026-11-17（Asia/Tokyo）；实现与首次故障复现始于11月14日。增量处理M0 S04中的AsyncLogger缺口，保留V1撮合核心、原CSV字段、BoundedQueue和Journal契约，不宣称M1/M3/V2.1整体完成。

2026-11-18仅做[归档一致性复核](validation/2026-11-17-m1-logger/archive-review-2026-11-18.txt)，未重跑程序：当前36项源码身份全部匹配；在独立临时Git index上从HEAD应用累计patch，重建结果也36/36匹配，真实暂存内容未改动。Benchmark 3/3哈希及7场景齐全。下文CTest/差分仍为11月17日实测，不改写执行日期。

后续：[M1第十批](M1_BATCH_10.md)已补Audit冷导出的错误检查；以下Audit相关限制保留本批归档口径。

## 1. 三种日志不混淆

- CommandJournal：恢复关键路径，原有整命令预算/接受及失败屏障不变。
- AuditLog：内存中的订单生命周期视图，原有CREATE/SUBMIT/TRADE/CANCEL逻辑不变。
- AsyncLogger：独立的best-effort通用事件诊断日志。本批只改这个组件及其监控/示例接线，不把它冒充恢复WAL或异步持久化Audit账本。

原AsyncLogger在链式输出后无条件增加written；流坏掉仍显示成功，线程中格式化/输出异常也没有捕获，最终flush/close不检查。先增加可借用ostream的输入接缝、尚未改变错误计数的版本，实际复现`accepted=1 written=1`并断言失败；修复后同一核心用例为`accepted=1 written=0`。[11月14日红灯摘录](validation/2026-11-17-m1-logger/logger-red.txt)。摘录保留stdout/stderr交错，不冒充完整构建日志或旧原版逐字节源码。

## 2. 修改与ADR-015

状态：本节所述组件健康/计数与已测故障路径已实现。

| 文件 | 修改 | 动机 / 真实组件对应 |
|---|---|---|
| `src/async_logger.hpp` | 健康状态、失败计数、错误检查/捕获、借用ostream适配 | 非关键诊断输出的可靠可观测性，不改变交易提交点 |
| `src/monitor.hpp` | snapshot/CLI展示accepted/written/failed/dropped及logger health | 控制面能发现丢弃、失败和flush异常，而非只看写计数 |
| `src/v2_main.cpp`、`benchmarks/v2_perf.cpp` | stop后检查健康、丢弃和失败 | 示例/基准不在日志漏记后仍报告完整成功；检查不在撮合循环内 |
| `tests/async_logger_tests.cpp` | 原2组扩至9组，受控streambuf故障/背压/所有权验证 | Windows可执行的确定性故障测试，不依赖/dev/full或耗尽真实磁盘 |
| `tests/allocation_failure_tests.cpp`、`tests/monitor_tests.cpp` | 生产者复制bad_alloc及失败指标回归 | 复用既有独立分配钩子、验证业务侧可见结果 |

未修改OrderBook、价格优先级、AuditLog、CommandJournal、队列实现、CMake或CI。本批没有新框架/新target，CTest仍24项。

### 状态与计数

| API/字段 | 准确定义 |
|---|---|
| accepted | 成功入队的记录数，不代表已经写入或持久化 |
| written | 完成一次格式化记录输出且ostream仍good；不是flush/fsync确认数 |
| failed | 已接受但没有成功完成上述输出的记录数；最终stop后稳定 |
| dropped | 本次enqueue被拒绝：满队列、停止、不可用或入队复制/分配异常；从未调用enqueue的事件不在计数内 |
| healthy() | 未观察到流/工作线程错误；HEALTHY或正常STOPPED都为true，不要求dropped=0 |
| available() | 当下允许接收新日志；停止或失败后为false，不能据此承诺跨线程原子准入 |

生产者静止且单控制者stop完成后，`accepted == written + failed`。运行中各计数独立读取，Monitor快照不是这些原子的事务快照，不能要求每个瞬间都满足这个等式。flush/close失败可能出现`accepted == written`、`failed == 0`，但health为WRITE_FAILURE；所以Demo/Benchmark不能仅比较计数。

工作线程先格式化，再写记录；失败后不重写部分记录，继续取出队列尾部并计为failed，避免恢复后把尾部当作连续成功输出。捕获格式化/写入/flush/owned-file close异常，错误不会被正常STOPPED覆盖。若发生意外的工作线程基础设施异常，外层保护退出；stop把尚未成功输出的尾部也归入failed，**不承诺所有此类异常后都物理出队排空**。

enqueue不做文件I/O；复制/队列分配异常被捕获、计dropped并设置失败状态，不从这个可选订阅者抛出阻断撮合。队列满仍非终止性：后续可继续记录，丢弃可见但不自动关停引擎。必需Journal原有停止策略保持独立；若业务要求诊断日志失败即停机，应由控制策略明确决定，本批不增加隐含交易门禁。

### 流与线程所有权

文件构造器保持原调用方式。新增`AsyncLogger(std::ostream&, capacity)`借用流，不负责close；流必须活到stop/join结束，期间不能被其他线程并发读写。header及首次flush在冷初始化时完成并检查，失败不启动worker；每事件磁盘I/O仍由worker执行。拥有的文件由worker最终flush/close。

先停止生产者/Dispatcher发布，再由一个控制线程stop；attach/reset/析构也须与发布外部串行。支持顺序重复stop，但不是“多个并发stop都会等待join”的协议，不能在worker自身或并发析构中调用它。stop解订阅后，Logger不会观察后续Dispatcher事件，这些事件也不会自动增加dropped。

## 3. 测试覆盖与实际结果

AsyncLogger target共9组：原队列契约、原真实文件输出/有序关闭；失败流不计written；短写与抛异常保留1条成功前缀/2条失败尾部；门控满队列准确丢1条且生产者不访问sink；flush返回失败/抛异常；header写/flush失败及零容量拒绝；借用流/重复stop/析构排空；可选Logger失败不影响canonical Journal接口与Audit生命周期。

故障streambuf用条件变量门控，不靠sleep猜线程调度。只有worker被确认阻塞后才填满队列；异常路径的RAII先释放门控再销毁Logger，避免测试自身挂死。实际文件测试使用独占的临时目录，清理仅限fixture创建的绝对直接子目录。

既有分配故障target现9组：新增主线程长Symbol复制处的bad_alloc注入，enqueue不抛出，accepted/written/failed均0、dropped=1、health为ALLOCATION_FAILURE。Monitor target现5组，包含`written=accepted=1`但flush失败的snapshot/CLI展示。Journal隔离用例使用接口记录mock，不把它称为真实磁盘Journal故障测试；实际Journal仍由原有测试覆盖。

| 2026-11-17实际复验 | 结果 | 证据 |
|---|---|---|
| MSVC Release / Ninja | 24/24 CTest通过，7.56s | [构建检查与全量测试](validation/2026-11-17-m1-logger/release-build-and-ctest.txt) |
| MSVC ASan RelWithDebInfo | 24/24通过，9.71s | [构建检查与全量测试](validation/2026-11-17-m1-logger/asan-build-and-ctest.txt) |
| 独立Logger/分配/Monitor及参考差分 | 9/9/5组通过；enum_boundary_v2的100种子×10,000命令通过 | [绿灯与100万命令](validation/2026-11-17-m1-logger/green-and-stress.txt) |

归档为已有m1-logger构建目录的最终检查/复验，输出为ninja无需重编；不冒充本次新建目录的完整编译日志。红灯后扩充过测试，不声称整个测试文件与红灯阶段相同。既有编译警告没有被包装成零警告。

owned-file close错误检查、worker内部基础设施/分配异常保护经过代码审查，但未分别定点注入；分配测试只覆盖生产者复制。没有真实磁盘满、网络盘故障、强杀、断电及本轮Linux/UBSan/TSan结果。独立分配故障程序替换new/delete的ASan覆盖限制仍见[第五批](M1_BATCH_05.md)。

VS x64 Developer cmd：

```bat
chcp 65001
cmake -S . -B build/m1-logger-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/m1-logger-release --parallel 8
ctest --test-dir build/m1-logger-release --output-on-failure
cmake -S . -B build/m1-logger-asan -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DENABLE_ASAN=ON -DENABLE_UBSAN=OFF
cmake --build build/m1-logger-asan --parallel 8
ctest --test-dir build/m1-logger-asan --output-on-failure
build\m1-logger-release\matching_engine_async_logger_tests.exe --failed-stream-only
build\m1-logger-release\matching_engine_differential_tests.exe --seeds 100 --commands 10000 --seed-base 0xD1FF0000
```

## 4. 性能与源码身份

[本批七场景观察](../benchmarks/results/2026-11-17-m1-logger-check/)完整保存raw/环境/汇总：core 50.05ns/op、routing 51.71、dispatcher 129.65、monitor 174.71、async logger 575.83、async journal 402.67、full_v2 884.87。full采样P99=3,300ns、P99.9=51,800ns、max=483,100ns。七行齐全，3/3证据文件SHA-256匹配。

仍为Windows Balanced、未绑核/固定频率、后台未隔离，仅1个新进程；程序显示的5轮只是进程内fixture批次。full数值明显不同于早前约4–5微秒观察，未做受控A/B归因，**不据此声称提速或无退化**；[第七批性能门禁](M1_BATCH_07.md)仍开放。60,000混合命令含撤单，不能把operations/s等同纯新单吞吐。

新增健康/drop/failed检查在stop之后，submit计时不包含最终drain/flush/close；header初始化也在fixture建立阶段。没有fsync确认，不是持久化或网络端到端延迟。原checksum仍是有限结果摘要，不替代全状态/日志比对；现在还会拒绝日志未干净收尾的Benchmark样本。

采集沿用现有Windows脚本，MATCHING_ENGINE_RAW_OUTPUT=1，Executable为`build/m1-logger-release/matching_engine_v2_benchmarks.exe`，OutputDirectory为`benchmarks/results/2026-11-17-m1-logger-check`，Runs=1。原始UTC时间标签不修改；目录日期按Asia/Tokyo归档。

基线仍为`c690dfd8b008e06690e914e272173a7f4e55d164`加未提交工作区，环境明确dirty。[九批累计patch](validation/2026-11-17-m1-logger/source_changes.patch)及[36项Git blob身份](validation/2026-11-17-m1-logger/source_git_blobs.txt)冻结当前已改编译输入及构建/脚本/证据属性；blob按Git文本入库规范化，不是任意checkout字节SHA-256。patch对基线index的`git apply --cached --check`通过且不改index；不要重复应用到当前工作区。

## 5. 限制、回滚与面试介绍

诊断CSV仍保留原格式化规则；本批没有整体解决payload截断/字段转义，也不证明输入业务语义完整。健康只说明观察到的组件状态，不证明每个订单都进了日志、每条日志已落盘或崩溃后可恢复。AuditLog.save仍是独立冷路径导出，本批未为它新增flush/close失败保证或异步持久化。

回滚只撤本批AsyncLogger/Monitor新增字段与错误路径、Demo/Benchmark收尾检查和对应测试，不恢复整个Monitor或v2_main而删除前批修复；独立分配测试只删除新增Logger用例，不删除全文件。回滚后安全失败流用例应重新失败，撤销日志健康声明。

S04中AsyncLogger错误计数/不可见问题已针对性修复；Audit导出、Journal durable prefix、公开Feed闭环、真实磁盘故障、完整并发生命周期和受控性能仍未全部完成。

面试可介绍：我把“入队、成功交给流、没能输出、入队拒绝”拆开统计，验证了flush失败时written仍可能等于accepted，所以必须同时检查健康状态。我保持诊断日志best-effort、恢复Journal必需的职责差异，并用门控streambuf和主线程bad_alloc注入复现故障，不把普通流成功当作磁盘持久化。
