# 当前限制（V2）

- 简化二进制 Feed 不是交易所协议；Gap 后会将品种标记为 STALE，但没有重传通道或外部快照服务。
- 风控状态是单账户教学模型，持仓和参考价由外围注入；没有账户层级、PnL、保证金或组合 Greeks。
- Event Dispatcher 与撮合保持单线程；没有 UDP、NUMA、多线程撮合或跨进程总线。
- 单条命令内部仍可能使用临时事件 vector，尚未证明完全无分配。
- Journal 是 CSV 教学格式，不是生产级 WAL；没有复制或高可用恢复。
- CI 文件只有在 GitHub runner 实际执行后才构成流水线证据；Gitee 不会自动运行 GitHub Actions。
- 历史 26.7 ns 仅代表旧提交的窄热点微基准，不代表当前 V2 端到端延迟。

