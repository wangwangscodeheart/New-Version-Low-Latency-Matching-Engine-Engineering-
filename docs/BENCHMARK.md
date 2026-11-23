# Benchmark 与性能证据

## 测量边界

- V1 benchmark：OrderBook 核心及场景矩阵。
- V2原七场景：预解析 InstrumentId、字符串路由、冻结 Dispatcher、Monitor、Async Logger、Async Journal 和历史 Full V2 的逐层开销。
- 新增三个合成公开行情场景：Journal/完整诊断到本地消费，以及16命令串行burst；输入与原七场景不同，不能直接拿旧数值做百分比比较。
- Batch Average 用于比较吞吐；逐笔采样用于观察 Median、P99、P99.9 和 Max。两者不能混为同一口径。

benchmark 输出包含 commit、CPU、操作系统、编译器、有效编译参数、Build Type、负载规模和 checksum。正式结果必须来自 Release 构建和固定环境；CI smoke 只证明程序可以运行。

## M2公开行情链路：证据缺口与M5验收要求

状态（2026-11-20）：**正式M5验收尚未完成。** 下述分层与受控实验仍是验收要求；同日后续已增加三个合成工程场景，见下一节。原七场景的EngineFixture只连接Monitor、Logger和Journal；`full_v2`这个历史名称仍不包含公开行情链路，不能把它或V1纳秒成绩直接当作新增场景的结果。

新链路要分别测量：撮合核心、可见事件投影/引用映射、单帧编码、整命令入队/出队、解码、本地簿更新，以及完整提交到消费闭环。完整闭环的结束点应是本命令全部可见增量已经应用并通过一致性检查，不只是`submit`返回或入队成功；无可见增量的拒单/IOC残量等单列，不能虚构消费样本。Journal接受、异步排空和durable属于不同边界，未实现durable时不得输出“持久化延迟”。

每个分层及完整场景必须保留：

- 输入manifest/哈希、source commit、编译参数、事件/日志开关、预热与样本量、CPU/OS、绑核/电源/频率策略及环境噪声；合成和真实验证集分开报告。
- orders/s与feed messages/s各自分母，含多品种、挂单/撤单/部分及跨档成交的公开消息放大比例；拒收、失败与丢弃计数不得从结果中消失。
- 逐笔P50/P99/P99.9/最大值、吞吐和计时器空测开销；批量均值与逐笔分位数分列，说明探针成本及是否包含排队，避免逐笔串行提交/立刻消费掩盖积压尾延迟。
- 分配次数/字节、队列峰值消息数/批次数/wire字节及总内存的区别。分配统计覆盖哪些线程必须明示；可用独立计数运行避免把统计探针成本误作正常性能，不能用wire预算声称总堆有界。
- 固定负载的多个全新进程原始输出和逐轮分布，不只挑最快值；性能运行结束后仍须验证可见订单/深度、连续序号及输出完整性。没有同输入、同环境、同边界的对照，不报告优化百分比。

M3/M4验收前允许做注明局限的工程冒烟，但不提前宣称完成真实市场分布或恢复路径的性能验收。新链路没有SLA或已证明的零分配承诺；当前运行时Monitor也没有分位数直方图。

## 2026-11-20：合成公开行情工程基线（非M5正式验收）

保留原七场景，另增三场景，源码位于`benchmarks/v2_perf.cpp`和`benchmarks/public_feed_benchmark.hpp`：

| 新场景 | 接线与消费策略 | 实测队列高水位：批次/消息/wire字节 |
|---|---|---|
| `journal_public_feed_sync` | 实际AsyncJournal接受 → Publisher/Encoder → 队列 → Decoder/独立本地簿，每命令drain | 1 / 2 / 112 |
| `full_v2_public_feed_sync` | 同一混合负载，增加冻结Dispatcher的Monitor/AsyncLogger，每命令drain | 1 / 2 / 112 |
| `full_v2_public_feed_burst16` | 同样的Monitor/Logger/Journal，串行提交16条命令后drain | 14 / 18 / 856 |

负载版本为`synthetic_mixed_visible_v1`，仅使用AAPL/TSLA/NVDA模拟命令，没有外部市场记录。每品种一轮：挂卖10、再挂卖6、买12（完全吃第一单并部分吃第二单）、撤第二单余量、低价挂买3、卖5（成交3并挂剩余2）、撤该余量、空簿IOC买1。即8条输入命令，其中6条新单/2条显式撤单，产生4 Add、3 Trade、2公开Cancel，共9帧/428字节；IOC剩余撤销不公开。默认每品种2,500轮：60,000命令、67,500帧、3,210,000 wire字节、52,500有公开增量命令及7,500无增量命令。源簿容量为16，和历史场景的大容量/纯挂撤负载不同，不能直接做旧/新百分比对比。

计时与完整性边界：

- 吞吐的`operations`分母是全部输入命令，包含显式Cancel和无公开增量的IOC，不是新订单数。消费者帧吞吐可由同一次batch的帧数/总命令数乘命令吞吐计算，不把帧与订单混为一谈。
- 新场景的逐笔样本仅针对有公开增量的命令，从提交前计至覆盖它的`drain()`返回。burst中一批命令共用该批drain结束点，表示覆盖批次返回的等待上界，不是逐消息内部apply的精确时刻，也不是仅测入队耗时。无增量命令不伪造消费样本；不足16条的尾批同样排空并计样本。
- 吞吐batch和逐笔采样分开运行；每个独立进程先预热，再测5个新fixture的batch中位数，最后单独采样。批量时间包含生成/提交/有界消费和健康计数检查；昂贵的逐盘口验证仅在未计时预热/测试开启，最终对账和Journal/Logger stop排空在batch计时之后。
- Journal计时只到进程内整命令接受，不含异步尾部排空，更不是fsync/durable。只有stop后日志健康、无丢弃、记录数匹配，消费者空簿/连续序号/队列排空验证通过才输出成功结果。
- 输出计时器空测分位数，仅报告不相减；wire高水位不等于总堆内存。**分配次数/字节本轮未测量**，各编码/解码子阶段也未独立计时。串行burst不是并发到达、真实网络或恢复实验，仍有协调遗漏与探针开销限制。

最终原始证据位于[2026-11-20-public-feed-engineering-v2](../benchmarks/results/2026-11-20-public-feed-engineering-v2/)：3个独立进程、每进程10场景；`summary.csv`保留原10列结果格式，`public_feed_summary.csv`保存三个公开场景的计数、采样范围和高水位，另有stdout、环境/二进制SHA-256及checksum清单。采集使用MSVC Release、Windows Balanced、未绑核、未冻结频率，工作区基于`b889136`且有未提交增量，不宣称这是原提交自带的新结果。样本量/环境未满足受控性能门禁，不能报告生产SLA、零分配、优化收益或真实市场延迟。

此前[初测目录](../benchmarks/results/2026-11-20-public-feed-engineering/)保留不覆盖；其历史`process_runs`字段实际指单进程5个batch fixture。最终二进制已将该字段/显示名修正为`batch_fixtures_per_process`，重新采集到`-v2`目录，不混用两次源码/二进制身份。当前stdout的METADATA_JSON为schema 2；RESULT_CSV格式仍兼容旧采集，新增PUBLIC_FEED_CSV单独汇总。

VS x64 Developer cmd构建后，PowerShell采集示例（目标目录必须不存在）：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/run_benchmark_windows.ps1 -Executable build/public-feed-bench-release/matching_engine_v2_benchmarks.exe -OutputDirectory benchmarks/results/my-public-feed-engineering -Runs 3
```

该ExecutionPolicy仅作用于子进程，不修改系统策略。采集脚本自动设置并恢复RAW_OUTPUT环境变量；每次执行在`build/benchmark_work/`的独占工作目录运行，不在仓库根目录覆盖同名临时日志。源码/测试与边界记录见[本轮验证](VALIDATION.md)。M3继续等待获授权股票样本，不改用加密永续；本轮未开展M4重同步。

## Linux perf

在仓库根目录运行：

```bash
bash scripts/run_perf_linux.sh
```

可选固定 CPU 核心：

```bash
PERF_CPU_CORE=2 bash scripts/run_perf_linux.sh
```

脚本生成独立时间戳目录 `benchmarks/results/perf-<UTC>/`，保存：

- 环境、CPU、内核、governor、Turbo 和 commit；
- CMakeCache；
- benchmark 原始输出；
- 五轮 `perf stat` CSV；
- `perf record` 数据和文本报告。

重点观察 cycles、instructions、IPC、branches、branch-misses、cache misses、page faults、context switches 和 CPU migrations。若内核限制硬件计数器，应调整 `kernel.perf_event_paranoid` 或由管理员授权，不能伪造缺失结果。

## VTune

Windows 或 Linux 上可对 Release/RelWithDebInfo 的两个 benchmark 分别执行 Hotspots 和 Microarchitecture Exploration。报告中必须记录目标二进制、commit、编译参数、采集模式、CPU affinity 与电源模式。VTune GUI 导出的截图或报告应放入独立结果目录，不应只在 README 中保留手工摘录。

## 当前 V2 本机基线

2026-08-07 在 Windows 10、Intel Family 6 Model 183、MSVC 19.44、Release `/O2`、Balanced 电源计划下运行。基准元数据 commit 为 `a99dd15e6cad`。表格取 5 个独立进程各自结果的中位数；[原始 stdout、环境 JSON 和逐轮 CSV](../benchmarks/results/2026-08-07-v2-baseline/) 与表格一起入库。它不是 CI 或生产延迟承诺。

| 场景 | Batch avg ns | Throughput/s | Sample median ns | P99 ns |
|---|---:|---:|---:|---:|
| Core/pre-resolved | 52.25 | 19,139,977 | 100 | 200 |
| String routing | 48.91 | 20,446,413 | 100 | 200 |
| Dispatcher | 120.82 | 8,277,004 | 100 | 300 |
| Dispatcher + Monitor | 168.91 | 5,920,371 | 200 | 300 |
| Dispatcher + Async Logger | 622.32 | 1,606,899 | 400 | 1,900 |
| Dispatcher + Async Journal | 374.47 | 2,670,453 | 300 | 2,100 |
| Full V2 | 3,976.93 | 251,450 | 2,800 | 7,300 |

每个进程内每层使用 60,000 次预热、60,000 次测量、5 个独立 fixture 的 batch 中位数。五轮中 pre-resolved 为 47.05–94.46 ns，而字符串路由为 48.26–50.87 ns；这说明前者“反而更慢”尚不能解释为代码因果，当前 Windows Balanced、未绑核环境存在明显调度/频率噪声。Full V2 同样为 832.79–4,234.44 ns。后续比较必须固定 CPU/电源策略并保存逐轮结果；异步场景不可用单次结果跨机器比较。

## 结果解释模板

```text
问题：哪个场景、哪个调用栈或硬件指标构成瓶颈。
修改：具体数据结构或热路径变化。
结果：同环境下 Batch、Median、P99 和硬件计数器变化。
代价：内存、容量、复杂度或适用范围变化。
```

没有相同 commit、编译参数和机器环境的数字，不做优化前后百分比比较。
