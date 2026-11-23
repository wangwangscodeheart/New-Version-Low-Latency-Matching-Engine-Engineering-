# M3第六批：可选gzip跨平台验证与测试夹具隔离

日期：2026-11-23（Asia/Tokyo）。本轮只改CI与测试设施，不增加交易业务功能，不修改V1撮合、M2发布或第五批SHA/gzip/重建生产实现。原先四个未接线分配统计文件继续保留本地，不列入完成项。真实市场数据仍为0，M3整体验收不因此完成。

## 为什么需要这一步

第五批Windows三种配置已通过，但默认Linux CI未启用zlib，gzip测试只验证“未启用时明确拒绝”，并没有执行CRC/member正向分支。同时，多套构建一起跑测试时，旧Journal/生成器等夹具会读写全局临时目录下的固定文件名，导致锁冲突或断言失败。给各构建设置不同TEMP只是运行规避，不能保护同一可执行文件的多次直接运行。

这两个问题属于验证可靠性，不能拿重跑成功覆盖失败，也不应混称撮合系统具备并行能力。

## 增量改动

| 文件/范围 | 改了什么与原因 | 实际系统对应与面试介绍 |
|---|---|---|
| `.github/workflows/ci.yml` | Release、ASan+UBSan分别使用OFF/ON矩阵；仅ON安装系统zlib开发包，保留严格GCC警告；全量后verbose执行五项完整性测试 | 配置能力必须有对应执行证据。相同CTest数量不等于相同功能覆盖，需明确feature flag和测试输出 |
| `tests/temporary_test_directory.hpp` | 仅用于测试的RAII所有者；原子创建唯一目录、限制标签/直接子文件名、禁止拷贝移动；只清理登记文件与空目录，不递归扫描删除 | 隔离测试数据与可追踪资源所有权。唯一性由目录原子预留保证，不把时间戳当作不会碰撞的保证 |
| 八份旧测试文件 | 保留原来的文件格式、故障输入和业务断言，只把固定文件名放进自有目录；Snapshot登记自身`.tmp`副文件 | 修复夹具相互污染，不改Journal、Replay、订单簿或生产落盘行为 |
| `market_data_generator_tests.cpp`新增夹具断言 | 验证两个目录同名文件共存、兄弟不被清理、异常路径清理、非法路径拒绝及未登记文件保留 | 测试工具本身也要有正反例，不能靠“这次没有撞名”断言可靠 |

八份文件是`async_journal_tests.cpp`、`journal_replay_tests.cpp`、`market_data_generator_tests.cpp`、`command_journal_tests.cpp`、`market_data_tests.cpp`、`snapshot_tests.cpp`、`unit_tests.cpp`、`order_type_tests.cpp`。已独立预留目录的Logger、多品种Snapshot、公开发布与ITCH夹具不改。CTest数量保持43，不删除或跳过原断言。

## CI边界

- 默认构建仍无zlib依赖；只有矩阵ON才安装runner系统开发包并设置`ENABLE_ZLIB_INPUT=ON`，项目不联网拉取数据或依赖。
- 两个矩阵使用`fail-fast: false`，保留每种模式独立结果；其余TSan子集、Benchmark smoke及手动差分任务不改。
- 每种模式全量运行43项，再verbose记录`sha256`、`streaming_gzip`、`itch_verified_input`、`itch_inspect`、`itch_inspect_help`五项。ON输出明确执行gzip分支，OFF明确标记未执行正向CRC测试。
- runner的系统zlib C库和本机Release静态zlib都未为本次ASan/UBSan插桩；这些配置验证C++封装/应用，不是第三方C库内部完整sanitizer证明。既有两个new/delete替换故障程序的诊断限制继续保留。
- CI成功、完整性测试通过和并发运行测试程序，均不是吞吐量、尾延迟、生产恢复或多线程撮合证据；手动差分任务未触发时仍标记skipped。

## 实际结果与复验方法

已经完成的`05f003b`真实Linux结果与本轮工作区结果按提交区分，见[原始日志与说明](validation/2026-11-23-m3-ci-and-isolation/)和[验证记录](VALIDATION.md)。旧批默认OFF的43/43不冒充本轮ON结果，旧11月20日27项CI也不扩大到新代码。

```text
cmake --build build/<configuration> --parallel 3
ctest --test-dir build/<configuration> --output-on-failure
```

测试夹具修复后的验证同时保留普通全量回归及“多个独立进程共享同一个专用临时根”的回归，不再靠每配置分离TEMP来证明修复。本机仍没有Linux发行版，不安装WSL/全局工具链；Linux证据只来自实际GitHub runner。

辅助类不是安全沙箱，不承诺敌对进程替换路径下的无竞态清理。析构不抛异常；文件仍被占用、路径被替换或有未登记内容时允许保留现场，不扩大删除范围，也不声称断电/崩溃后无残留。测试生成/保留的文件都在本轮自行创建的目录，不触碰已有用户文件。
