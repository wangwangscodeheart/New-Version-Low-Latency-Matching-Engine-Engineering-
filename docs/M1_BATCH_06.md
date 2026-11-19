# M1 第六批：模拟风控的整数极值与状态失效

日期：2026-11-14（Asia/Tokyo）。只增量修改可选PreTradeRisk及其测试，不修改撮合、Price Ladder、FIFO或订单入口协议。本批处理M0 C05及同模块的跨档数量累计风险；不宣称M1/V2.1整体完成。

后续：[M1第七批](M1_BATCH_07.md)已补订单池部分误用保护与移动回归；下文23项测试及剩余风险保留本批归档口径。

## 1. 源码事实、复现与修改范围

审查原实现发现：

- `llabs(position)`无法表示`INT64_MIN`的正绝对值。
- `order.price - reference_price`先在int64中相减，再转long double，极端输入在转换前就可能溢出。
- 仓位投影转浮点后比较，可能丢失接近64位边界的一股差异。本机MSVC使用相同的double/long double表示，不能靠换成长类型解决。[Microsoft类型说明](https://learn.microsoft.com/en-us/cpp/cpp/data-type-ranges?view=msvc-170)。
- 同侧多价格档位各自合法，合计仍可能超过UINT64_MAX；风险状态的`open_buy_quantity/open_sell_quantity`原来直接相加。

实际先运行不含旧UB的安全回归：持仓`INT64_MAX-1`再买2股，限额`INT64_MAX`，原实现错误放行并使断言失败。[修复前输出](validation/2026-11-14-m1-risk/risk-red-safe.txt)。没有在旧实现上执行MIN绝对值或溢出减法，并冒充其结果为合法业务行为。

| 文件 | 本批修改 | 原因 / 对应真实组件 |
|---|---|---|
| `src/risk_engine.hpp` | 无符号幅值/价格距离、精确bps阈值、仓位移动区间、累计量检查、状态可用性屏障 | 可选账户模拟层的限额计算及状态失效保护，不是新增交易所撮合规则 |
| `tests/risk_engine_tests.cpp` | 安全红灯、极值黄金案例、小范围独立oracle、导入与回调失败回归 | 防止平台精度差异或资源边界绕过限额 |
| `.gitattributes`与文档 | 固定增量patch字节并记录证据/限制 | 延续既有源码证据归档方式 |

仍使用原CTest target，没有新增可执行文件或依赖，CTest总数仍为23。`RiskRejectReason::STATE_UNAVAILABLE`追加于末尾，原枚举编号不变。

## 2. ADR-012：整数限额精确比较，不用浮点扩大“安全范围”

状态：下述整数路径已实现并验证。

### 价格偏离

按有符号大小关系选取无符号减法，得到精确距离，范围为0..UINT64_MAX。正参考价下，原规则等价于：

```text
distance <= floor(reference * bps / 10000)
```

不先计算可能溢出的乘积。把reference拆为`quotient*10000 + remainder`，阈值为`quotient*bps + floor(remainder*bps/10000)`；余数乘积小于10000×UINT32_MAX。先检查前一项与加法是否超出UINT64_MAX；若阈值已大于所有可表示距离，则此价格偏离规则不拒绝。没有使用非标准`__int128`或大整数库。

参考价非正仍按原规则禁用偏离检查。直接风险检查接受某个负价格，仅表示该项规则通过，绝不代表撮合核心接受非法价格。

### 仓位

`abs(position)`在无符号域计算，包含MIN。令当前仓位幅值为M、非负限额为L；同侧挂单量和新单量之和为Q：

- 朝零方向：Q必须位于`[max(M-L,0), M+L]`。
- 远离零方向：先要求M≤L，再要求Q位于`[0,L-M]`。

`M+L`至多为UINT64_MAX。先检查pending≤上界、新单量≤上界-pending，再相加比较下界，不溢出也不舍入。负仓位限额保守拒绝，不转换成巨大无符号限额。原有数量、订单数、价格偏离、名义金额、仓位、总敞口检查的业务优先级保持不变。

### 累计数量与状态失效

新增挂单计数前检查同侧uint64剩余容量，失败抛出`overflow_error`，不插入本条风险记录，也不修改其计数。bind在导入前置为不可用，全部完成才可用；事件更新异常也锁定不可用并继续抛出，交给既有Engine异常屏障停止。

不可用状态下，新单返回STATE_UNAVAILABLE；事件不继续修改部分状态，配置setter只更新冷配置，不访问部分导入向量。已知symbol撤单仍通过风险层，但**不保证Engine会接受**：若Engine已FAILED，它照旧拒绝所有输入。UNKNOWN_SYMBOL仍先拒绝。绑定对象不一致或新symbol尚未导入时，也不能拿别的注册表/缺失槽位做新单判断。

只有对可信、已修复来源的一次完整bind能恢复风险状态；它不会清除Engine/Book的FAILED。`state()`在失效后仅供诊断，业务判断必须同时检查可用性。导入/回调失败不是撮合回滚，控制面应重建已FAILED的协调器，而不是热重绑后直接续跑。

## 3. 真实测试结果与复现

新增七组（包括安全红灯那组），保留原有风控集成断言：

| 覆盖 | 内容 |
|---|---|
| 安全舍入回归 | MAX-1加BUY2必须拒绝；`--safe-boundary-only`可单独运行 |
| 仓位极值 | 24条MIN/MAX、UINT64_MAX数量、过限纠偏、零/负限额黄金案例 |
| 精确价格偏离 | 15条极值与边界，含大价格相差1、全宽距离、阈值溢出分支；9,150条小范围oracle |
| MIN敞口 | bind前配置与bind后设置均安全重算，并触发TOTAL_EXPOSURE_LIMIT |
| 挂单投影 | 24,786条小范围oracle，另4条全宽pending+new边界 |
| 导入累计量 | BUY/SELL超界停止、恰好UINT64_MAX、减量、来源修复后完整重绑 |
| 回调异常 | 合法核心跨档挂单让风险计数超界；风险失效且Engine/Book停止，当前核心订单不回滚 |

两个小范围oracle独立使用安全的小整数交叉乘法/有符号投影，范围有限且不调用生产helper，共33,936条比对；不是随机数据，也不宣传为完整uint64穷举。负价格/合成事件仅直接验证风险数学，不作为公开Feed或实际订单入口数据。

| 实际执行 | 结果 | 证据 |
|---|---|---|
| 全新MSVC Release / Ninja | 52/52编译链接成功，23/23 CTest通过，5.38s | [构建与CTest](validation/2026-11-14-m1-risk/release-build-and-ctest.txt) |
| 全新MSVC ASan RelWithDebInfo | 52/52编译链接成功，23/23通过，11.74s | [构建与CTest](validation/2026-11-14-m1-risk/asan-build-and-ctest.txt) |
| 独立风险程序及额外差分 | 七组及原集成断言通过；enum_boundary_v2的100种子×10,000命令通过 | [绿灯与100万差分](validation/2026-11-14-m1-risk/risk-green-and-differential.txt) |

两套构建并行执行，耗时受争用影响，不与前批测试秒数做性能比较。构建保留既有D9025、Benchmark C4244和ASan故障Sink C4702警告。没有本轮Linux/UBSan/TSan结果，ASan通过也不能代替UBSan。第五批独立分配故障程序的ASan覆盖限制仍有效；本批风险程序没有替换new/delete。

VS x64 Developer cmd：

```bat
chcp 65001
cmake -S . -B build/m1-risk-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/m1-risk-release --parallel 8
ctest --test-dir build/m1-risk-release --output-on-failure
cmake -S . -B build/m1-risk-asan -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DENABLE_ASAN=ON -DENABLE_UBSAN=OFF
cmake --build build/m1-risk-asan --parallel 8
ctest --test-dir build/m1-risk-asan --output-on-failure
build\m1-risk-release\matching_engine_differential_tests.exe --seeds 100 --commands 10000 --seed-base 0xD1FF0000
build\m1-risk-release\matching_engine_risk_engine_tests.exe
```

## 4. 性能与源码身份

本批不报告风险处理延迟：现有七场景`v2_perf.cpp`并未接入PreTradeRisk，重跑它不能衡量修改过的风险路径。新增整数除法和分支需要后续同输入的风险专项A/B；不能根据O(1)或测试耗时宣称零开销/无退化，也不把[第五批七场景数据](../benchmarks/results/2026-11-14-m1-allocation-check/)当成本批风险测量。

基线仍为`c690dfd8b008e06690e914e272173a7f4e55d164`加未提交工作区。源码身份由[第五批累计patch](validation/2026-11-14-m1-allocation/source_changes.patch)加[本批两个源码文件增量patch](validation/2026-11-14-m1-risk/risk_changes.patch)共同确定；两个patch对基线index的联合`git apply --cached --check`通过，不修改index。[30项Git blob清单](validation/2026-11-14-m1-risk/source_git_blobs.txt)按入库文本规范化计算，覆盖当前全部已改编译输入及既有构建/脚本项。不要重复应用patch到当前工作区。

## 5. 限制、回滚与下一步

本批精确化的是价格偏离、仓位与累计数量，**不是全部风控算术**。notional/总敞口仍用long double，仍可能存在金额边界舍入、减量累计残差及NaN配置验证不足。仓位和参考价仍由外围注入，成交不自动归属到完整账户账本。可选RiskCheckedTradingEngine不是不可绕过的交易所入口；标准回放也不因此恢复账户状态。

回滚只选择本批risk_engine.hpp和risk_engine_tests.cpp的diff及相关文档/证据属性，不整文件回退撮合核心或删除前五批成果；安全舍入回归应重新失败，不再保留“极值已保护”声明。

C05所指MIN绝对值和有符号价差危险表达式已移除，固定版本有极值和独立oracle证据；不能把无UBSan的本机结果写成全域UB证明。池API误用、全部分配异常、快照generation复用、真实durable prefix/磁盘故障、公开行情闭环仍未完成。下一步仍按门禁核对剩余M1范围，M2需先明确公开行情schema及可见性；不据此宣布整个任务书完成。

面试可介绍：我先用安全边界用例复现了MSVC下“一股越限仍放行”，再把风险判定改成精确整数阈值与方向容量区间；同时给不可表示的观察状态加失效屏障。我明确区分风险拒绝、观察者失败和撮合回滚，并承认金额估算及账户模型的边界。
