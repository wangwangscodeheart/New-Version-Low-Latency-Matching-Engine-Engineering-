# Benchmark 与性能证据

## 测量边界

- V1 benchmark：OrderBook 核心及场景矩阵。
- V2 benchmark：预解析 InstrumentId、字符串路由、冻结 Dispatcher、Monitor、Async Logger、Async Journal 和 Full V2 的逐层开销。
- Batch Average 用于比较吞吐；逐笔采样用于观察 Median、P99、P99.9 和 Max。两者不能混为同一口径。

benchmark 输出包含 commit、CPU、操作系统、编译器、有效编译参数、Build Type、负载规模和 checksum。正式结果必须来自 Release 构建和固定环境；CI smoke 只证明程序可以运行。

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

2026-08-07 在 Windows 10、Intel Family 6 Model 183、MSVC 19.44、Release `/O2` 下运行。基准元数据 commit 为 `7b4de2ee75ce`；它不是 CI 或生产延迟承诺。

| 场景 | Batch avg ns | Throughput/s | Sample median ns | P99 ns |
|---|---:|---:|---:|---:|
| Core/pre-resolved | 61.03 | 16,384,937 | 100 | 200 |
| String routing | 51.29 | 19,496,978 | 100 | 200 |
| Dispatcher | 125.39 | 7,974,800 | 100 | 300 |
| Dispatcher + Monitor | 174.53 | 5,729,783 | 200 | 400 |
| Dispatcher + Async Logger | 575.35 | 1,738,072 | 400 | 1,800 |
| Dispatcher + Async Journal | 379.83 | 2,632,780 | 1,400 | 3,500 |
| Full V2 | 4,129.91 | 242,136 | 2,700 | 8,300 |

每层使用 60,000 次预热、60,000 次测量、5 个独立 fixture 的 batch 中位数。异步场景受后台线程调度和磁盘缓存影响，单次结果不可用于跨机器比较。

## 结果解释模板

```text
问题：哪个场景、哪个调用栈或硬件指标构成瓶颈。
修改：具体数据结构或热路径变化。
结果：同环境下 Batch、Median、P99 和硬件计数器变化。
代价：内存、容量、复杂度或适用范围变化。
```

没有相同 commit、编译参数和机器环境的数字，不做优化前后百分比比较。
