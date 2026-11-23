# M3第五批：单遍文件SHA-256与可选gzip完整性

日期：2026-11-23（Asia/Tokyo）。在[第四批离线检查器](M3_ITCH_REPORT_VALIDATION.md)上增量接线，不改V1撮合、Journal/Audit或M2模拟发布；不把外部ITCH转成撮合指令。输入仍全部自行构造，没有下载、读取或上传真实市场记录。真实授权、交易日和独立参考验收仍未完成。

## 改动与职责

| 模块 | 修改内容及设计原因 | 对应组件与面试介绍 |
|---|---|---|
| `src/sha256.hpp` | 固定块流式SHA-256，先检查长度溢出；finish幂等、finish后禁止update | 离线数据指纹。说明摘要是字节身份，不是签名、来源认证或授权 |
| `src/streaming_gzip.hpp` | 默认关闭的zlib适配；逐个验证拼接member的CRC/ISIZE，拒绝截断和尾随垃圾 | 数据接入完整性层。不用会忽略尾随垃圾的便利读接口，不把ISIZE当64位总长度 |
| `src/itch_verified_input.hpp` | 对同一份输入单遍hash并可选解压，再调用既有重建器；整个操作使用原单写者/重入保护 | 接入协调器，避免先hash再重开文件的两遍读取竞态；不是文件快照或恢复服务 |
| `src/itch_book_rebuilder.hpp` | 仅授予上述协调器访问现有guard和私有读取器；不开放强制恢复/伪造完成接口 | 复用已有状态边界；连预先吃掉一个长度前缀字节的会话也不能冒充fresh |
| `src/itch_inspect.hpp`、CMake及测试 | CLI新增压缩、预期hash、字节/member限额；显式ON才查找已安装zlib，不自动下载 | 可复现诊断入口。将能力、启用条件和验收证据分开，不宣传完整生产接入 |

```text
授权声明/参数/日期检查
  → 单次打开本地文件 → 4KiB原始读取 ┬→ SHA-256（原始源字节）
                                 └→ none / streaming gzip
                                      → BinaryFILE → ITCH → 原独立行情簿
物理EOF → 全部gzip尾部 → expected hash（若提供）
        → BinaryFILE finish → session finalize → 摘要
任一失败 → STALE / 非零退出 / 不接受结果
```

SHA覆盖实际读取的原始源字节；gzip头、压缩内容、所有member及尾部均包含在摘要里，**不是解压后字节**。不seek、不重新打开、不生成解压文件。调用者若让文件并发变化，摘要只标识实际读到的流，仍不保证某个时刻的原子文件快照。`istream`接口面向阻塞式文件流，不是非阻塞网络协议。

gzip可能在CRC错误被发现前就输出前缀。此时会话只能是暂态，失败后整体STALE且新查询不提供可信簿；不承诺回滚已应用前缀、已有借出指针或自动恢复。只有物理EOF、所有member、hash（若有）、BinaryFILE标记和会话验证全部成功才COMPLETE，绝不在单个member或S/C处提前完成。

## 配置与使用

默认`ENABLE_ZLIB_INPUT=OFF`：无新增外部依赖，普通BinaryFILE可计算SHA；gzip输入明确失败，不伪装处理成功。手动准备可信zlib开发包后才启用：

```powershell
cmake -S . -B build/itch-integrity -G Ninja -DCMAKE_BUILD_TYPE=Release `
  -DENABLE_ZLIB_INPUT=ON -DZLIB_ROOT="<本地zlib安装前缀>"
cmake --build build/itch-integrity --parallel 4
ctest --test-dir build/itch-integrity --output-on-failure
```

Windows Ninja需先进入对应MSVC开发环境并设`chcp 65001`。本轮验证使用静态zlib1.4，但代码只使用常规inflate接口，并未承诺所有版本/平台已验证。项目不自带第三方源/库，不联网拉依赖，也不机器级安装。

旧CMake的FindZLIB可能不识别新版静态库名`libzs`或`ZLIB_USE_STATIC_LIBS`提示；此时显式传`-DZLIB_INCLUDE_DIR=<prefix>/include -DZLIB_LIBRARY=<prefix>/lib/libzs.lib`，而不是关闭检查或宣称已在最低版本工具链复验。

只有用户已经取得本地使用权时才能运行真实文件：

```powershell
build/itch-integrity/matching_engine_itch_inspect.exe `
  --input "<已授权本地文件>" --date 2020-01-30 --authorized-local-use `
  --compression auto --expected-sha256 "<独立可信渠道取得的64位十六进制摘要>"
```

`--authorized-local-use`只是调用者声明，不授予或验证授权；命令是用法模板，本项目没有执行真实文件。`--date`也只是上下文标签，不认证文件交易日。

- `--compression auto|none|gzip`：auto按gzip魔数识别，CLI的`.gz`/`.GZ`后缀也声明必须gzip；none显式覆盖后缀，仍须通过BinaryFILE/ITCH校验。gzip未启用时拒绝，不降级。
- `--expected-sha256`：恰好64个hex，大小写不敏感；可选，缺省仅计算，不声称校验了外部预期值。格式/重复参数在任何文件访问前拒绝。自己计算再填回去不是独立认证。
- `--max-input-bytes`：默认64GiB，原始源字节；`--max-decoded-bytes`：默认256GiB，解压/直通字节；`--max-gzip-members`：默认1024。均正整数、溢出/非法/重复参数拒绝，超限失败而非截断接受。
- 原有`--max-reports`、时间倒退和系统阶段选项保留。各项上限不是总堆预算；SHA与适配层缓冲固定，zlib窗口、簿/引用/报告容器和流仍会分配。没有时间预算或解压吞吐保证。

成功摘要包含`source_sha256`、`hash_domain=raw_source_bytes`、`expected_sha256_verified`、压缩方式、输入/解码字节数、member数和`gzip_crc_and_isize_verified`（普通文件为not_applicable）。始终保留`source_authenticated=false`、`date_authenticated=false`及`PROFILE_COMPLETE_NOT_REAL_DATA_ACCEPTANCE`。非零退出时任何部分摘要均不接受。

## 验证范围

新增3个CTest：SHA已知向量、可选gzip封装、完整读取/重建协调器，已有CLI测试扩展；原40项保留。SHA包括RFC向量、million-a和独立.NET边界摘要；gzip覆盖拼接/空member、可选头/FHCRC、所有切点/逐字节、CRC/ISIZE、所有截断点、垃圾/错误wrapper、限额、异常/吞异常重入。集成覆盖压缩前后hash域、跨member拆记录、晚到错误、流异常掩码、预先消费字节与输出失败。测试不是市场数据，也不是密码算法认证。

实际配置、命令、结果、第三方来源和源码身份见[本批证据](validation/2026-11-23-itch-input-integrity/)及[验证记录](VALIDATION.md)。启用与禁用配置分开列，禁用构建只证明拒绝策略，不算CRC路径执行。ASan C++测试链接的zlib Release静态库本身未插桩；不把封装测试通过说成第三方库内部已完成ASan验证。

仍未覆盖：真实文件/许可/独立参考、完整业务与官方tape、加密认证、文件并发修改快照、网络序号恢复、强杀/断电、受控性能及新Linux/UBSan/TSan验证。M3只是第五批子项完成，不是整体验收。

协议依据：[RFC1952](https://www.rfc-editor.org/info/rfc1952/)、[zlib官方手册](https://zlib.net/manual.html)（inflate gzip模式、CRC/ISIZE和member reset）、[RFC6234](https://www.rfc-editor.org/rfc/rfc6234)（SHA-256）；本地依赖版本/摘要以[来源记录](validation/2026-11-23-itch-input-integrity/dependency-provenance.md)为准。
