# M3第四批：时间范围、成交关联与离线检查入口

本页保留2026-11-22第四批历史范围（当时无gzip/hash）；2026-11-23后续增加的文件完整性以[第五批说明](M3_ITCH_INPUT_INTEGRITY.md)为准，不覆盖本页原始验收证据。

日期：2026-11-22（Asia/Tokyo）。在[第三批历史22种消息](M3_ITCH_METADATA.md)之上增量实现；不改V1撮合、MarketManager、既有Journal/Audit/M2发布和显示簿规则。仍然只处理自行生成的测试数据，真实市场记录未下载、未交给开发工具读取。

## 改了什么，为什么

| 模块 | 增量及原因 | 真实组件 / 面试说明 |
|---|---|---|
| `itch_session_validation.hpp` | 精确Gregorian日期解析、日内纳秒范围、时间倒退计数/拒绝；拒绝不提交状态 | 数据接入上下文校验；日期不是从ITCH凭空恢复，也不当作网络序号 |
| `itch_trade_ledger.hpp` | 有界Match去重、E/C/P Broken关联及墓碑、净可打印数量溢出保护 | 市场成交报告关联，不是账户持仓/资金或生产交易账本 |
| `itch_book_rebuilder.hpp` | 显式验证配置；先验证执行量，再登记报告，最后扣显示量；时间观测暂存成功后提交 | 订单簿与报告一致性边界，失败后整会话STALE |
| `itch_inspect.hpp` / `itch_inspect_main.cpp` | 离线CLI启用上述检查；读取前验证参数与本地使用声明，拒绝gzip，不导出原始记录 | 可复现历史文件诊断入口；不是数据授权或真实性证明 |
| 四个新测试目标、help CTest与独立故障增量 | 时间/报告/重建/入口分开测；故障时不扣错量或返回成功 | 需求到测试到原始输出的阶段证据 |

## 验证范围必须显式选择

旧`ItchBookRebuilder(limits)`默认仍是此前的可见簿/元数据profile，不要求完整系统阶段、日内时间范围或Match关联。这样保留既有合成回归和调用语义；**不能用旧默认入口宣称报告已校验**。新检查不隐藏在撮合核心，也不自动升级之前已经生成的证据。

```cpp
nasdaq_itch::RebuildValidation validation;
validation.validate_intraday_timestamps = true;
validation.timestamp_regression_policy = nasdaq_itch::TimestampRegressionPolicy::COUNT_ONLY;
validation.require_all_system_events = true;
validation.correlate_trade_reports = true;
nasdaq_itch::ItchBookRebuilder session(nasdaq_itch::RebuildLimits{}, validation);
// session.read(authorized_uncompressed_input);
```

`validation_config()`说明当前选择；未启用或STALE时，`trade_ledger()` / `temporal_metrics()`返回空。离线CLI默认启用日内范围、Match关联及六阶段检查。库的可选配置与工具的默认检查不同，不能混称“每个入口都必须经过严格报告校验”。

## 日期与时间语义

- `TradingDate::parse`只接受ASCII `YYYY-MM-DD`、1900..9999的合法Gregorian日期，处理世纪/400年闰年规则。它不证明文件属于该日、不校验交易所开市日或来源；直接聚合构造的原始值不是校验入口。
- 目标日内域为`0 <= ns < 86400 * 10^9`；边界和全宽u48非法值在应用订单/报告之前拒绝。Decoder仍保留原始全宽整数。
- 倒退相对上一个成功观测；相等允许。默认只计数，保持原文件顺序，CLI输出warning；显式REJECT使会话失效。不是时区/UTC/DST/闰秒转换，也不把倒退或Tracking变化冒充网络丢包。
- 时间观测先复制暂存，记录业务应用成功后再提交；被拒绝记录不推进已接受观测/统计。
- 严格系统事件选项要求O/S/Q/M/E/C六码依次出现；S/E后仍可收到D/B，S/C加BinaryFILE终止及finish才结束。六码出现不等于完整交易时段业务规则都正确；未实现全部盘前/盘中/收市时序校验。

## 报告关联，不恢复显示订单

依据[官方TotalView说明及FAQ](https://data.nasdaq.com/databases/NTV)的逐执行Match标识，以及[历史ITCH规范](https://www.nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/NQTVITCHSpecification_5.0.pdf)的成交/Broken字段，在本profile定义：

| 消息 | 报告 | 显示簿 |
|---|---|---|
| E | 记录原挂单价、执行量、Match、所属Locate；可打印 | 正常减量 |
| C | 记录独立执行价和printable标志；N不计净可打印量 | 仍按原挂单价减量，不移动价位 |
| P | 非显示报告，可打印；引用0不进入订单索引 | 不变 |
| 正成交量Q | 保存Cross报告，可打印 | 不变 |
| 零成交量Q | 通知，不登记成执行，不占报告容量 | 不变 |
| B | 必须已观察同股票、未broken的E/C/P Match；保留不可复用墓碑并扣减对应净可打印统计 | **不恢复已经成交的挂单** |

Match身份跨股票、跨已broken记录唯一；不假设单调递增。未知/重复报告、错股票B、重复B、容量或统计溢出均拒绝并使所在重建会话STALE。独立ledger自身的非法/分配失败操作保留原报告和统计。

Q报告可保存但**Q的Broken不在本profile支持范围，遇到时拒绝**，不是宣称所有交易所/所有协议年代都禁止Cross更正。实际样本出现该情况时须重新核对规则，不能静默忽略或假装还原订单。非打印C的B只标记broken，不从本来未计入的可打印量扣减。

`reports`为累计已登记数，含broken；`broken_reports`为累计已关联撤销数；`printable_reports/volume`为本口径扣除已broken后的净报告数/股份数。它们不是官方时间销售认证、金额/notional、账户成交、资金、持仓、PnL或跨来源合并成交量。尚无独立真实报告参考对账。

默认`maximum_trade_reports=1,000,000`，可在库配置或CLI的`--max-reports`修改；broken墓碑也占容量，不删除以腾位置后允许复用身份。这是条目上限，不是预分配、总堆预算或性能承诺。

执行报告的所有权/数量在入账之前校验；ledger节点分配失败不会先扣显示量。成功入账后调用已验证的无分配减量；意外状态错误仍停止整个会话，不宣称可回滚先前记录。借用的报告/簿指针在失效后仅供诊断，不是可信线上视图。

## 离线入口与权限边界

```bat
build\itch-report-release\matching_engine_itch_inspect.exe --help
```

只有**确已核清许可**后，用户才可在本地执行类似命令：

```bat
build\itch-report-release\matching_engine_itch_inspect.exe --input data/local/session.bin --date 2020-01-30 --authorized-local-use --max-reports 1000000
```

`--authorized-local-use`是调用者声明，**不授予、不验证、不代替数据授权**。程序在任何文件检查/打开前先确认参数、声明与日期，不下载、不登录、不付费、不解压、不生成输出数据文件；拒绝.gz/.GZ及gzip签名。MSVC Windows入口用UTF-16参数转UTF-8，文件路径用filesystem处理；本轮已实测中文合成路径。

默认输出仅为本地汇总，包括声明日期、记录数/品种数/活动单、报告统计和时间倒退数；不dump原始payload或逐订单行情。真实数据的汇总仍可能受派生数据许可约束，不应自动公开或交给云端AI。程序明确输出：

```text
completion=PROFILE_COMPLETE_NOT_REAL_DATA_ACCEPTANCE
date_authenticated=false
checksum_computed=false
```

参数错误/缺声明返回2，IO/解析/业务/输出失败返回1，配置profile处理完成返回0。输出write/flush失败可能留部分文本，必须结合返回码判定；部分文本不代表验收成功，API也覆盖开启ostream异常的help/诊断场景。

可选`--reject-time-regressions`拒绝倒退；`--allow-missing-system-phases`放宽六阶段检查，并在摘要明确输出false。后者不关闭时间域和报告关联。工具没有文件SHA、gzip CRC、内容认证、快照一致性/文件并发修改保护或自动恢复；成功不证明没有被完整删掉的消息。

## 测试与剩余门禁

新增ledger六组、日期/时间五组、严格会话七组、CLI两组及help CTest。覆盖边界/关联/容量/全宽溢出、不可逆B、价量差异、默认兼容与显式检查、任意分片/逐字节/istream、临时合成文件、缺声明、日期、gzip、截断、中文路径、输出短写/flush/异常。

新定向E报告分配扫描同时核对旧显示量、账本、观测和计数不变，合法B执行时无分配且不复活订单。这不穷尽全链路分配点；独立new/delete替换程序的ASan局限保留。旧35项测试仍保留，不以修改旧fixture来假装默认行为已严格化。

构建和运行：

```bat
chcp 65001
cmake -S . -B build/itch-report-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/itch-report-release --parallel 6
ctest --test-dir build/itch-report-release --output-on-failure
cmake -S . -B build/itch-report-asan -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DENABLE_ASAN=ON -DENABLE_UBSAN=OFF
cmake --build build/itch-report-asan --parallel 6
ctest --test-dir build/itch-report-asan --output-on-failure
```

实际结果和源码身份见[验证记录](VALIDATION.md)及[原始证据](validation/2026-11-22-itch-report-validation/)。仍没有真实授权样本、压缩文件完整性/哈希、全日/交易所独立参考、生产成交流/账户语义、网络恢复或受控原生性能证明，M3没有整体验收。没有新FOK/并行撮合等扩展。

面试介绍：**我把历史字段解码、显示簿更新和市场报告关联分层，并显式记录验证配置。离线入口能拒绝无声明输入和错误数据，但日期标签与成功处理不等于数据授权或真实交易所验收。**
