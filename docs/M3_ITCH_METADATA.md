# M3第三批：历史元数据覆盖与状态分层

后续状态：同日[第四批](M3_ITCH_REPORT_VALIDATION.md)已增加可选报告/时间/系统阶段检查和离线入口；本文保留第三批默认仅计数等范围及35项证据，不代表最新全部能力。默认兼容入口仍不自动启用新保证，真实数据仍未取得。

日期：2026-11-22（Asia/Tokyo）。在[第二批接收端](M3_NATIVE_REBUILD.md)增量补Y/L/V/W/K/J/h/I/N，不改V1撮合、M2公开发布或显示簿生命周期规则。

## 本批结果，不是什么

解码器现有22种历史wire消息的typed payload，原13种的字段和前12个payload索引保持；A/F共用一个Add类型。新9种有精确长度、大端全宽整数和自有定长文本，不再只有Unsupported占位。未知类型仍默认拒绝，显式报告模式仅供解码诊断；重建器不跳过未知后继续宣称状态可信。

这是**历史布局与有限字段策略的合成测试覆盖**，不是“全版本ITCH业务已完成”。尚未取得Nasdaq原始记录、gzip校验、独立参考、真实交易日或原生链路性能。来源清单仍记录[下载0、SHA空、授权待确认](../data/sources/m3_candidate_review.json)。不自动下载、建号、付费、联系提供方或上传真实/可逆市场数据。

## 修改、设计原因与面试说明

| 文件/模块 | 改了什么、为何如此 | 真实组件 / 面试讲法 |
|---|---|---|
| `itch50_decoder.hpp` | 新9种typed payload、显式u64 Price(8)与u32 Price(4)，保留原始零/全宽值 | 协议层只恢复字段，避免浮点精度丢失或把语义校验塞进字节读取器 |
| `itch_book_rebuilder.hpp` | 最新股票元数据、按MPID的位置表、按场所停牌槽位、全市场状态；正确区分K/V/W路由 | 接收端控制/状态信息与显示订单簿分层；不生成私有撮合Command |
| 原核心测试及两个新增CTest | 历史字段黄金、物理短缓冲区、全部22种混合流、状态隔离和错误失效 | 不让“能解字段”冒充“能正确使用状态”，也不拿真实数据当可公开fixture |
| 独立native allocation测试 | 补L新增节点失败与已有MPID零分配更新验证 | 元数据分配失败也会使会话STALE，不能只保护订单节点 |
| 来源、README与验证文档 | 区分布局覆盖、有限语义、真实数据和完整业务验收 | 解释清楚已完成层与证据不足层，不沿用旧性能口径 |

## 新增布局与路由

所有长度不含BinaryFILE的2字节前缀，共同ITCH头11字节；以下为零基payload偏移。

| 类型/长度 | 字段 | 保存方式 |
|---|---|---|
| Y / 20 | stock@11:8, action@19:1 | 每股票最新Reg SHO状态 |
| L / 26 | MPID@11:4, stock@15:8, primary@23, mode@24, state@25 | 每股票按MPID保存最新位置 |
| V / 35 | levels:u64@11/19/27，Price(8) | 全市场最新熔断阈值 |
| W / 12 | breached_level@11 | 全市场最新触发级别 |
| K / 28 | stock@11:8, release_seconds:u32@19, qualifier@23, price:u32@24 | **头Locate=0，按正文股票查目录** |
| J / 35 | stock@11:8, ref/upper/lower:u32@19/23/27, extension:u32@31 | 每股票最新重开竞价区间 |
| h / 21 | stock@11:8, market@19, action@20 | Q/B/X三个场所分别保存 |
| I / 50 | paired/imbalance:u64@11/19, direction@27, stock@28:8, prices:u32@36/40/44, cross@48, variation@49 | 每股票最新NOII，不改可见量 |
| N / 20 | stock@11:8, interest@19 | 每股票最新零售兴趣指示 |

依据[官方2015版规范](https://www.nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/NQTVITCHSpecification_5.0.pdf)，结合[2020年前J/h修订记录](https://www.nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/NQTVITCHSpecification.pdf)。V/W的Locate固定0；K同样为0但正文带股票，不应走普通非零Locate路由。其他新股票消息必须已有R目录，Locate和正文股票都匹配，不能凭未知股票自行创建簿。第一批PDF获取失败保留为当时记录，本轮可检索到官方完整字段表；仍无冻结2018副本或真实2020会话兼容性证明。

## 应用策略及特殊值

- Y允许历史action 0/1/2，仅记录状态，不代替监管检查或风险准入。
- L校验primary Y/N、mode N/P/S/R/L、state A/E/W/S/D；MPID允许右侧空格填充，但不能为空或含内嵌空格/NUL。状态D仍作为最新记录保留，不删除该MPID来规避容量。
- V保留完整64位Price(8)，刻度1e8，不套股票Price(4)上限、不转double、不推断三个值必须如何排序。W校验级别1/2/3，不据此自动清空或冻结所有订单簿。
- K的释放时间单位是秒/午夜，不是共同头的纳秒。当前策略范围0..86399，A允许零；C取消/推迟时要求时间和价格均为0。价格不超过历史Price(4)范围，K只更新预期信息，不执行IPO竞价。
- J只检查Price(4)上限，保存零或未排序字段，不擅自推断重开算法；extension是**次数**，不是时长。没有参考价区间一致性或交易规则全部通过声明。
- h校验市场Q/B/X及动作H/T，按市场独立保存，不让一个市场的恢复覆盖其他市场的停牌，不把它等同于H全股票交易动作。
- I保留零价/零量（可能无法计算），检查历史direction B/S/N/O、Cross仅O/C/H、variation L/1..9/A/B/C/空格及Price(4)上限。**它不是Q的O/C/H/I集合**；不接入2022的Extended Close A或2023的Paused方向P，也不把imbalance当已成交/未成交委托。
- N允许B/S/A/N，作为遗留布局支持；2015修订记录说明RPI计划已结束，支持此布局不代表已观察2020样本出现它。

新元数据只改变相应状态槽和成功类型计数，不能改变现有订单、价格档、BBO、Active/Seen引用或生成Trade。所有元数据保存的都是**最新body**，不是带完整接收时刻的历史序列、有效交易权限或freshness检查；未观察到的optional保持空，不填造默认正常状态。

## 容量与失败契约

新增`maximum_market_participants_per_instrument`，默认1024且须大于0。它限制每股票保存的不同MPID（含Deleted），不是总内存字节预算。满额时允许已有MPID更新，新MPID终止会话，不能静默驱逐旧状态。新map节点分配失败保留原映射；已存在项为固定字段赋值。

解析、路由、字段、容量或分配错误仍使整个重建器STALE；`metadata()`与`market_wide_metadata()`和`book()`一样拒绝失效会话。之前借出的指针只能做诊断，调用方不能缓存它们当作错误后可信状态。单所有者、不可复制/移动、禁止重入、未知类型拒绝、S/C与BinaryFILE共同结束等[第二批契约](M3_NATIVE_REBUILD.md)保留。

元数据可读取不代表业务就绪；必须另查`state()`。V/W可在R前接收，但仍要求S/O先行；K需要股票目录已建立。没有全日状态时序、跨消息监管约束、交易日/时区/freshness或完整Match/Broken账本验证。

## 使用

```cpp
// session.read(input) 或 consume(...)/finish() 后，先检查session.state()。
if (const auto code = session.locate("AAPL")) {
    if (const auto* info = session.metadata(*code)) {
        // info->reg_sho, market_participants, ipo, luld,
        // operational_halts[0/1/2] = Q/B/X, imbalance, retail。
        (void)info;
    }
}
const auto* market = session.market_wide_metadata();
// market可能为空（STALE）；其中optional可能为空（尚未观察）。
(void)market;
```

复现（VS x64 Developer cmd）：

```bat
chcp 65001
cmake -S . -B build/itch-metadata-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/itch-metadata-release --parallel 6
ctest --test-dir build/itch-metadata-release --output-on-failure
cmake -S . -B build/itch-metadata-asan -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DENABLE_ASAN=ON -DENABLE_UBSAN=OFF
cmake --build build/itch-metadata-asan --parallel 6
ctest --test-dir build/itch-metadata-asan --output-on-failure
```

新decoder四组、元数据会话七组合成回归，覆盖逐字段黄金、非对齐、所有权、全宽/零、物理截断、全部22类型混合流的任意切点/逐字节/istream、特殊全市场/股票路由、合法/非法枚举与MPID容量。每次元数据更新对照显示簿不变；文件尾/未知消息仍拒绝。L新节点分配扫描与已有MPID更新的定向验证只覆盖此程序/路径，不是全链路零分配承诺。

完整结果见[验证记录](VALIDATION.md)及[本批原始日志](validation/2026-11-22-itch-metadata/)。两个独立故障程序使用new/delete替换的ASan局限继续保留，不能把CTest通过当作全分配族或Linux sanitizer证明。

面试可说明：**我先分开显示订单更新与控制元数据，再按历史协议处理作用域和特殊值。22种布局已用合成流接通，但真实会话、完整业务约束和性能仍须独立验收。**
