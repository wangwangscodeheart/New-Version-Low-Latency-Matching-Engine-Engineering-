# 低延迟撮合引擎工程

这是一个面向量化开发学习、历史订单回放和撮合机制研究的 C++17 限价订单簿项目。

我不是从一张白纸开始写这个项目。它最初建立在开源项目
[voyager-jhk/DeterministicMatchingEngine](https://github.com/voyager-jhk/DeterministicMatchingEngine)
的代码和思路之上。原项目提供了一个很好的教学骨架：用整数保存价格、用订单池保存订单、用双向链表维护同价位订单、按照价格优先和时间优先完成撮合，并通过事件日志做回放演示。

在实际阅读、编译、测试和压测后，我发现原项目更适合作为“思路演示”，还不能直接当作成熟的低延迟撮合工程使用。它的宣传数字、跨平台构建、拒单处理、订单索引、价格层生命周期、磁盘回放、真实交易规则和测试完整度之间存在明显差距。因此，我以“先保证账本正确，再改善性能，最后补真实交易场景”为原则，对项目进行了持续重构。

当前仓库保留了原项目的 MIT 许可证和 Git 历史，也明确说明了继承关系。我的重点不是把原项目包装成原创，而是展示我如何阅读一个已有系统、识别问题、设计改进方案、编写测试并逐步把它变成更可靠的工程。

## 建议阅读顺序

项目说明拆成四份专题文档，建议按顺序阅读：

1. [原项目框架与问题分析](docs/01-原项目框架与问题分析.md)

   介绍原项目如何组织订单、如何撮合、哪些设计值得保留，以及代码和宣传之间有哪些差距。

2. [从原项目到当前版本的改造报告](docs/02-从原项目到当前版本的改造报告.md)

   按阶段说明每一项修改：原来的不足、修改思路、代码变化、带来的优势和仍然存在的限制。

3. [当前项目的后续改进路线](docs/03-当前项目的后续改进路线.md)

   说明哪些问题还没有解决、为什么不能只追求 30ns，以及后续如何继续提高正确性、性能和实际应用价值。

4. [新项目完整介绍与使用说明](docs/04-新项目完整介绍与使用说明.md)

   从使用者角度介绍当前新项目的总体架构、关键数据结构、订单处理流程、回放方式、测试方法、构建步骤和性能口径。

## 当前项目能做什么

目前已经实现并验证：

- 单品种限价订单簿；
- 买卖盘价格优先；
- 同价位时间优先（FIFO）；
- 完全成交和部分成交；
- 主动订单跨多个价格层成交；
- 按订单 ID 撤单；
- 重复订单 ID、非法价格、非法数量和容量耗尽拒单；
- 每个品种独立配置价格最小变动单位、最小交易数量、价格范围和订单数量上限；
- 固定容量订单池、固定容量订单 ID 索引和价格层池；
- 内存事件回放；
- CSV 日志保存、严格加载和确定性恢复；
- 完整订单簿状态哈希；
- 完整账本不变量审计；
- 单元测试、随机属性测试、CTest 和 MSVC AddressSanitizer；
- 吞吐、延迟、撤单、价格层变化、内存和百万订单压力测试。

## 当前项目还不能做什么

这不是一个可以直接连接交易所并管理真实资金的生产系统。目前尚未实现：

- Market、IOC、FOK、Post Only 和 Replace 等完整订单类型；
- 自成交保护、账户资金、持仓和撮合前风险控制；
- 多品种路由和多线程分片；
- L2/L3 行情接入、序列号和丢包恢复；
- 二进制预写日志、快照和崩溃恢复；
- 固定容量事件环；
- 配置价格区间内完全绕过 `std::map` 的连续价格索引；
- Linux 裸机 CPU 隔离、NUMA 和用户态网络接入。

因此，当前最准确的定位是：

> 一个经过较大幅度工程化改造、能够运行和验证的单线程确定性限价订单簿核心，用于量化开发学习、撮合机制研究和后续回测系统建设。

## 当前性能口径

以下数据来自本机 Windows、Visual Studio 2022、x64 Release 构建。不同 CPU、电源模式、系统调度和测试负载会产生明显差异。

| 测试项目 | 当前大致结果 | 应该如何理解 |
| --- | ---: | --- |
| 关闭事件记录的核心批量路径 | 多轮中位数约 40～50 ns/order | 本轮 41.597 ns；进程内微基准，不包含网络、协议和风控 |
| 单轮批量范围 | 本轮约 38.662～51.169 ns/order | 最低单轮不能当作稳定承诺 |
| 取消订单 | 多数约 18～30 ns/order | 普通订单取消；清理空价格层时仍会访问有序索引 |
| 完整路径 P99 | 数百纳秒级 | 包含事件日志写入 |
| 完整路径 P99.9 | 约 1～3 微秒 | 会受到 Windows 调度和内存分配影响 |
| 百万订单压力吞吐 | 约 640～680 万 orders/sec | 压力测试结束后执行完整账本审计 |
| 高频价格层创建/销毁 | 多轮约 66～86 ns/轮 | 本轮 85.78 ns；主要瓶颈仍是 `std::map` 节点分配和维护 |

本项目不再宣传“稳定 30ns”。当前更严谨的说法是：部分关闭事件记录的批量核心路径可以进入低 40ns，个别单轮略低于 40ns，但完整系统延迟远高于这个数字，而且端到端实盘延迟还需要加入网络、协议解析、风控和回报发送。

本轮逐笔计时在 Windows 上出现过 `P50 = 0 ns`，这不表示订单真的不耗时间，而是单次操作短于计时器可见粒度。所以上表把批量 100,000 笔、重复 7 轮后的平均值作为核心路径主要参考，逐笔测试主要用来观察 P99/P99.9 尾部。

## 快速构建

### Windows + Visual Studio 2022

建议在 **Developer PowerShell for VS 2022** 中执行，或先确保 `cmake` 已加入 `PATH`。

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

运行程序：

```powershell
.\build\Release\matching_engine_demo.exe
.\build\Release\matching_engine_unit_tests.exe
.\build\Release\matching_engine_property_tests.exe
.\build\Release\matching_engine_benchmarks.exe
```

### Linux + GCC/Clang

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

### AddressSanitizer

```powershell
cmake -S . -B build-asan -G "Visual Studio 17 2022" -A x64 -DENABLE_ASAN=ON
cmake --build build-asan --config RelWithDebInfo --parallel
ctest --test-dir build-asan -C RelWithDebInfo --output-on-failure
```

## 主要目录

```text
src/
  types.hpp                 强类型、价格缩放和方向定义
  order.hpp                 订单对象、订单池和同价位链表
  fixed_order_index.hpp     固定容量订单 ID 索引
  instrument_config.hpp     品种 tick、lot、价格和数量规则
  limit_level_pool.hpp      固定容量价格层池
  price_ladder.hpp          配置价格区间内的快速价格层指针表
  price_level_store.hpp     有序价格索引与价格层所有权边界
  events.hpp                命令结果与成交事件
  orderbook.hpp             撮合、撤单、拒单和账本审计核心
  replay.hpp                日志保存、严格加载和确定性回放
  main.cpp                  演示程序

tests/
  unit_tests.cpp            功能、拒单、池化和磁盘恢复测试
  property_tests.cpp        随机属性、FIFO和回放一致性测试

benchmarks/
  perf.cpp                  延迟、吞吐、撤单、内存和压力测试

docs/
  01-原项目框架与问题分析.md
  02-从原项目到当前版本的改造报告.md
  03-当前项目的后续改进路线.md
  04-新项目完整介绍与使用说明.md
```

## 测试状态

当前阶段已经通过：

- 19 项单元测试；
- 5 组随机属性测试；
- 3 个 CTest 入口；
- MSVC AddressSanitizer 单元测试和属性测试；
- Demo 确定性回放；
- 百万订单压力测试和终态账本审计。

测试通过只能说明当前覆盖场景没有发现错误，不代表项目已经达到交易所生产标准。仓库文档会持续明确“已经验证的内容”和“尚未实现的内容”。

## 许可证与致谢

本项目基于 MIT 许可证开源项目进行学习和重构，保留原许可证与历史记录。感谢原作者提供订单簿、对象池、事件回放和基准测试的初始框架。

本仓库新增内容主要集中在跨平台构建、输入规则、拒单语义、固定索引、价格层池化、回放完整性、账本审计、测试体系、Sanitizer 验证和性能口径修正。
