# M0：当前提交 Benchmark 基线

日期：2026-11-11（Asia/Tokyo）；源码 `c690dfd8b008e06690e914e272173a7f4e55d164`。这是当前已有窄负载的观测，不是优化收益或正式端到端 SLA。

## 1. 原始证据与采集命令

目录：[2026-11-11-m0-baseline](../../benchmarks/results/2026-11-11-m0-baseline/)：五个独立进程的 `raw_run_01..05.txt`、`environment.json`、`summary.csv`、`checksums.sha256`。汇总35条数据，七个被哈希文件全部匹配。

Release 构建命令见 [测试报告](baseline_test_report.md)。采集使用现有脚本，未更改 benchmark 源码：

```powershell
$env:MATCHING_ENGINE_RAW_OUTPUT = '1'
powershell -NoProfile -ExecutionPolicy Bypass -File ./scripts/run_benchmark_windows.ps1 -Executable 'build/m0-release/matching_engine_v2_benchmarks.exe' -OutputDirectory 'benchmarks/results/2026-11-11-m0-baseline' -Runs 5
```

首次直接调用脚本被本机 execution policy 拒绝，未产生该目录；第二次使用仅新进程生效的 Bypass 成功，未修改系统策略。必须显式设置 RAW_OUTPUT：`c690dfd` 的程序默认只输出人读结果，现有脚本却只解析 RESULT_CSV，未设置时可能得到只有表头的汇总。这是待修复采集脚本缺陷，不以空表冒充成功。

SHA-256 验证（PowerShell，不要求另装 sha256sum）：

```powershell
$directory = 'benchmarks/results/2026-11-11-m0-baseline'
Get-Content "$directory/checksums.sha256" | ForEach-Object {
    $parts = $_ -split '  ', 2
    $actual = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $directory $parts[1])).Hash.ToLowerInvariant()
    if ($actual -ne $parts[0]) { throw "Hash mismatch: $($parts[1])" }
}
```

现有 `.gitattributes` 对 `benchmarks/results/** -text -diff` 保存字节；历史2026-08-07证据本轮也7/7匹配，不重新生成旧哈希。

## 2. 环境与测量边界

- i7-14650HX，16核/24线程；Windows 11 `10.0.26300`；MSVC19.44.35228、CMake4.4.2/Ninja1.13.2、C++17、x64 Release `/O2 /DNDEBUG /MD`。
- Balanced 电源计划；未绑核；未固定频率、未控制后台负载，审计过程中仍有普通文件读取等活动。不能做跨机器或严格 A/B 结论；ASan 构建在此五轮完成后才开始。
- 负载来自 `benchmarks/v2_perf.cpp:192`：三品种交错，每次同价 BUY1 再 Cancel，共60,000命令；不是成交负载、不是开盘突发、不是校准真实数据。没有 PRNG；输入由循环和基线源码定义，未保存独立命令文件。
- 每场景一个60,000命令 warmup，再5个独立 fixture 的批量测量，取批量中位数；另一个 fixture 逐操作 steady_clock 采样60,000次。脚本启动5个独立进程。
- 程序标注 `Process Runs: 5` 实际是同一进程内批次，不是独立进程；真正进程数由采集脚本给出。
- Core 场景仍经过 MarketManager 的预解析 InstrumentId 路径并生成事件到 NullEventSink，不等于历史关闭事件的 V1 极窄微基准。
- 计时在 `run()` 返回即结束，`stop()` 排空/flush 在计时之外；日志场景测的是交易线程处理/入队成本及竞争，不是持久化完成延迟。没有公开 Feed 的发送/接收链路。
- 结果 checksum 只折叠命令 sequence、事件数量和处理状态，不是完整盘口/成交内容哈希；也不验证异步日志接受数、写入成功数或 durable 状态。

## 3. 实测结果

batch栏为“五个进程各自五批的中位 ns/op，再取进程中位数”；P99/P99.9栏是五个采样分位值的中位数，不是合并样本分位数；Max是五个进程最坏采样值。

| 场景 | batch中位 ns/op | 进程batch范围 ns/op | 采样P99 ns | 采样P99.9 ns | 最坏采样Max ns |
|---|---:|---:|---:|---:|---:|
| core_pre_resolved_instrument | 48.78 | 47.80–61.95 | 200 | 300 | 23,000 |
| core_plus_routing | 51.53 | 50.89–54.05 | 200 | 300 | 52,800 |
| dispatcher | 124.74 | 122.48–134.04 | 200 | 300 | 114,400 |
| dispatcher_monitor | 166.24 | 157.52–186.62 | 300 | 600 | 292,200 |
| dispatcher_async_logger | 581.80 | 561.84–600.54 | 3,200 | 55,600 | 493,800 |
| dispatcher_async_journal | 388.19 | 381.47–397.28 | 1,700 | 18,400 | 261,300 |
| full_v2 | 950.50 | 904.38–988.47 | 3,900 | 61,100 | 777,000 |

完整 P50、吞吐和每轮数据在 summary.csv。例如 full_v2 进程吞吐约1.01–1.11M命令/s，仅适用于上述闭环挂单/撤单负载。100ns级采样阶梯及环境抖动意味着不能从这份测量推导单操作稳定几十纳秒。

## 4. 性能门禁与未测指标

本轮没有 P95、开环到达/排队延迟、分配计数、cycles/instructions/cache-misses、RSS/峰值、置信区间、持久化/公开行情端到端延迟。闭环样本可能遗漏外部排队，不能证明没有 coordinated omission。

M1 的硬门禁先为正确性：事件和状态确定、边界回归通过；不设置“稳定30ns”等阈值。数值回归阈值须在固定命令文件、绑核/频率与背景负载记录、可靠输出成功检查、足够重复及分层时延后制定。此基线为之后同口径再测保留证据，不足以定生产 SLA。

后续只增量增强采集器（强制 machine output、校验行数与异步健康度、改正批次标签）；保留当前原始文件不覆盖。未执行 Linux perf、真实数据校准或吞吐优化。
