# M1 第七批：订单池归还检查与移动所有权

日期：2026-11-14（Asia/Tokyo）。增量处理M0 C01，不改变V1撮合算法、LIFO槽位顺序、订单布局、价格优先级或单写者模型；本批不是整个M1/V2.1完成声明。

后续：[M1第八批](M1_BATCH_08.md)已处理快照generation复用覆盖；下文C03等剩余事项保留本批归档口径。

## 1. 已复现事实与改动

ObjectPool原来接受任意非空指针。两个直接API误用用例实际失败：

- 容量2、全部已借出，归还同一槽位两次，空闲数变为2而非1：[重复归还红灯摘录](validation/2026-11-14-m1-pool/duplicate-red.txt)。
- 容量1、槽位已借出，归还另一个仍存活池的指针，错误增加空闲数：[跨池归还红灯摘录](validation/2026-11-14-m1-pool/foreign-red.txt)。

这证明公共池API存在误用保护缺口，**不表示正常撮合路径已经出现double-free或UAF**。此外，原隐式拷贝会把free_list中的源池指针原样复制；本批用编译期约束禁止这种拷贝，不伪称在旧版本运行过其悬空访问。

| 文件 | 本批修改 | 为什么 / 真实组件对应 |
|---|---|---|
| `src/order.hpp` | 每槽预分配in_use标记；归还前检查地址/槽边界/状态；禁止拷贝，显式移动构造 | 池化订单对象的所有权和资源回收边界，不替换撮合数据结构 |
| `tests/order_pool_tests.cpp` | 七组独立回归和编译期类型契约 | 直接验证池误用、LIFO、槽复用及移动后的真实簿操作 |
| `tests/allocation_failure_tests.cpp` | 在原七组上加入池操作/移动不分配验证，现八组 | 复用既有可执行文件内故障注入，不往生产代码加测试开关 |
| `CMakeLists.txt` | 注册/安装池测试程序，CTest从23增至24项 | 纳入现有构建体系，无新依赖或复杂框架 |

## 2. ADR-013：有界归还、唯一所有权，保留原始指针接口

状态：下述池API保护已实现并验证。

构造时分配每槽1字节的状态存储。allocate仍从原free_list尾部取槽，再标记借出。deallocate先排除空指针/空池，使用uintptr地址差验证完整槽位、边界及规范指针相等，最后检查借出标记；只有有效归还才清除标记并push一次。无效归还忽略，与既有LimitLevelPool普通release风格一致。

不在未验证候选指针上做指针相减、解引用或`base + byte_count`范围计算。free_list始终不超过预留容量，因此这些操作不扩容；allocate/deallocate标为noexcept。检查增加了热路径分支和状态访问，不能称零开销。

池不可拷贝，也不支持移动赋值；移动构造转移存储、空闲表和借出标记，原有活动对象地址保持有效，源池容量与空闲数都为0。OrderBook原有移动后重绑价格层池的逻辑不变，源Book仍不可接单。没有使用重新分配/逐元素重建来“修正”内部指针。

适用边界：

- 地址检查针对项目支持的普通平坦地址MSVC/GCC/Clang目标，不声称适用于每一种C++抽象指针模型。
- 仅防止当前空闲槽重复归还及可识别的非本池/非槽位指针；槽位重新借出后，旧裸指针与新持有者地址相同，无法识别代际/ABA。没有引入generation handle。
- 池不验证订单是否仍挂在FIFO/索引中。生产调用者仍必须先摘链、删索引，再归还；也不提供任意伪造指针的安全认证。
- 本批不改变LimitLevelPool的复制规则或trusted-owner快速归还前置条件，不把订单池修复扩大为所有池API均已硬化。

这是撮合基础设施中的对象生命周期保护，而不是并发内存回收或事务回滚。

## 3. 回归、故障注入与真实结果

池程序七组覆盖：重复归还；跨池归还；容量0..8/耗尽/历史LIFO顺序；null/栈对象/槽内地址/one-past归还；带活动对象的两次移动及空源池；500轮、3,500次独立槽激活；Book移动后继续成交、撤单、补满容量和快照。

编译期检查不可拷贝、不可移动赋值、移动构造及借还noexcept。原故障程序新增组在精确武装bad_alloc后执行有效/无效归还、借出和移动，观察到0次operator new尝试。所有指针比较均在对应槽存储存活期间完成；断言和输出在解除故障后执行。

| 实际验证 | 最终结果 | 记录 |
|---|---|---|
| MSVC Release / Ninja | 24/24 CTest通过，7.91s | [最终构建与全量测试](validation/2026-11-14-m1-pool/final-build-and-ctest.txt) |
| MSVC ASan RelWithDebInfo | 24/24通过，12.05s | 同上，独立ASan构建目录 |
| 独立池/故障程序 | 七组池回归、八组故障回归通过 | [绿灯](validation/2026-11-14-m1-pool/stress-and-green.txt) |
| 额外参考差分 | 100种子×10,000命令通过，enum_boundary_v2 | 同一绿灯记录 |

这两个目录为本批构建目录；修正新增测试里存储销毁后的指针比较方式后，重编故障程序并重跑全量。归档的是最后复验记录，构建显示ninja无需再编译，不把它称为干净构建的完整编译日志，也不与前轮秒数混用。已有D9025、Benchmark C4244及ASan故障Sink C4702警告没有被包装成零警告。

本轮没有Linux Release/UBSan/TSan、真实系统OOM、磁盘满或进程强杀结果。独立分配故障程序的new/delete替换仍限制其部分ASan分配族诊断覆盖，见[第五批说明](M1_BATCH_05.md)；其余23个测试程序无该替换。ASan全绿不等于证明所有生命周期错误不存在。

VS x64 Developer cmd复现：

```bat
chcp 65001
cmake -S . -B build/m1-pool-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/m1-pool-release --parallel 8
ctest --test-dir build/m1-pool-release --output-on-failure
cmake -S . -B build/m1-pool-asan -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DENABLE_ASAN=ON -DENABLE_UBSAN=OFF
cmake --build build/m1-pool-asan --parallel 8
ctest --test-dir build/m1-pool-asan --output-on-failure
build\m1-pool-release\matching_engine_differential_tests.exe --seeds 100 --commands 10000 --seed-base 0xD1FF0000
build\m1-pool-release\matching_engine_order_pool_tests.exe
```

## 4. 性能观察：不宣称无退化

状态字节名义存储为capacity字节，另有vector/allocator开销；没有测RSS，不把这个算式当作实测峰值内存。修改触及核心借还路径，保留[旧池控制程序采集](../benchmarks/results/2026-11-14-m1-pool-control/)和[新池程序采集](../benchmarks/results/2026-11-14-m1-pool-check/)。

| 场景，批量ns/op | 旧池观察 | 新池观察 |
|---|---:|---:|
| core预解析 | 49.64 | 84.17 |
| routing | 51.78 | 53.32 |
| dispatcher | 130.26 | 145.46 |
| monitor | 175.29 | 178.04 |
| async logger | 586.71 | 592.06 |
| async journal | 388.52 | 389.73 |
| full_v2 | 4,933.20 | 4,791.61 |

两次各1个独立进程，采集相隔数小时；仍为Windows Balanced、未绑核/固定频率、后台未隔离，**不是受控交错A/B**。相同Benchmark源码/输入逻辑，每场景60,000命令；程序Process Runs=5仅为同进程内部批次。旧程序来自第六批构建，新程序来自本批构建；[本地二进制SHA-256](validation/2026-11-14-m1-pool/benchmark_binary_sha256.txt)仅标识这两个文件，不要求未来构建产物字节相同。

core数值增大且新routing小于core，不能根据这一对样本归因；也不能拿full均值变小宣称提速。新full采样P99=9,100ns、P99.9=396,000ns、max=2,870,800ns，长尾全部保留。两目录各7行结果，合计6/6证据文件SHA-256匹配。checksum一致只是原程序的有限结果摘要，不能替代完整状态/日志校验。submit计时不含最后drain/flush，不是durable latency。

本批正确性验证通过，**性能无退化尚未验收**；受控同输入归因仍属于M4，不能删除异常样本来完成门禁。采集沿用现有Windows脚本，显式MATCHING_ENGINE_RAW_OUTPUT=1，Runs=1；没有改Benchmark/采集脚本。

## 5. 源码身份、回滚与剩余事项

基线仍为`c690dfd8b008e06690e914e272173a7f4e55d164`加未提交工作区，不把编译宏的基线SHA冒充干净新版本。本批重新冻结[七批累计patch](validation/2026-11-14-m1-pool/source_changes.patch)与[33项Git blob清单](validation/2026-11-14-m1-pool/source_git_blobs.txt)，包括已改编译输入、构建/脚本及证据属性；按Git入库规范化计算，不是任意checkout换行字节的SHA-256。patch对基线index的`git apply --cached --check`通过且不修改index；不要重复应用到当前工作区。

回滚只撤本批ObjectPool类改动、新池测试target和文件、故障程序新增函数/调用及对应文档。不可删除整个allocation_failure_tests或还原整个CMake而移除前批成果；回滚后重复/跨池归还用例应重新失败，不继续保留修复声明。

C01指定的订单池误用保护已补，但裸指针代际、已挂链对象误归还、其他池复制规则、全部分配异常仍不能笼统关闭。快照generation复用、durable prefix/真实磁盘故障、公开行情闭环和风险金额浮点边界也仍开放。下一步继续核对M1门禁与恢复边界，再进入M2公开行情schema/可见性；不新增无证据的并发框架或订单类型。

面试可介绍：我保留原有LIFO对象池，用独立误用回归复现重复槽位和跨池污染，再以预分配状态位和不可拷贝所有权做小范围修复；同时验证移动后的真实订单簿仍能成交、撤单和复用槽位。我明确保留裸指针代际限制，也没有把一次性能样本当作无退化证明。
