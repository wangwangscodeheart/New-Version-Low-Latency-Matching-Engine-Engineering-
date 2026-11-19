# M1 第四批：核心入口枚举校验

归档：2026-11-12（Asia/Tokyo）。继续在独立撮合仓库增量修改，保留V1核心及前三批成果。本批修复M0 S10，不新增订单类型，不宣称M1全部验收或V2.1全部完成。

后续：[M1第五批](M1_BATCH_05.md)已补新价格节点分配失败的实际注入与结构清理验证；以下“未执行bad_alloc”及22项结果保留本批归档时口径。

## 1. 修改、原因与组件对应

| 文件 | 本批修改 | 为什么 / 真实组件对应 |
|---|---|---|
| `src/orderbook.hpp` | 重复ID检查后，显式拒绝非法Side/TIF | 核心入口防御，不能假设所有调用都经过Journal或正确解析器 |
| `src/reference_engine.hpp` | 独立实现同样的业务拒绝规则 | 参考oracle与公开语义一致，不复用优化核心的校验实现 |
| `src/events.hpp` | 末尾追加INVALID_SIDE=12、INVALID_TIME_IN_FORCE=13及字符串 | 可解释拒单原因，不重排旧编码 |
| `src/replay.hpp` | 两种新拒绝原因进入诊断CSV解析 | 拒单事件可读往返，不把诊断日志改成恢复协议 |
| `src/types.hpp` | 非法方向的to_string返回UNKNOWN，不再默认SELL | 避免错误方向被审计/诊断输出伪装成合法卖单 |
| `tests/order_type_tests.cpp` | 五组入口/优先级/续送回归，扩充拒因roundtrip | 黄金边界、全量枚举空间与输出契约组合验证 |
| `tests/differential_tests.cpp` | 普通CTest随机域加入非法枚举；输出generator版本 | 不只在手工案例中覆盖入口错误 |

没有修改撮合循环、价格层、索引/对象池布局或Journal协议。

## 2. ADR-010：在核心拒绝不属于支持集合的枚举

状态：已实现并验证。Side支持BUY/SELL；TIF支持GTC/IOC/POST_ONLY。C++ enum class不会阻止调用者从原始字节static_cast出其他值，外围校验不是核心安全边界的替代。

已路由到簿后的顺序为：

```text
输出屏障 -> 非零递增序列 -> ID=0/重复ID
 -> Side -> TimeInForce -> 原价格/数量规则
 -> Post-Only -> 档位累计量 -> Pool/Index -> 原撮合
```

因此两枚举同时非法时方向优先；重复ID仍优先；非法枚举优先于价格0、数量0和满池。合法方向/TIF的旧业务顺序不变。prepare_insert只读探测，不在枚举拒绝前占用索引槽。

非法字段属于已准入命令的业务拒绝：只推进CommandSequence，产生一个index=0的OrderRejectedEvent，不修改订单、FIFO、档位、池、索引及其峰值指标。序列0/旧序列仍SEQUENCE_REJECTED且零事件；输出待发仍OUTPUT_BLOCKED且序号不推进。拒绝事件自身被Sink拒收时，也按原outbox契约保留/续送，不重执行命令。

兼容process_new_order、无Sink诊断路径和record_events=false均经过相同入口。快照恢复原本已独立校验Side，没有为本批改变恢复格式。

### Journal边界不混淆

标准CommandJournal更早拒绝不能序列化的Side/TIF，属于预准入协议失败，不保证返回核心业务RejectReason；本批不改变该规则。普通内存Journal仍是legacy观察捕获，非法输入保存为UNKNOWN后不能恢复原始非法字节，严格CSV不会把UNKNOWN猜成某个合法方向。因此“拒单EngineEvent可诊断往返”不等于“legacy CSV可无损保存任意畸形Command”。

## 3. 红灯、绿灯及测试覆盖

新增回归先在尚未补核心校验的实现上实际运行，失败于`optimized.check_invariants()`，证明非法方向已进入簿：[红灯](validation/2026-11-12-m1-enum/enum-red.txt)。随后实现校验、扩展测试并重新编译通过：[绿灯](validation/2026-11-12-m1-enum/enum-green.txt)。不把修复前后扩充过的测试文件称为逐字节相同版本。

穷举所有uint8枚举表示中的非法值：Side 2..255共254条；TIF 3..255各对BUY/SELL验证共506条，合计760条业务拒绝。每条与独立ReferenceEngine比较状态码、事件和完整簿，验证不变量、序号/index、容量/峰值不变，最后复用被拒ID挂单成功。另覆盖重复/零ID、两个枚举均非法、价格/数量错误、满池、零/旧序列、无记录模式、兼容API、pending屏障及非法TIF拒绝事件续送。

差分生成器现在为`enum_boundary_v2`：不增加PRNG抽样次数，复用既有tif_case将部分输入换成非法枚举。**即使seed相同，生成命令也不等于旧版本**；STD分布仍不保证跨工具链逐字节一致。Benchmark的固定BUY1/Cancel输入没有改变，不混用差分输入和性能输入。

| 实际验证 | 结果 | 原始记录 |
|---|---|---|
| 全新MSVC Release / Ninja | 50/50构建、22/22 CTest通过，7.57s | [Release](validation/2026-11-12-m1-enum/release-ctest.txt) |
| 全新MSVC ASan RelWithDebInfo | 50/50构建、22/22通过，10.94s | [ASan](validation/2026-11-12-m1-enum/asan-ctest.txt) |
| 扩展差分，新generator | 100种子×10,000命令=100万通过 | [差分](validation/2026-11-12-m1-enum/differential-stress.txt) |

本轮Linux/UBSan/TSan、核心bad_alloc和实际磁盘/进程崩溃注入未执行；不因全绿就关闭这些风险。已知MSVC D9025、Benchmark C4244和故障Sink的ASan C4702告警仍如实保留。

VS x64 Developer cmd复现：

```bat
chcp 65001
cmake -S . -B build/m1-enum-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/m1-enum-release --parallel 8
ctest --test-dir build/m1-enum-release --output-on-failure
cmake -S . -B build/m1-enum-asan -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DENABLE_ASAN=ON -DENABLE_UBSAN=OFF
cmake --build build/m1-enum-asan --parallel 8
ctest --test-dir build/m1-enum-asan --output-on-failure
build\m1-enum-release\matching_engine_differential_tests.exe --seeds 100 --commands 10000 --seed-base 0xD1FF0000
```

## 4. 性能与源码身份

正常新单增加固定次数枚举比较，不增加扫描、锁、线程、分配或磁盘I/O；不能据此直接断言测量中零成本。[本批单进程七场景](../benchmarks/results/2026-11-12-m1-enum-check/)保留全部raw/环境/CSV：core预解析79.90ns/op、routing56.46ns/op、dispatcher123.21ns/op、异步Journal376.90ns/op、full_v2 4,397.19ns/op；full采样P99=7,600ns、P99.9=413,300ns、max=2,259,200ns。7行结果、3/3 SHA-256匹配。

仍为Balanced、未绑核/固定频率、非隔离后台环境，单进程每场景5批，不是5个独立进程。部分观测耗时较前批增大，未做受控交错A/B或归因，不能声称无性能退化或优化成功，也不删异常值。正确性验证通过不等于数值性能门禁完成；正式归因仍按M4规范执行。队列排空/flush不在submit计时内，不能称持久化延迟。

采集使用现有Windows脚本，显式`MATCHING_ENGINE_RAW_OUTPUT=1`，Executable=`build/m1-enum-release/matching_engine_v2_benchmarks.exe`、OutputDirectory=`benchmarks/results/2026-11-12-m1-enum-check`、Runs=1。

基线仍为`c690dfd8b008e06690e914e272173a7f4e55d164`加未提交工作区；环境标记dirty，不能将编译宏里的基线SHA冒充干净新版本。[累计源码patch](validation/2026-11-12-m1-enum/source_changes.patch)与[Git blob身份](validation/2026-11-12-m1-enum/source_git_blobs.txt)已冻结四批代码；patch对基线index的`git apply --cached --check`通过，不修改index。不要在当前工作区重复应用。原始证据时间标签保持工具输出。

## 5. 回滚、剩余风险与面试说法

仅反向应用本批校验、拒因、formatter及测试/生成器diff；不能整文件恢复events/orderbook而删掉前三批成果。保留历史证据。如果回滚生产保护，新回归应重新失败，不继续保留“入口已安全”声明。

S10核心入口漏洞已关闭，但bad_alloc定点故障验证、池误用硬化、真实durable prefix与公开Feed闭环仍未完成。下一步先核验剩余M1故障边界，再按门禁进入M2；不把项目包装成生产交易所。

面试可介绍：我没有把enum class当成运行期校验，核心直接入口也显式验证支持集合，并用760个非法枚举案例、独立参考模型和随机域覆盖确认拒单原子性；同时固定拒绝优先级，区分准入失败、业务拒绝和待发屏障，不用一个REJECT笼统掩盖状态变化。
