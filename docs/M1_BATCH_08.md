# M1 第八批：多品种快照generation禁止复用

日期：2026-11-14（Asia/Tokyo）。增量处理M0 C03，保留V1撮合核心、现有Book文件/manifest v1格式及文件名。没有新增恢复框架，不宣称M1/M3/V2.1整体完成。

后续：[M1第九批](M1_BATCH_09.md)补诊断AsyncLogger的错误可见性；以下24项结果及剩余事项保留本批归档口径。

## 1. 实际复现：旧manifest不变，恢复状态却变了

原save_atomic使用`<manifest>.g<snapshot_sequence>.<index>.book`，SnapshotFile会原子替换同名Book。重复使用序号时，旧manifest引用的文件在新manifest提交之前就被覆盖。

回归先保存三品种状态，再改变三本簿，在`<manifest>.tmp`位置创建目录，使保存确定性地失败于manifest打开阶段。旧实现实际输出：`manifest_unchanged=1 books_unchanged=0 recovered_old_state=0`；保存抛异常，但旧manifest仍可加载成非原来的状态。[红灯原始输出](validation/2026-11-14-m1-generation/generation-red.txt)。这是普通文件系统故障复现，不是kill/断电测试，也不是checksum未检测到任意字节损坏的测试。

## 2. 修改范围与ADR-014

状态：本节所述非复用与提交前故障保护已实现并验证。

| 文件 | 修改 | 设计原因 / 真实组件对应 |
|---|---|---|
| `src/market_snapshot.hpp` | 整代目标路径预检、不自动清理的.reserved占位、不可加载源形状的写前校验 | 保护已经发布的恢复基准，避免控制面保存失败污染旧代 |
| `tests/market_snapshot_tests.cpp` | 原2组扩为8组；隔离临时目录、逐字节比较、旧布局兼容与文件系统故障 | 验证原子发布前的文件不可变性，而不只测试一次正常往返 |

不修改SnapshotFile的单文件替换规则、load协议、Journal、Matching Engine、CMake或Benchmark。现有CTest target复用，总数仍24。

保存顺序：

```text
所有簿静止/ready、源形状合法
 -> 检查全部目标.book和.book.tmp路径均未占用
 -> create_directory(<manifest>.g<sequence>.reserved)
 -> 写入/验证新代Book文件
 -> 写入/验证临时manifest
 -> 替换正式manifest
```

路径预检在任何Book写入之前全部完成，不采用“检查一个、写一个”的方式；同时保护升级前没有占位目录的legacy文件。symlink_status把悬空链接也视作占用，不跟随链接后误判为空；本机未做符号链接故障实测，不扩大为恶意文件系统下的安全认证。

原子创建.reserved目录后，这个generation即被消耗。成功、失败都保留占位，绝不自动删除重用。即使状态完全相同，也拒绝对同一manifest命名空间、同一序号再次保存。源为空、超过现有loader的100,000品种限制、Book容量为0或manifest无有效文件名时，写前拒绝；尚未通过输出屏障也不会消耗generation。

这是保守的拒绝策略，不是幂等重试。序号仍同时用作外部提供的快照截点和存储generation，尚未解耦：**不能为了绕过占位凭空递增Journal截点**。有真实新截点时可使用其未用序号；没有新截点可选择另一个由控制面管理的manifest命名空间，入口切换/自动恢复流程不在本批。

调用者仍须在单写者静止点串行保存同一manifest。占位不提供不同generation并发保存的全局锁，也不支持不遵守协议的旧写者或外部进程同时改文件。

## 3. 结果与故障范围

多品种快照target共8组：

1. 原三品种快照与Journal后缀Replay；移除测试占位后照旧读取v1布局，证明load不依赖占位。
2. 原manifest损坏拒绝，不暴露半恢复Manager。
3. 同generation加manifest临时路径目录故障；修复后旧manifest、全部旧Book字节和恢复状态均不变。
4. 无故障时也拒绝同generation；新代可发布新状态，旧Book保持原字节。
5. 最后一个品种的legacy最终文件/临时文件碰撞；早先品种不落盘，碰撞文件不改写。
6. 只有占位目录或同名普通文件、没有Book时，也拒绝保存且不写Book。
7. 新代61写完Book、但manifest打开失败；旧代60仍可恢复。移除测试阻塞目录后仍拒绝复用61，未完成文件不改写；测试代62成功，旧代和孤儿文件均保留。
8. 空市场、零容量Book及无效manifest名在写前拒绝，不消耗generation；序号0本身仍支持。

测试中60/61/62等只是故障fixture标签，不代表验证了真实Journal截点。每个fixture以独占创建的系统临时目录隔离；清理只针对本fixture的绝对直接子目录，不删除生产快照或用户目录。没有给生产代码加故障钩子。

| 实际执行 | 结果 | 证据 |
|---|---|---|
| 全新MSVC Release / Ninja | 54/54构建成功，24/24 CTest通过，8.05s | [构建与全量测试](validation/2026-11-14-m1-generation/release-build-and-ctest.txt) |
| 全新MSVC ASan RelWithDebInfo | 54/54构建成功，24/24通过，13.22s | [构建与全量测试](validation/2026-11-14-m1-generation/asan-build-and-ctest.txt) |
| 独立快照回归和额外参考差分 | 8组通过；100种子×10,000命令通过，enum_boundary_v2 | [绿灯与100万差分](validation/2026-11-14-m1-generation/green-and-stress.txt) |

红灯之后扩充过测试，不声称红/绿阶段的整个测试文件逐字节相同。保留既有D9025、Benchmark C4244及ASan故障Sink C4702警告。本轮Linux/UBSan/TSan、真实磁盘满、刷新失败、强杀和断电未执行；独立分配故障程序的ASan限制仍见[第五批](M1_BATCH_05.md)。

VS x64 Developer cmd复现：

```bat
chcp 65001
cmake -S . -B build/m1-generation-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/m1-generation-release --parallel 8
ctest --test-dir build/m1-generation-release --output-on-failure
cmake -S . -B build/m1-generation-asan -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DENABLE_ASAN=ON -DENABLE_UBSAN=OFF
cmake --build build/m1-generation-asan --parallel 8
ctest --test-dir build/m1-generation-asan --output-on-failure
build\m1-generation-release\matching_engine_market_snapshot_tests.exe --generation-reuse-only
build\m1-generation-release\matching_engine_differential_tests.exe --seeds 100 --commands 10000 --seed-base 0xD1FF0000
```

## 4. 明确不作的保证

本批只证明上述**提交前**故障下旧代不被覆盖。不保证“任何保存异常都保留旧manifest”：现有POSIX路径在rename之后才刷新目录，后置刷新失败可能在已经发布后抛异常；Windows替换/刷新错误的最终可见状态也不能笼统当作回滚。失败后须重新检查磁盘上的正式manifest及相关文件，不得仅凭异常决定提交是否发生。

占位不是WAL commit frame或已验证的durable marker；没有测试断电后占位的持久性。存储ID与Journal真实完整命令水位的绑定、恢复服务编排、Feed快照拼接及跨设备原子性仍待M3。旧格式读兼容不等于允许旧程序并发写新目录。

未完成新代文件、临时文件、占位和历史代都会保留；没有自动GC、磁盘预算或孤儿清理工具。不要在不知道有效manifest引用关系时手动清空占位并复用原序号。保留本身不能冒充资源管理问题已经解决。

## 5. 性能、源码身份与回滚

本批新增的是快照控制面的路径检查/占位创建，不改撮合算法。没有测快照延迟或磁盘耗时；既有七场景Benchmark不覆盖MarketSnapshotStore，不重跑后套用为本批性能证据。测试总秒数也不是撮合/持久化延迟。[第七批](M1_BATCH_07.md)已记录的核心性能未归因事项仍开放。

基线仍为`c690dfd8b008e06690e914e272173a7f4e55d164`加未提交工作区。[八批累计源码patch](validation/2026-11-14-m1-generation/source_changes.patch)和[34项Git blob身份](validation/2026-11-14-m1-generation/source_git_blobs.txt)冻结当前编译输入及既有构建/脚本/证据属性；blob按Git文本入库规范化，不是任意checkout字节哈希。patch对基线index的`git apply --cached --check`通过，不改index；不要重复应用到当前工作区。

回滚仅选择本批路径/占位校验及快照回归diff，不还原market_snapshot整个文件而移除第二批输出屏障。不要自动删除已经生成的占位或孤儿文件，也不要删除有效manifest引用的旧Book。回滚后同代覆盖问题重新存在，必须同时撤销相关安全声明。

C03的同代覆盖故障已复现并针对性修复；ADR-004的完整持久化/拼接承诺未全部完成。后续继续检查剩余事件/恢复门禁，再进入M2公开行情闭环，不把本批包装成生产级崩溃恢复。

面试可介绍：我用一个普通目录阻塞manifest临时路径，复现了“保存报错、旧manifest不变，但旧状态已被覆盖”。修复保留原格式，先检查整代目标再独占占位，并拒绝generation复用；同时明确异常不必然代表未提交，重试序号也不能伪造真实日志水位。
