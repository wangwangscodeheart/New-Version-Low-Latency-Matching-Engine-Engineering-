# 公开行情链路：独立C++分配计数

日期：2026-11-24（Asia/Tokyo）。本轮收尾此前未接线的四个计数/采集文件，另增独立companion；不改`src/`、V1撮合、既有`v2_perf.cpp`或公开行情Fixture。它补的是合成链路的有限分配证据，不是M3真实数据验收、M4恢复或M5完整性能验收。

## 模块与设计原因

| 文件 | 增量职责 | 实际系统对应/面试说明 |
|---|---|---|
| `benchmarks/allocation_meter.hpp/.cpp` | 可执行文件本地替换new/delete，TLS区间计数、饱和溢出、对齐/nothrow/零大小语义 | 测量工具，不是交易内存分配器；成功hook调用次数不等于业务对象数 |
| `benchmarks/public_feed_allocations.cpp` | 复用已有三个Fixture，各分setup/run/stop_and_validation/teardown；全部对账成功才输出12行 | 将初始化、热负载、日志排空验证和析构分开，不用一个总数字混淆边界 |
| `tests/allocation_meter_tests.cpp` | 保留八组，新增24个失败/new_handler组合；失败注入仅测试目标编译 | 验证探针本身，避免把失败尝试或转发重计当成真实分配 |
| `scripts/run_allocation_probe_windows.ps1` | 独立进程采集、超时、测量口径/UInt64/工作量校验、拒绝覆盖，失败也留证据 | 可审计实验采集，不只保留成功/最快输出 |
| CMake/CI | 默认OFF；仅显式开启的非sanitizer、单配置Release构建加入两个目标；Linux单列小负载验证 | 诊断工具和原延迟基准隔离，不把全局new替换混进原生产/延迟可执行文件 |

## 精确口径

计数仅包含创建Scope的调用线程中，实际进入替换函数且成功返回存储的C++17 new/new[]请求；覆盖普通、数组、对齐、nothrow。记录请求size参数之和，零大小请求记0字节。失败尝试不计数；handler中若有成功hook调用则依其实际发生线程/区间计数。允许嵌套且互斥归属最内层，finish幂等；仅支持在同一线程创建/结束/销毁Scope。

表达式级分配省略/合并的依据见[C++工作草案expr.new](https://eel.is/c++draft/expr.new)；GCC也有对应[优化选项说明](https://gcc.gnu.org/onlinedocs/gcc/Optimize-Options.html)。本轮没有关闭全局优化。

**不测**：malloc/realloc、CRT/OS内部、Journal/Logger worker分配、free次数、存活内存、RSS、堆峰值、分配器取整/缓存成本和时间。编译器允许省略/合并某些new表达式，因此数字只代表本次构建观察到的hook，不是源码中的逻辑分配总数。单元测试用不透明函数指针验证真实函数调用，不关闭全局优化来制造计数。

四阶段各有独立Scope。setup包含`make_unique<Fixture>`所有者；run使用`run(false,nullptr)`，不采样延迟，但已有Monitor内部时钟行为保持原样；stop保留消费者空簿/连续序号、Journal/Logger排空、健康/计数核对；teardown含析构和自有临时目录清理。输出格式化在Scope外，所有场景成功且checksum一致才输出。异常/溢出/输出失败均非零，非零退出时部分文本不接受。

每个CSV阶段行的commands/public_messages/wire_bytes都是同一完整场景的上下文，不是该阶段新增工作量，不能把四行相加。阶段分配计数彼此不重叠，但也不能补出未观察线程或总堆数据。

## 构建、测试与采集

```powershell
# 在MSVC x64开发环境中；Ninja配置前chcp65001
cmake -S . -B build/public-feed-alloc-release -G Ninja `
  -DCMAKE_BUILD_TYPE=Release -DENABLE_ALLOCATION_PROBE=ON
cmake --build build/public-feed-alloc-release --parallel 3
ctest --test-dir build/public-feed-alloc-release --output-on-failure
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/run_allocation_probe_windows.ps1 `
  -Executable build/public-feed-alloc-release/matching_engine_public_feed_allocations.exe `
  -OutputDirectory benchmarks/results/my-allocation-capture -Runs 3 -TimeoutSeconds 60
```

ExecutionPolicy仅作用于子进程，不修改机器策略。输出目录必须不存在；默认目录为本轮日期，不会覆盖已存结果。`ENABLE_ALLOCATION_PROBE=OFF`时仍43项；开启后45项。ASan/UBSan/TSan选项、全局sanitize标志、Debug及多配置构建被拒绝，避免把替换new工具宣称为普通sanitizer验证；不是抵御任意外部toolchain标志注入的安全机制。

`BENCHMARK_SMOKE=ON`把companion从每品种2500轮降到25轮；Linux新job只构建/运行两个独立目标，默认43项和gzip矩阵继续独立验证。小负载不是正式分配结果。Flags字段包含配置完成后的CMake全局/Release/目录选项，不含defines/includes；C++标准另列。原延迟基准仍声明allocation_measurement=not_measured，没有拿探针计数运行替代其计时结果。

## 实际工程采集（不是生产结论）

[三进程证据](../benchmarks/results/2026-11-24-public-feed-allocations/)包含stdout/stderr、36行汇总、环境/二进制SHA及8个文件的校验清单。每进程每场景60000条合成命令、67500帧、3210000 wire字节，AAPL/TSLA/NVDA自行构造，无市场记录。源身份是`045c7a2`基线加未提交增量，由本轮规范化Git blob清单绑定，不能说原提交已经包含这个工具。

MSVC19.44.35228.0 Release、Windows Balanced、未绑核/冻结频率。下面仅列三个进程中run阶段的观察范围，非线程全覆盖/因果对比：

| 场景 | 成功C++ hook次数范围 | 请求字节范围 |
|---|---:|---:|
| Journal + public sync |272056–272804|28624281–28648217|
| Full diagnostics + public sync |422736–431676|54932287–56105743|
| Full diagnostics + burst16 |424063–430568|55102527–55952351|

跨进程计数有变化，未完成归因，不将此表解读为固定常数、优化收益或场景间确定因果。证据表明该限定范围不是零分配；不推导ns/单次分配、总堆用量、worker零分配或生产SLA。

采集器校验测量口径及工作量，不认证任意外来二进制或完整JSON schema；实际构建身份另与源码/二进制清单核对。stdout/stderr是.NET解码再UTF8写入的文本捕获，不是原始管道字节跟踪。结果SHA验证入库的文件字节，不是提供商真实性；script_sha256是当时工作区原始脚本字节，Git blob清单则遵循仓库属性，二者口径不同。

本机45/45、默认Release/ASan43/43、严格编译、24个失败组合、14个collector夹具、配置拒绝及后续实际Linux状态见[验证说明](validation/2026-11-24-public-feed-allocations/)与[验证记录](VALIDATION.md)。真实授权股票数据、逐层延迟、总内存/工作线程和受控M5仍未完成。
