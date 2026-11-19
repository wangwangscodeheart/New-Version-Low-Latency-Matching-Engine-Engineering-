# M1 第十批：生命周期Audit冷路径导出的错误报告

日期：2026-11-18（Asia/Tokyo）。增量补AuditLog导出缺口，只修改`src/audit_log.hpp`和`tests/journal_replay_tests.cpp`的本批逻辑；不改撮合、生命周期事件记账、Journal、AsyncLogger、CSV字段或CMake。本批不是M1/M3/V2.1整体验收。

## 1. 源码事实与红灯

原`AuditLog::save(filename)`仅检查文件能否打开，随后写header/records，依赖ofstream析构关闭；写入、flush或close失败可能不报告。先抽出保持原序列化逻辑的借用ostream接缝、尚未补错误检查时，注入拒绝所有写入的streambuf，实际得到`stream_failed=1 error_reported=0`并断言失败。[红灯记录](validation/2026-11-18-m1-audit-export/audit-export-red.txt)。不是在旧原版里凭空存在ostream接口，也不冒充真实磁盘满。

## 2. ADR-016：同步操作报告异常，保留原所有权与格式

状态：以下导出错误检查已实现并验证已测路径。

| 修改 | 原因 / 真实组件对应 |
|---|---|
| 新增`save(std::ostream&) const`，检查header/逐记录输出并flush检查 | 让控制面导出能显式失败，也能通过标准流做确定性故障测试；不引入持久化框架 |
| 原filename入口保持文本模式/trunc/open检查，委托导出后显式close并检查 | 文件所有者承担最终close检查，不把析构返回当作导出成功 |
| 原Journal/Audit/Replay target增加五组回归，现共10组 | 原五组保留，验证日志格式、错误报告和后续使用，不新增target |

Audit导出是一次冷路径同步const操作，错误直接抛给调用者；无需沿用异步Logger的sticky health，也不会自动清除坏流、自动重试或清空内存记录。已经完成的撮合不会因为导出失败回滚。

借用ostream会flush但不close；借用ofstream的最终close检查仍由调用者负责。方法不主动修改异常mask、数字flags或locale，使用调用者当前的格式配置；正常默认十进制流对应既有CSV。没有引入任意flags/locale下的规范化序列化承诺。

调用者必须让Audit所属事件写者静止，不能与on_event并发或重入修改，也不能并发使用同一个输出流。导出不进入submit热路径；Demo原本在交易和异步组件停止后调用save，调用方式不变。

## 3. 回归与实际执行

新增五组：

1. 坏流必须报告错误，修复后`stream_failed=1 error_reported=1`。
2. 手工审查的9条生命周期CSV黄金输出，涵盖CREATE/SUBMIT、双方Trade、部分/全部成交、撤单；空Audit只导出header。借用流逐字节一致，filename文本模式读回逻辑一致（Windows保留原CRLF转换），借用流调用后仍归调用者使用。
3. 3个写入截点（无字节、header内、数据行内）×2种streambuf行为（短写/抛异常）×2种异常mask，共12条；都报错、不清理坏流、不改异常mask，保留预期输出前缀。
4. sync返回失败/抛异常×两种mask，共4条；即使完整CSV已进入streambuf，也不能返回成功。
5. 目标为目录或父目录不存在时报告open错误；之后向合法目标可成功导出。

失败后还验证内存记录可原样重新导出、后续TSLA撤单仍能记录。不把这些断言扩大为穷举每个私有生命周期状态；on_event本批未修改。测试streambuf同时覆盖xsputn/overflow/sync，无sleep、无生产故障钩子。新增文件用例使用独占创建的临时目录，清理只针对本fixture的绝对直接子目录。

| 2026-11-18实际验证 | 结果 | 证据 |
|---|---|---|
| 全新MSVC Release / Ninja | 54/54构建成功，24/24 CTest通过，8.23s | [Release](validation/2026-11-18-m1-audit-export/release-build-and-ctest.txt) |
| 全新MSVC ASan RelWithDebInfo | 54/54构建成功，24/24通过，10.43s | [ASan](validation/2026-11-18-m1-audit-export/asan-build-and-ctest.txt) |
| 独立Journal/Audit/Replay及额外差分 | 10组通过；enum_boundary_v2的100种子×10,000命令通过 | [绿灯与100万命令](validation/2026-11-18-m1-audit-export/green-and-stress.txt) |

红灯后扩充过测试，不声称整个文件与红灯阶段逐字节相同。保留既有D9025、Benchmark C4244和ASan故障Sink C4702警告，不包装成零警告。close检查经过代码审查，但没有实际close失败注入；sync故障不是close故障。真实磁盘满/强杀/断电及本轮Linux/UBSan/TSan未执行，分配故障程序ASan限制仍见[第五批](M1_BATCH_05.md)。

VS x64 Developer cmd复现：

```bat
chcp 65001
cmake -S . -B build/m1-audit-export-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/m1-audit-export-release --parallel 8
ctest --test-dir build/m1-audit-export-release --output-on-failure
cmake -S . -B build/m1-audit-export-asan -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DENABLE_ASAN=ON -DENABLE_UBSAN=OFF
cmake --build build/m1-audit-export-asan --parallel 8
ctest --test-dir build/m1-audit-export-asan --output-on-failure
build\m1-audit-export-release\matching_engine_journal_replay_tests.exe --audit-export-failure-only
build\m1-audit-export-release\matching_engine_differential_tests.exe --seeds 100 --commands 10000 --seed-base 0xD1FF0000
```

## 4. 性能、源码身份与回滚

本批没有测Audit导出耗时/磁盘吞吐。现有七场景Benchmark（包括full_v2）不包含AuditLog导出，重跑不能衡量本批修改；不沿用[第九批观察](M1_BATCH_09.md)声称本批提速。逐记录流状态检查和最终flush属于冷路径，不能用测试总秒数宣称其零开销。撮合及受控性能门禁保持原有边界。

基线仍为`c690dfd8b008e06690e914e272173a7f4e55d164`加未提交工作区。[十批累计patch](validation/2026-11-18-m1-audit-export/source_changes.patch)及[38项Git blob身份](validation/2026-11-18-m1-audit-export/source_git_blobs.txt)冻结当前已改编译输入及既有构建/脚本/证据属性，按Git文本入库规范化，不是任意checkout换行字节SHA-256。patch对基线index检查通过；独立临时index重建身份的结果见[归档检查](validation/2026-11-18-m1-audit-export/archive-check.txt)，真实暂存内容不变。不要重复应用到当前工作区。

回滚仅撤本批Audit导出函数和测试增量，保留原五组测试及前九批成果。若同时撤掉新ostream接口，需一并撤掉适配该接口的测试；若只去掉错误检查而保留接缝，单独失败流回归会重新得到error_reported=0，不能保留修复声明。

## 5. 保留的限制与下一步

filename入口仍以trunc写入，不提供原子替换或旧文件保留；异常可能留下截断/部分目标。调用成功只表示本次流输出/flush及拥有文件的close未报告错误，没有fsync/FlushFileBuffers或崩溃一致性保证。CSV字段转义、任意locale规范化、内存Audit容量预算和完整异步审计持久化没有在本批实现，不能拿Audit导出来恢复交易状态。

S04涉及的诊断Logger与本批Audit冷导出错误报告分别获得针对性证据，但真实设备故障、Journal durable prefix、公开Feed闭环、并发生命周期及受控性能仍未全部验收。下一步收敛剩余M1门禁并明确M2公开行情schema/可见性，不继续向导出函数堆复杂框架。

面试可介绍：我把“文件能打开”与“导出成功”分开，保留既有生命周期CSV和调用入口，补上写入/flush/close错误报告；用受控流验证部分写入和完整字节但flush失败两类情形。Audit仍是可重新导出的业务视图，不是撮合回滚或持久化Journal。
