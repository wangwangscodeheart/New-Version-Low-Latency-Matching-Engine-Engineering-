# 当前验证记录

## 2026-08-07：提交 `a99dd15e6cad`

### Windows Release

- 工具链：MSVC 19.44、CMake/Ninja、C++17、Release。
- 结果：20/20 CTest 通过。
- 差分规模：25 个固定种子 × 4,000 条命令，包含 GTC、IOC 和 Post Only。

### Windows AddressSanitizer

- 配置：`RelWithDebInfo -DENABLE_ASAN=ON -DENABLE_UBSAN=OFF`。
- 构建：46/46 目标成功。
- 结果：20/20 CTest 通过，总测试时间 10.46 秒。
- 结论只覆盖 MSVC AddressSanitizer；MSVC 不提供本项目 CI 所配置的 GCC/Clang UBSan 与 TSan 等价验证。

### Linux / UBSan / TSan 状态

本机检查结果：没有已安装的 WSL Linux 发行版、Docker、GCC 或 Clang。因此本轮没有执行 Linux Release、UBSan 或 TSan，不能将 `.github/workflows/ci.yml` 的存在视为通过证据。它们仍需在真实 Ubuntu runner 上运行，保留 job URL/日志后再更新本节。

### Benchmark 证据

提交 `a99dd15e6cad` 的五个独立进程原始输出、环境元数据、逐轮汇总和 SHA-256 清单位于 [`benchmarks/results/2026-08-07-v2-baseline`](../benchmarks/results/2026-08-07-v2-baseline/)。采集环境为 Windows Balanced 电源计划、未绑核；结果展示环境抖动，不用于跨机器结论。

