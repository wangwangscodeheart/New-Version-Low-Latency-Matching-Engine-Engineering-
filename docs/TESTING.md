# 当前测试体系（V2）

2026-11-19最新验证为M2公开行情闭环：新增encoder、committed_handoff、public_feed三个CTest，最终MSVC Release/ASan均27/27，额外100万命令差分通过；消费者分配扫描50次失败注入通过，见[M2记录](M2_PUBLIC_FEED.md)及[原始证据](validation/2026-11-19-m2-public-feed/)。独立allocation_failure程序带new/delete替换的ASan限制继续保留，其余26个程序不受该替换影响。下文24/24是11月18日历史口径。

- Public Feed：手写大端黄金、137字节帧串的138个切点、非法输出、提交后完整批次、多品种投影、公开引用、部分/全部成交/IOC/Post-Only/撤单、队列满及消费异常失效。
- Consumer Allocation：两侧新/已有档位Add回滚、sequence observe/apply异常后STALE、检查点全部分配点的事务恢复；不声称已穷尽Publisher及全系统分配故障。

2026-11-18最新验证补Audit冷路径导出错误报告，Journal/Audit/Replay target由5组增至10组；MSVC Release/ASan仍各24/24通过，enum_boundary_v2生成器100万命令差分通过，详见[M1第十批与原始记录](M1_BATCH_10.md)。前批Logger、快照、池和风险回归保留。[第五批](M1_BATCH_05.md)引入的独立分配故障程序含new/delete替换，ASan分配族诊断覆盖有限；其余23个程序不受替换影响。历史结果保留原口径；Linux/UBSan/TSan本机未执行。

- Unit 与属性测试：交易规则、FIFO、撮合、撤单、容量和不变量。
- Reference Differential：默认25个固定种子×4,000条命令，enum_boundary_v2从原GTC分段中取部分输入注入非法Side/TIF；逐命令比较状态、事件、不变量，每25步及最终比较完整盘口。旧80%/10%/10%为此前未注入非法枚举的分布口径。
- Recovery：Journal、单品种磁盘 Snapshot、多品种一致性 Snapshot、损坏拒绝和增量 Replay。
- Snapshot Generation：同代拒绝、legacy最后路径碰撞、占位独占、manifest提交前失败保留旧代、失败代不复用；不等于强杀/断电验收。
- Infrastructure：事件订阅、异步 Journal/Logger、Monitor、风控、Feed Decoder、Sequence Gap、Trade 扣减和行情检查点。
- Logger Failure：门控短写/抛异常、flush/header失败、精确满队列、借用流/关闭职责、生产者复制bad_alloc、Monitor错误状态；不等于真实磁盘满或并发stop验收。
- Audit Export：原生命周期CSV黄金输出，12种写入/4种flush错误、open错误、重新导出及后续撤单；实际close失败未注入，不等于原子保存或持久化验收。
- Risk Boundaries：精确整数bps/仓位区间、MIN绝对值与敞口重算、同侧跨档累计量、状态失效及可信来源完整重绑。
- Allocation Failure：新价格节点定点bad_alloc、池/索引清理、部分成交保留与FAILED、Journal abort，以及预留事件批次/续送不分配。
- Order Pool：重复/跨池/非槽位归还、容量与LIFO、不可拷贝、带活动槽位移动，以及Book移动后成交/撤单/补满容量。
- CI 配置：Linux Release、ASan+UBSan、TSan 子集及 benchmark smoke；只有实际 runner 结果才算 CI 证据。

2026-08-07 已实际运行 MSVC ASan RelWithDebInfo 全量测试并通过 20/20。Linux Release、UBSan 与 TSan 因本机没有 Linux 发行版或容器工具而未执行，详见 [`VALIDATION.md`](VALIDATION.md)。

本地基线以根 README 中最近一次明确记录为准，不沿用 V1 历史文档中的 4/4 结果。
