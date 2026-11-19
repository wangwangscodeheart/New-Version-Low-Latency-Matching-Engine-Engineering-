# 当前恢复模型（V2）

- Journal 保存可重放输入、派生成交和命令结果；Replay 只重新提交输入，再核对输出和最终状态。
- 单品种 Snapshot 使用版本化二进制格式、checksum、临时文件验证和原子替换。
- 多品种 Snapshot 先提交同代 book 文件，最后提交 manifest；恢复时先在旁路对象完整校验。
- Snapshot 恢复后仅回放 `journal_sequence > snapshot_sequence` 的后缀。
- Linux 路径对临时文件和 rename 后父目录执行 `fsync`；Windows 使用 `FlushFileBuffers` 与 `MOVEFILE_WRITE_THROUGH`。

边界：CSV Journal 不是生产级 WAL；项目没有复制、共识、双机热备或跨设备原子替换保证。

## 多品种generation非复用（2026-11-14补充）

保存前检查所有Book输出屏障及整代最终/临时路径，再创建`<manifest>.g<sequence>.reserved`目录。占位在成功/失败后都保留，保护同一manifest命名空间内的generation不被复用；旧v1加载不要求占位。不要删除占位后直接重试，更不能凭空递增真实Journal截点。没有新有效截点时，可以选择另一个由控制面管理的manifest路径；入口切换/GC未自动实现。

同一manifest的保存须在单写者静止点串行执行。新代在提交前失败的已测场景不污染旧代，但保存异常不必然代表未提交：已有POSIX实现可能在rename后的目录刷新阶段抛错。失败后重新检查正式manifest及引用文件，不能假定全局回滚。断电/强杀、durable prefix、截点绑定仍未验收，详见[M1第八批](M1_BATCH_08.md)。

# 进程内待发事件与恢复边界（2026-11-12补充）

OrderBook的PENDING必须向同一逻辑输出流续送，不重执行Command；非ready禁止快照，多簿写前预检全部簿。FAILED接受状态不明，不自动重试，控制面还要检查Engine.failed与Journal状态，不能仅凭book.ready证明整体可恢复。outbox不跨崩溃保留。后续[M1第三批](M1_BATCH_03.md)已加入标准CommandJournal整命令预留/接受及v2完整组验证；新格式宽容EOF丢整组，旧legacy不作同等承诺。durable prefix、CRC/fsync和快照截点绑定仍待后续实现。

2026-11-14的[M1第五批](M1_BATCH_05.md)验证了部分成交后新价格节点分配失败：未挂簿主动单被清理，结构不变量成立，但已发生的成交不回滚，命令完成序号未推进，Book/Engine仍FAILED。不能仅凭结构有效继续接单、保存快照或重投失败命令；本批没有新增自动恢复协议。

