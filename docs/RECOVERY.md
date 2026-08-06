# 当前恢复模型（V2）

- Journal 保存可重放输入、派生成交和命令结果；Replay 只重新提交输入，再核对输出和最终状态。
- 单品种 Snapshot 使用版本化二进制格式、checksum、临时文件验证和原子替换。
- 多品种 Snapshot 先提交同代 book 文件，最后提交 manifest；恢复时先在旁路对象完整校验。
- Snapshot 恢复后仅回放 `journal_sequence > snapshot_sequence` 的后缀。
- Linux 路径对临时文件和 rename 后父目录执行 `fsync`；Windows 使用 `FlushFileBuffers` 与 `MOVEFILE_WRITE_THROUGH`。

边界：CSV Journal 不是生产级 WAL；项目没有复制、共识、双机热备或跨设备原子替换保证。

