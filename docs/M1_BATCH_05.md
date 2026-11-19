# M1 第五批：新价格档位分配失败与结构清理

归档：2026-11-14（Asia/Tokyo）。在原仓库增量修改，保留V1撮合算法及前四批成果。本批不增加业务功能，不宣称M1或V2.1全部完成。

后续：[M1第六批](M1_BATCH_06.md)已处理可选模拟风控的整数极值及累计量失效；下文剩余风险列表保留本批归档口径。

## 1. 实际复现的问题与最小修复

新单先从OrderPool取对象并加入OrderIndex，再撮合，最后把剩余量挂入价格层。新价格的`std::map`节点分配可能抛出`std::bad_alloc`。原有PriceLevelStore已归还刚取得的空Level槽，但尚未挂簿的主动单仍留在索引里：池/索引显示有订单，盘口却没有对应FIFO节点。

两条修复前实际失败记录分别为：

- [空簿新价格](validation/2026-11-14-m1-allocation/red-new-level.txt)：`indexed=1 free_levels=4`，清理断言失败。
- [部分成交后挂剩余量](validation/2026-11-14-m1-allocation/red-partial-match.txt)：`indexed=1 retained_trades=2 delivered=0`，同一清理断言失败。

修复仅在`add_to_book(order)`异常出口检查`parent_level`：若尚未挂链，删除该订单的索引并归还Order槽，然后重新抛出。已链接订单不在此处擅自释放。PriceLevelStore原有的Level归还职责不变；外围`process`继续置为FAILED。

| 文件 | 本批修改 | 设计原因 / 真实系统对应 |
|---|---|---|
| `src/orderbook.hpp` | 新价格挂单异常时清理未挂链主动单 | 明确索引、订单池、价格层之间的所有权；异常出口不能留下孤儿订单 |
| `tests/allocation_failure_tests.cpp` | 独立可执行文件内替换new/delete，定点注入分配失败 | 资源耗尽故障测试，不向撮合生产代码加入测试开关 |
| `CMakeLists.txt` | 注册并安装新测试程序，CTest增加到23项 | 故障回归进入现有构建流程，不引入新框架 |

没有更换Price Ladder、FIFO、对象池、索引或撮合循环，没有全局生产allocator改造。

## 2. ADR-011：结构清理不是撮合回滚

状态：上述新价格节点分配故障路径已实现并验证。

部分成交后才发生分配失败时，先前成交已经修改被动单/盘口。这次修复**不回滚成交**，也不把异常伪装成普通业务拒单：

```text
新单取槽并入索引 -> 已完成若干成交 -> 新价格节点分配失败
 -> 清理未挂链主动单 -> 抛异常 -> Book/Engine FAILED
```

保留的TradeEvent仅用于进程内诊断；异常命令没有开始向Sink交付完整批次，`last_applied_command_sequence`仍为上一条完成命令。结构不变量可以重新成立，但这不表示本条命令提交成功、当前状态可快照或系统可以继续接单。

FAILED时仍禁止新命令、事件自动续送和快照；Engine的标准CommandJournal本次begin会abort，不会commit。不能把同一条命令直接重投到这个部分改变的实例。停机后应从独立可信的恢复基准另建实例，并显式处理未确认命令；本批没有新增自动恢复、durable prefix或Exactly-Once保证。

这对应交易基础设施里的资源所有权清理与fail-closed边界，而不是数据库式事务回滚。

## 3. 定点注入范围与实际验证

独立测试程序包含七组：

1. PriceLevelStore节点分配失败时归还空Level槽，不破坏已有档位；解除故障后可重试/删除。
2. 空簿新价格挂单失败，覆盖BUY/SELL及record_events开关；无孤儿索引，结构有效且FAILED。
3. BUY/SELL跨两档部分成交后，剩余量挂单失败；保留两笔诊断成交，Sink收到0条，序号不前进。
4. 已缓存档位添加订单及撤单，在武装故障状态下没有调用operator new。
5. 部分成交失败不破坏其他原有挂单，双方向对照完整剩余盘口。
6. 容量4时3笔成交加IOC余量撤销达到4事件上界；容量1的Sink背压及逐后缀续送均不分配，预留容量不变。
7. Engine在ORDER回调后精确武装故障；标准CommandJournal begin一次、abort一次、commit零次；Engine及Book停止，其他symbol的新单也被拒绝。

故障钩子只作用于独立测试程序的当前线程，命中一次后即解除，断言/诊断前也会解除。覆盖scalar/array、sized、aligned、nothrow形式；Windows对齐分配使用配对的`_aligned_malloc/_aligned_free`。没有把这些替换链接进Demo、其他测试或Benchmark。

| 验证 | 最终结果 | 记录 |
|---|---|---|
| 全新MSVC Release / Ninja | 23/23 CTest通过，2.07s | [Release日志](validation/2026-11-14-m1-allocation/release-ctest.txt) |
| 全新MSVC ASan RelWithDebInfo | 23/23 CTest通过，8.20s | [ASan日志](validation/2026-11-14-m1-allocation/asan-ctest.txt) |
| 独立故障程序 | 七组通过，空簿/部分成交后的indexed均为0 | [最终绿灯](validation/2026-11-14-m1-allocation/allocation-green.txt) |
| 独立参考差分 | 100种子×10,000命令通过，generator=enum_boundary_v2 | [100万命令](validation/2026-11-14-m1-allocation/differential-stress.txt) |

两套新构建的全部52个编译/链接步骤成功；补充第五组后重新编译故障程序，再次执行上表两套全量CTest。红灯记录来自补生产清理之前，不将先后扩展过的整个测试文件称为逐字节同一版本。LastTest.log混有UTF-8程序输出及CP936本地时区标签；证据文本逐行识别这两种编码后转录为UTF-8、统一换行，不冒充原始字节副本。构建目录中仍保留原始LastTest.log。

ASan限制：用户替换new/delete可能降低依赖ASan自带分配函数的诊断覆盖。因此这个故障程序证明的是定点异常、结构清理与停止行为，不能声称保留了全部分配族诊断能力。其余22个测试程序没有这些替换。[Microsoft官方限制说明](https://learn.microsoft.com/en-us/cpp/sanitizers/asan-known-issues?view=msvc-170#overriding-operator-new-and-delete)。未添加特殊链接选项或ASan屏蔽。

本轮未执行Linux Release、UBSan、TSan、真实物理内存耗尽、磁盘满或进程强杀。注入不等于遍历所有分配位置/大小/线程，也不覆盖全部legacy历史捕获组合。已有D9025、Benchmark C4244及故障Sink的ASan C4702警告未包装成零警告构建。

VS x64 Developer cmd复现（先设UTF-8，避免中文showIncludes探测问题）：

```bat
chcp 65001
cmake -S . -B build/m1-allocation-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/m1-allocation-release --parallel 8
ctest --test-dir build/m1-allocation-release --output-on-failure
cmake -S . -B build/m1-allocation-asan -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DENABLE_ASAN=ON -DENABLE_UBSAN=OFF
cmake --build build/m1-allocation-asan --parallel 8
ctest --test-dir build/m1-allocation-asan --output-on-failure
build\m1-allocation-release\matching_engine_differential_tests.exe --seeds 100 --commands 10000 --seed-base 0xD1FF0000
```

## 4. 性能观察与源码身份

正常路径只增加异常清理出口；不根据这一点直接断言零开销或没有退化。[本批单进程七场景记录](../benchmarks/results/2026-11-14-m1-allocation-check/)：core 47.10ns/op、routing 47.02ns/op、dispatcher 131.74ns/op、monitor 175.47ns/op、async logger 574.81ns/op、async journal 388.04ns/op、full_v2 4,237.60ns/op。full采样P99=7,300ns、P99.9=231,800ns、max=1,992,600ns。7行汇总完整，3/3证据文件SHA-256匹配。

仍为Windows Balanced、未绑核/固定频率、后台未隔离。仅采集1个新进程；程序显示的Process Runs=5是同进程内部批次，不能冒充5个独立进程。routing偶尔比core小、长尾大等均保留；未做受控交错A/B，不能据此声称优化成功或无性能退化。Benchmark使用正常分配器，不带故障钩子；submit计时不含最终drain/flush，不代表durable latency。

采集沿用`scripts/run_benchmark_windows.ps1`，`MATCHING_ENGINE_RAW_OUTPUT=1`，Executable为`build/m1-allocation-release/matching_engine_v2_benchmarks.exe`，OutputDirectory为`benchmarks/results/2026-11-14-m1-allocation-check`，Runs=1。

基线仍为`c690dfd8b008e06690e914e272173a7f4e55d164`加未提交工作区，环境明确标记dirty。[累计源码patch](validation/2026-11-14-m1-allocation/source_changes.patch)与[28项Git blob身份](validation/2026-11-14-m1-allocation/source_git_blobs.txt)冻结前五批代码；blob按Git入库文本规范化计算，不等于任意checkout换行字节的SHA-256。patch对基线index的`git apply --cached --check`通过，不改动index；不要重复应用到当前工作区。文档补充不改变已测编译输入。

## 5. 回滚、剩余风险与面试介绍

回滚只能选择本批add_to_book异常清理hunk、新测试程序和对应CMake条目，不可整文件还原OrderBook而删除前四批保护；保留历史红/绿灯证据。移除清理后，本批复现用例应重新失败，不能继续保留修复声明。

C02中的新价格map分配故障现已从候选变为真实复现并完成结构清理验证，但不是所有分配异常均获强保证。C01池API误用硬化、C05极值风控、真实磁盘故障/durable prefix、快照generation唯一性、权威簿到公开Feed再到独立本地簿的闭环仍需后续审查或实施。本批不关闭这些事项，不跨过门禁宣称整个M1/V2.1已验收。

面试可介绍：我用独立程序里的确定性分配失败注入，复现了“索引有订单但FIFO没有节点”的异常所有权漏洞；修复只清理未挂簿对象，保留已经发生的成交并停止系统。我区分了结构有效、命令完成和持久化完成，不用一份全绿测试报告声称撮合事务可回滚。
