#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${1:-${repo_root}/build/perf-release}"
cpu_core="${PERF_CPU_CORE:-}"
timestamp="$(date -u +%Y%m%dT%H%M%SZ)"
result_dir="${repo_root}/benchmarks/results/perf-${timestamp}"
mkdir -p "${result_dir}"

command -v cmake >/dev/null
command -v perf >/dev/null
command -v lscpu >/dev/null

cmake -S "${repo_root}" -B "${build_dir}" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build "${build_dir}" --target \
  matching_engine_benchmarks matching_engine_v2_benchmarks

{
  echo "utc_timestamp=${timestamp}"
  echo "git_commit=$(git -C "${repo_root}" rev-parse HEAD)"
  echo "kernel=$(uname -a)"
  echo "compiler=$(${CXX:-c++} --version | head -n 1)"
  echo "perf=$(perf --version)"
  echo "governor=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo unknown)"
  echo "turbo=$(cat /sys/devices/system/cpu/intel_pstate/no_turbo 2>/dev/null || echo unknown)"
  echo "cpu_affinity=${cpu_core:-not_pinned}"
  echo "background_processes_not_controlled=true"
  lscpu
} > "${result_dir}/environment.txt"

cp "${build_dir}/CMakeCache.txt" "${result_dir}/CMakeCache.txt"

run_command=()
if [[ -n "${cpu_core}" ]]; then
  command -v taskset >/dev/null
  run_command=(taskset -c "${cpu_core}")
fi

events="cycles,instructions,branches,branch-misses,cache-references,cache-misses,page-faults,context-switches,cpu-migrations"
for binary in matching_engine_benchmarks matching_engine_v2_benchmarks; do
  executable="${build_dir}/${binary}"
  "${run_command[@]}" "${executable}" > "${result_dir}/${binary}_raw.txt"
  perf stat -x, -r 5 -e "${events}" \
    -o "${result_dir}/${binary}_perf_stat.csv" \
    -- "${run_command[@]}" "${executable}"
  perf record -g -o "${result_dir}/${binary}.perf.data" \
    -- "${run_command[@]}" "${executable}"
  perf report --stdio -i "${result_dir}/${binary}.perf.data" \
    > "${result_dir}/${binary}_perf_report.txt"
done

echo "Performance evidence written to ${result_dir}"
