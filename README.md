# 低延迟撮合引擎工程

这是一个面向量化开发学习、订单簿回测和确定性撮合研究的 C++17 项目。

项目以开源仓库 [voyager-jhk/DeterministicMatchingEngine](https://github.com/voyager-jhk/DeterministicMatchingEngine) 为起点。原项目提供了整数价格、订单对象池、同价 FIFO 队列和价格优先撮合等教学骨架。本仓库在保留 MIT 许可证和来源说明的前提下，对它进行了跨平台修复、正确性补强、热路径改造和最小架构重构。

本项目不把“在开源代码上继续开发”包装成从零原创。它想展示的是另一种同样重要的能力：读懂已有系统，找出宣传、实现和真实场景之间的差距，用测试约束改动，再把原型整理成结构清楚、可以运行、可以解释的工程。

## 当前定位

当前仓库是一个用于量化开发学习的 C++17 交易基础设施项目。V1 提供单品种、单写者、进程内运行的确定性限价订单簿与撮合核心；V2 保留该核心，在外层增加多品种路由、统一事件、Journal、Audit、Replay、异步日志、行情校验、简化二进制 feed handler、容量指标、盘前风控和监控。它适合：

- 学习交易所撮合的价格优先和同价 FIFO；
- 给量化回测提供可控的撮合内核；
- 研究对象池、固定容量索引、价格层和尾延迟；
- 学习 Command、EngineEvent、Replay 和 Snapshot 的边界；
- 作为量化开发实习项目继续拆解和扩展。

它不是可以直接管理真实资金的交易所生产系统。当前没有真实交易所网络接口、账户资金与持仓系统、完整盘前风控、生产级 WAL、高可用复制或并行撮合分片。

## Version 1 与 Version 2

| 范围 | Version 1 | Version 2 |
|---|---|---|
| 核心职责 | `OrderBook`、Price Ladder、价格时间优先撮合、撤单 | 保留 V1 核心并提供系统外围能力 |
| 品种 | 单个 `OrderBook` | `MarketManager` 管理 AAPL、TSLA、NVDA 等独立订单簿 |
| 输入输出 | Command 与 EngineEvent | 带 timestamp/type/symbol/InstrumentId/payload 的统一 SystemEvent |
| 恢复 | Command Replay、进程内 Snapshot | 多品种 Journal Replay 与结果校验 |
| 可观测性 | 状态、invariant、微基准 | Audit、Async Logger、Monitor、数据质量事件 |

V2 的完整架构、运行方式和模块说明见 [README_V2.md](README_V2.md)。根 README 是当前仓库的统一入口，V1 文档和历史性能结果保留用于说明演进过程，不代表尚未提供 V2。

## 最终版本解决了什么

### 撮合与业务规则

- 基础限价买单和卖单；
- 价格优先、同价 FIFO；
- 完全成交、部分成交和跨多个价格档成交；
- 按订单 ID 撤单；
- tick、lot、价格范围和最大单笔数量校验；
- 重复订单 ID、非法字段、容量不足和不存在订单的明确拒绝结果。

### 确定性架构

- 输入命令与输出事件分离；
- 统一入口 `ProcessResult process(const Command&)`；
- `CommandSequence` 管理命令准入顺序；
- `EventIndex` 标记同一命令产生的事件顺序；
- `PrioritySequence` 独立保证同价 FIFO；
- 核心撮合顺序不再依赖系统时间或成交数量。

当前输入只有：

- `NewOrderCommand`；
- `CancelOrderCommand`。

当前输出事件只有：

- `TradeEvent`；
- `OrderRestedEvent`；
- `OrderCancelledEvent`；
- `OrderRejectedEvent`。

没有为了“看起来完整”增加 `OrderAcceptedEvent`。一笔新单如果全部主动成交，本身不需要额外的接受事件；如果产生剩余挂单，`OrderRestedEvent` 已经表达了实际状态变化。

### Replay 与 Snapshot

- Replay 输入是明确的 `std::vector<Command>`，不再从混合事件里猜输入命令；
- 在全新引擎中重执行 Command，并逐字段比较全部 `EngineEvent`；
- 比较全部价格档、每档 FIFO、原始/剩余数量、最优价、活跃订单数和最后命令序列；
- Replay 接口内部同时检查原始引擎和重放引擎的 `check_invariants()`；
- `state_hash()` 作为辅助校验，不是唯一正确性依据；
- 支持轻量的进程内 Snapshot，可恢复订单池、索引、价格档、FIFO、BBO 和最后序列；
- 单 OrderBook Snapshot 可写入版本化二进制文件，包含显式字段、snapshot sequence 和 checksum，并通过临时文件校验、持久化刷新和原子替换保护上一份有效文件；
- Snapshot 恢复前完整校验，失败返回明确原因且不暴露半恢复状态。

V2 进一步使用“同代 Book 文件先落盘、manifest 最后原子提交”的方式保存多品种一致性切面，并可只回放 snapshot sequence 之后的 Journal。当前 Journal 仍是教学用途的 CSV 格式，尚不宣称生产级 WAL 或高可用崩溃恢复。

### 数据结构与工程验证

- 固定容量订单对象池；
- 固定容量、开放寻址的订单 ID 索引；
- 后移删除，避免长期撤单后墓碑堆积；
- 共享价格层对象池；
- 订单保存所属价格层指针，撤单可直接摘链；
- `PriceLadder` 加速配置区间内的已知价格层访问；
- `std::map` 仍作为完整有序价格目录和范围外回退；
- 完整订单簿状态模型、状态哈希和内部不变量检查；
- Release CTest、随机属性测试和 MSVC AddressSanitizer 验证。

## 一条命令怎样流过引擎

```text
NewOrderCommand / CancelOrderCommand
                 │
                 ▼
        CommandSequence 准入检查
          │                  │
      失败│                  │通过
          ▼                  ▼
ProcessStatus::       参数与业务规则检查
SEQUENCE_REJECTED        │           │
无事件、无状态变化      失败         通过
                         ▼           ▼
                  OrderRejected   撮合 / 撤单 / 挂单
                  Event           │
                                  ▼
                  Trade / Rested / Cancelled Event
```

准入拒绝和业务拒绝是两件不同的事：

- sequence 为 0、重复或倒退属于输入准入失败，只返回 `SEQUENCE_REJECTED`，不产生事件、不推进最后序列；
- 命令通过 sequence gate 后，即使因为重复订单 ID、非法价格或找不到撤单目标而业务拒绝，也会产生 `OrderRejectedEvent`，并推进最后序列；
- 一个命令产生多个事件时，`EventIndex` 从 0 连续递增。

## 快速构建

### Windows + Visual Studio 2022

建议在 **Developer PowerShell for VS 2022** 中执行：

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

分别运行：

```powershell
.\build\Release\matching_engine_demo.exe
.\build\Release\matching_engine_unit_tests.exe
.\build\Release\matching_engine_property_tests.exe
.\build\Release\matching_engine_snapshot_tests.exe
.\build\Release\matching_engine_benchmarks.exe
```

### Linux / WSL + GCC 或 Clang

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/matching_engine_demo
```

### MSVC AddressSanitizer

```powershell
cmake -S . -B build-asan -G "Visual Studio 17 2022" -A x64 -DENABLE_ASAN=ON
cmake --build build-asan --config RelWithDebInfo --parallel
ctest --test-dir build-asan -C RelWithDebInfo --output-on-failure
```

如果系统找不到 `clang_rt.asan_dynamic-x86_64.dll`，请从 Developer PowerShell 启动，或把当前 Visual Studio MSVC 的 `Hostx64\x64` 运行库目录加入本次终端的 `PATH`。Sanitizer 构建用于查内存问题，不用于性能比较。

## 当前测试状态

当前 V2 代码已经通过：

| 验证层 | 当前内容 |
|---|---|
| Unit Test 可执行文件 | 21 个固定测试函数，覆盖撮合、撤单、拒绝、事件顺序、Sequence、Replay、索引和池化 |
| Property Test 可执行文件 | 5 组随机/属性测试 |
| Differential Test 可执行文件 | 25 个固定种子 × 4,000 条命令，逐命令比较状态、事件和完整盘口 |
| Snapshot Test 可执行文件 | 5 组恢复测试，包含继续执行、非法快照原子失败、磁盘往返、截断与 checksum 损坏检测 |
| Demo Integration | 构建订单簿、跨档成交、撤单和 Command Replay |
| V1 + V2 Release CTest | 20/20 通过（2026-08-07 本机复验） |
| MSVC ASan RelWithDebInfo CTest | V1 阶段曾完成 4/4；V2 全量 ASan 尚需重新固定基线 |

2026-08-07 的现有 Release 构建复验为 20/20 CTest 通过。历史 Release `0.74 s` 和 ASan `3.80 s` 是 V1 阶段结果，不与当前 V2 测试耗时混用。

`.github/workflows/ci.yml` 配置了 Linux Release、ASan+UBSan、外围异步模块 TSan 和缩小负载的 V2 benchmark smoke。共享 CI 的 benchmark 只验证程序可运行，不作为正式延迟数据。工作流需要推送后由 GitHub runner 实际验证；本节的 20/20 仅代表本机 MSVC Release 结果。

## 性能结论：怎样理解“30 ns”

性能优化阶段保留了原始数据，位于 `benchmarks/results/`。下表对应提交 `e4d59e2`，环境为 Intel Core i7-14650HX、MSVC 19.44、x64 Release、Windows Balanced 电源计划、线程未绑核、关闭事件记录。每个值是“5 个全新进程 × 每进程 7 次”的进程中位数再取中位数。

| 关闭事件的批量微基准 | 优化前 | 优化后 | 结论 |
|---|---:|---:|---|
| 同价已有价格层挂单 | 44.559 ns/order | 26.663 ns/order | 进入 30 ns 内，但只代表最短挂单场景 |
| 多个已有价格层挂单 | 43.876 ns/order | 27.104 ns/order | 进入 30 ns 内，仍是批量微基准 |
| 新建价格层挂单 | 79.730 ns/order | 73.090 ns/order | `std::map` 节点仍是主要成本 |
| 立即成交 | 43.838 ns/order | 24.988 ns/order | 进入 30 ns 内，不含外部链路 |
| 一笔扫四档 | 200.090 ns/order | 186.440 ns/order | 多次成交和跨档访问成本明显更高 |

这些数据说明：在特定预热、批量、关闭事件的窄场景里，项目已经能够靠近或低于 30 ns；但不能据此宣称“撮合引擎稳定 30 ns”，更不能说端到端交易链路是 30 ns。网络收包、协议解析、风控、事件发布、日志持久化、线程调度和真实订单分布都没有包含在表内。

之后的架构 Sprint 增加了 Command/Event 分离、Sequence gate、完整 Replay 验证和 Snapshot。为了遵守“停止追纳秒、先把结构讲清楚”的目标，没有在最终提交 `9e43b8e` 上重新执行同口径性能矩阵。因此上表是可追溯的性能阶段结果，不冒充最终架构版本的新测数据。

## 当前明确没有实现

- Market、FOK、Replace 和自成交保护（IOC 与 Post Only 已实现）；
- 账户资金、持仓、交易时段、手续费和完整风控；
- 多线程撮合分片和 NUMA 部署；
- 真实交易所网络协议、组播订阅、重传通道和行情发布；
- 带校验和、版本头、fsync 策略和 fail-stop 语义的生产级 WAL；
- 生产级二进制 WAL、高可用复制与完整断电恢复；
- Reference Engine、Differential Testing 和大规模故障注入；
- 完全无内部临时事件缓冲的撮合实现；当前 V2 批次会在命令后清空，V1 兼容诊断捕获严格限制在构造时预留容量内；
- 完全消除 `std::map` 节点分配。

CSV 的 `save_log/load_log` 只用于 `EngineEvent` 的可读诊断往返，不是 Replay 输入，也不是持久化恢复协议。

V2 已实现可重放的 CSV Journal，但只保证正常关闭时排空与 flush；它不具备断电一致性、每批 fsync 或生产级 WAL 语义。V2 的合成行情也只用于确定性测试，不是真实交易所数据。

## 主要目录

```text
src/
  types.hpp                 强类型、方向和价格缩放
  commands.hpp              NewOrderCommand / CancelOrderCommand
  events.hpp                四种 EngineEvent 和拒绝原因
  order.hpp                 订单、同价链表和订单对象池
  book_state.hpp            可逐字段比较的完整订单簿状态
  instrument_config.hpp     tick、lot、价格和数量规则
  fixed_order_index.hpp     固定容量订单 ID 索引
  limit_level_pool.hpp      共享价格层对象池
  price_ladder.hpp          配置区间内的快速价格层表
  price_level_store.hpp     完整有序价格目录
  orderbook.hpp             统一入口、撮合、撤单和状态审计
  replay.hpp                Command 重执行验证和 CSV 事件诊断
  snapshot.hpp              轻量内存 Snapshot 数据模型
  snapshot_recovery.hpp     Snapshot 校验与恢复
  snapshot_file.hpp         版本化磁盘格式、checksum 与原子替换
  market_snapshot.hpp       多品种代际快照与 manifest 提交
  main.cpp                  可运行演示

tests/
  unit_tests.cpp            21 个固定测试函数
  property_tests.cpp        5 组属性/随机测试
  differential_tests.cpp   Reference Engine 差分测试
  snapshot_tests.cpp        5 组 Snapshot 测试
  market_snapshot_tests.cpp 多品种快照与增量 Journal 恢复

benchmarks/
  perf.cpp                  延迟、吞吐和压力基准
  results/                  性能阶段的原始结果与验证元数据
```

## 文档阅读顺序

1. [原项目框架与问题分析](docs/01-原项目框架与问题分析.md)：原项目做了什么，哪些思路值得保留，哪些宣传需要重新验证。
2. [从原项目到当前版本的改造报告](docs/02-从原项目到当前版本的改造报告.md)：按阶段记录全部主要改动、效果和失败实验。
3. [Benchmark 与性能证据](docs/BENCHMARK.md)：说明测量边界、Linux perf/VTune 采集方法和结果解释规范。
3. [当前项目的后续改进路线](docs/03-当前项目的后续改进路线.md)：只列当前仍未完成的问题、选择理由和验收标准。
4. [新项目完整介绍与使用说明](docs/04-新项目完整介绍与使用说明.md)：从金融概念、最终架构到代码调用、Replay 和 Snapshot 的完整说明。

## 许可证与致谢

本项目沿用 MIT License。感谢原作者提供可以继续学习和验证的开源起点。

本仓库新增和重构的主要内容包括：Visual Studio/CMake 兼容、交易规则校验、固定订单索引、价格层池化、撤单直达、热路径实验、Command/Event 分离、确定性 Sequence、Command Replay 全量验证、轻量内存 Snapshot、内部不变量、测试体系和完整文档。
