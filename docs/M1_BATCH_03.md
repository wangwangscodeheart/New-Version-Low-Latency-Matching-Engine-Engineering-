# M1 第三批：整命令 Journal 接受契约

后续：[M1第四批](M1_BATCH_04.md)已补核心直接入口枚举保护，本文保留第三批时的范围和待办作为历史记录。

归档：2026-11-12（Asia/Tokyo客户端日期）。在独立撮合仓库继续增量修改；V1撮合循环及前两批修复保留。原始日志时间按工具输出保存。本批完成**标准接线下的进程内整命令接受边界**，不将它等同断电持久化或生产WAL。

## 1. 源码与组件变化

| 文件 | 修改及设计理由 | 真实组件对应 |
|---|---|---|
| `src/command_journal.hpp` | begin/append_trade/commit/abort接口，独立于普通观察订阅 | 恢复关键输出的事务边界 |
| `src/journal_batch_queue.hpp` | 按记录槽预留，完整vector批次入队，写完释放预算 | 有界异步持久化流水线的进程内接受队列 |
| `src/async_journal.hpp` | 标准模式撮合前预留、整体入队；兼容observer模式失败抛错；worker检测写入/flush错误 | Journal writer，不是普通可丢弃Audit日志 |
| `src/trading_engine.hpp` | 预留在ORDER发布前；Trade先进入stage；Journal提交后再发最终处理结果 | 单写者应用协调与准入 |
| `src/journal.hpp` | 追加format_version，v2标头、完整组验证、尾部整组丢弃，严格unsigned解析及save错误检查 | 版本化可回放输入与完整性语法门禁 |
| `src/replay_v2.hpp` | 新格式在任何订阅/submit之前验证整个输入vector | 防止半条命令先执行后才发现错误 |
| `src/v2_main.cpp`、`benchmarks/v2_perf.cpp` | 改为标准引用接线；Benchmark停止后检查Journal健康 | 运行示例与测量不绕开可靠路径 |
| `tests/command_journal_tests.cpp`、`tests/async_journal_tests.cpp` | 新增12组契约/故障测试；原overflow测试切换标准接线 | 正式确定性回归，不依赖worker调度制造队列满 |
| `CMakeLists.txt`、`.github/workflows/ci.yml` | 新target使CTest21→22；并发测试加入已有TSan配置 | 本机验证及未来runner配置，配置不等于已通过 |

## 2. ADR-009：先预留，再应用，最后整体接受

状态：本批进程内契约已实现；磁盘durable prefix、CRC和同步策略仍待M3。

```text
检查入口 / 单写者屏障
 -> Journal.begin_command：预留完整命令最坏记录预算
 -> 发布ORDER -> 原撮合核心 -> 收集Trade / 正常观察事件
 -> Journal.commit_command：INPUT + TRADE* + COMMAND_RESULT一次入队
 -> 发布处理结果 / 返回
                          ... worker写文件，正常stop排空flush
```

标准接线为：

```cpp
AsyncJournalWriter journal("trade.journal.csv", 4096); // 记录槽预算
TradingEngine engine(markets, dispatcher, journal);  // 引用，不是旧gate指针
// 此模式不要再 journal.attach(dispatcher)。
```

旧第三参数`const SubmissionGate*`仍仅表达可用性检查，不自动记录Journal；旧attach接口保留为observer兼容模式，不能与标准模式混用。兼容模式直到最终结果才整体入队，失败抛异常，停止继续撮合而非静默丢记录，但无法在撮合前知道最坏Trade数，因此不承诺同样的提前拒绝。Demo/Benchmark已用标准模式。

### 状态与失败语义

- begin失败：不发布ORDER，不修改簿、不推进CommandSequence，返回SYSTEM_UNAVAILABLE / OUTPUT_BLOCKED。
- append/commit在状态已应用后失败：不能说“没有成交”。结果保留APPLIED并置event_output_complete=false，协调器FAILED、后续接单停止；未提交stage释放，不产生部分入队组。异常同样停止并abort尚未接受的组。
- Journal已接受完整组，随后最终结果订阅者抛异常：该已接受组不会被abort撤回，协调器仍停止，等待对账。测试区分提交前/后异常。
- worker稍后写失败：WRITE_FAILURE可见，标准入口后续被gate阻断；之前的队列接受不倒推成已落盘确认。

订单的外部观察事件可能在Journal提交之前已送达，失败时不能撤销这些副作用；系统选择显式停止，不宣称状态/磁盘/所有订阅者原子提交或Exactly-Once。

## 3. 容量、所有权和性能代价

被动/撤单/Post-Only/未知品种的Journal预算为2（输入+结果）；可能成交的普通New保守取活跃订单数+2。没有预扫描撮合路径。该上界可能比真实成交数宽松，资源准入失败优先于后续业务校验，属于SYSTEM_UNAVAILABLE而不是业务拒单。

记录槽预算覆盖**producer stage + 排队 + worker正在写的vector容量**，不能在wait_pop时提前归还，不能只按size收费却保留巨型capacity。worker释放vector后才归还。单个批次预留直到写完，队列不足不等待空位，沿用失败终态策略。它是记录槽数量上限，不是Symbol字符串/allocator开销的总堆字节配额。

交易路径无文件写盘，但有mutex、vector预留/符号复制及可能的队列分配，不是无锁或严格非阻塞。预留在修改簿前完成；后续分配/接口异常不承诺回滚。stop/订阅操作要求producer静止；不是并发stop安全声明。当前worker写序不变，正常关闭排空并检查flush；written计数是ostream成功写记录，不能称durable sequence。

## 4. 新日志与Replay

新文件首行`DME_JOURNAL,2,COMMAND_BATCHES`，第二行为原列头，body保留13列。JournalRecord末尾增加format_version，旧aggregate与11/12/13列legacy文件仍兼容。

v2验证要求INPUT→Trade*→COMMAND_RESULT、记录序号连续、组内timestamp/symbol/command_sequence一致、结果order_id和Trade.aggressive_id对应输入；Cancel不得含Trade，非APPLIED不得有Trade，未准入的SYSTEM_UNAVAILABLE不得冒充完整结果。直接传vector给Replay也先验证版本/字段/完整分组，混版本或不完整输入在任何状态变更之前拒绝。

严格读取拒绝截断。宽容读取仅在EOF允许丢弃整条未完成尾命令，包括已完整写出的INPUT/Trade；v2结果行缺最终换行也不算完整。完整行身份错误、序号缺口和中间损坏不能用“尾部恢复”掩盖。旧legacy仍保留历史宽容行为，不能把这些旧文件标为新的完整组保证。

没有CRC，合法数字被改写仍可能无法仅凭格式检测；没有fsync/group-sync水位。这里的完整前缀是可读语法边界，不是已证明的断电持久化前缀。快照截点仍须由调用方选在完整命令边界，现有多簿API不会自动证明其与Journal绑定。

## 5. 验证与证据

| 验证 | 实际结果 | 原始输出 |
|---|---|---|
| 新目录MSVC Release / Ninja | 全量构建共50步（先4步模块验证、再46步）；writer最终防护重编后22/22通过，2.71s | [Release](validation/2026-11-12-m1-command-journal/release-ctest.txt) |
| 新目录MSVC ASan RelWithDebInfo | 50/50构建，22/22通过，10.64s | [ASan](validation/2026-11-12-m1-command-journal/asan-ctest.txt) |
| 扩展差分 | 100种子×10,000命令=100万通过 | [差分](validation/2026-11-12-m1-command-journal/differential-stress.txt) |

12组新测试涵盖：预留拒绝零状态变化、提交失败保留已应用语义、Trade追加失败保留核心待发、观察者提交前/后异常、真实预算1拒绝、真实多品种v2往返与Replay、INPUT+Trade截断尾组/缺最终换行、中间损坏/身份错/序号缺口、writer持有缓冲仍占预算、直接vector非法枚举/版本前置拒绝、兼容observer失败不静默且无半组入队、两种接线混用拒绝。原legacy Journal、Snapshot、策略和V1测试保留。

这些故障由Mock CommandJournal和定容量队列确定性触发，**不是实际磁盘满/掉电/kill -9注入**。Linux/UBSan/TSan本批仍未执行；TSan配置已包含新测试但没有runner通过证据。已知D9025、Benchmark C4244及故障Sink的MSVC ASan C4702告警未掩盖，不声称零警告。

VS x64 Developer cmd：

```bat
chcp 65001
cmake -S . -B build/m1-journal-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/m1-journal-release --parallel 8
ctest --test-dir build/m1-journal-release --output-on-failure
cmake -S . -B build/m1-journal-asan -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DENABLE_ASAN=ON -DENABLE_UBSAN=OFF
cmake --build build/m1-journal-asan --parallel 8
ctest --test-dir build/m1-journal-asan --output-on-failure
build\m1-journal-release\matching_engine_differential_tests.exe --seeds 100 --commands 10000 --seed-base 0xD1FF0000
```

## 6. 性能检查与源码身份

[本批单进程七场景](../benchmarks/results/2026-11-12-m1-command-journal-check/)保留环境、raw、summary及SHA-256，7条结果、3/3证据哈希匹配。core预解析45.81ns/op、routing45.70ns/op、dispatcher120.34ns/op、异步Journal395.31ns/op、full_v2 3,852.37ns/op；full采样P99=7,200ns、P99.9=223,700ns、max=1,683,500ns。完整数据不删异常值。

每场景60,000个固定BUY1/Cancel命令，仍为Balanced、未绑核/固定频率、普通后台环境；本批Journal已改变接受契约和接线，不能把它与旧observer场景当作严格同语义A/B。仅作可运行检查，不宣称提速或稳定延迟。停止时新增长Journal健康检查，未完整排空不冒充成功；排空/flush仍不在submit计时内。Monitor的内部处理时延也不包含begin/commit全部成本，不能拿它当完整链路延迟。

采集使用现有Windows脚本，设置`MATCHING_ENGINE_RAW_OUTPUT=1`，Executable=`build/m1-journal-release/matching_engine_v2_benchmarks.exe`、OutputDirectory=`benchmarks/results/2026-11-12-m1-command-journal-check`、Runs=1。

工作区尚未提交/推送，基线仍`c690dfd8b008e06690e914e272173a7f4e55d164`，环境JSON标记dirty。[累计源码patch](validation/2026-11-12-m1-command-journal/source_changes.patch)及[Git blob身份](validation/2026-11-12-m1-command-journal/source_git_blobs.txt)冻结三批源码与CI/Benchmark接线；patch已对基线index执行`git apply --cached --check`通过，未修改index。不要在当前工作区重复应用。

## 7. 回滚、未完成事项与面试介绍

回滚只撤本批CommandJournal/队列/v2读取/接线与测试target；保留前两批OrderBook数量保护和事件outbox。不要整仓reset，不覆盖历史原始证据。旧attach可继续教学用途，但若退回它，应同时撤回“撮合前完整Journal预算准入”声明；降级到legacy读取也不能继续宣称完整组恢复。

M0 R05在标准接线的进程内容量失败路径已修复，兼容observer失败也不再静默；这不关闭持久化、合法字段篡改、实际磁盘故障、唯一generation和公开Feed闭环等风险。剩余核心入口非法枚举（非Journal直接API）、bad_alloc故障验证等仍需按M1清单处理，不能把22/22等同整个V2.1完成。

面试可介绍：普通Logger可以丢诊断消息，恢复Journal不行。我把Journal从普通订阅回调中分离成显式的预留/提交接口，并用完整命令批次接收、全生命周期记录槽预算和回放前置校验处理半条日志；同时区分状态应用、队列接受、ostream写入与持久化，失败时停止而不是编造回滚或Exactly-Once。
