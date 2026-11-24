# Separate public-feed C++ allocation instrumentation evidence

Date2026-11-24, Asia/Tokyo. Baseline045c7a234349630e211a11ae245aeb022488415f
plus pending measurement files. No production src/, existing latency .cpp or
public-feed Fixture change; no market data downloaded/read. [Scope](../../PUBLIC_FEED_ALLOCATIONS.md).

## Actual Windows executions

MSVC19.44.35228.0 x64, C++17, CMake4.4.2, Ninja; code page65001.

| Output | Result |
|---|---|
|release-instrumented-build-and-ctest.txt|Fresh98-step non-sanitized Release,45/45,15.54s|
|release-instrumented-final-retest.txt|4-step probe rebuild after metadata scope correction,45/45,5.68s|
|release-default-retest.txt|Existing default tree reconfigured/rebuilt build identity,43/43,9.91s|
|asan-default-retest.txt|Existing ASan/gzip ON tree, probe OFF,43/43,19.16s|
|strict-allocation-meter.txt|/O2 /W4 /WX meter +8 groups +24 failure/handler combinations passed|
|strict-allocation-companion.txt|/O2 /W4 /WX companion compiled and ran; not the official captured binary|
|configuration-rejections.txt|ASan/UBSan/TSan options and Debug each rejected as expected|
|collector-contract-tests.txt|Initial14 mock protocol cases passed; not performance evidence|
|collector-final-contract-retest.txt|Archived parameterized harness14 cases passed|
|capture-three-processes.txt|Real collector completed3 independent companion processes|

Two optional targets link allocation_meter.cpp; only the unit target defines
ALLOCATION_METER_TEST_FAILURE_INJECTION=1. The companion has no failure branch.
Default targets and normal sanitizer tests do not acquire this replacement.
CTest remains43 whenOFF,45 whenON; not a new sanitizer-coverage claim. Existing
two allocation-fault executables retain their prior ASan diagnostic limits.

Strict flags: /std:c++17 /EHsc /MD /O2 /W4 /WX /utf-8 /I src /I benchmarks,
companion and meter.cpp linked together; meter test additionally macro1. Source
flags metadata is resolved after CMake's build-type overrides and labels its
limited global/Release/directory-options scope; definitions/includes excluded,
C++ standard separate. Earlier generic latency metadata remains unchanged.
Warnings in full builds are not silenced or advertised as all-project /WX.

## Capture and repeatability

[Results directory](../../../benchmarks/results/2026-11-24-public-feed-allocations/)
has3 stdout/3 stderr files, allocation_summary.csv, environment.json and a
checksums.sha256 listing8 payload files. All processes exited0, each emitted
exactly12 validated scenario/phase rows after state/worker health checks. Each
scenario has60000 commands,67500 public messages,3210000 wire bytes. Workload
context repeats in4 phases; do not sum it. Numeric totals are scoped observed
successful hook calls/request-size arguments, not total logical allocations,
frees, surviving bytes, RSS, heap peak or workers. No elapsed time is output.

These are newly measured synthetic fixtures, NOT provider data. Windows
Balanced, no affinity/frequency controls, no warmup study or controlled A/B.
Source was baseline045c7a2 plus uncommitted files; do not attribute these
results to the old commit alone. Executable SHA-256 at capture:
be17cf873087792ad9e11069c779f53dd95fd4b2f93ba2108d563d77c43e2037.
No rebuild of that binary after capture is silently substituted.

Collector SHA values identify its stored decoded-text captures, not original
pipe bytes or provider authenticity. Script SHA identifies the local capture
worktree bytes; source_git_blobs.txt instead respects Git normalization.
Benchmark result files already have -text attributes. The manifest excludes
itself. Changing source checkout EOL does not alter the captured result bytes.

From an MSVC x64 environment, commands in the design doc build/capture3 fresh
processes into a NEW directory; do not overwrite the archived result.
Reproduce the collector's fake protocol tests separately (not measurements):

```powershell
cl /nologo /std:c++17 /EHsc /MD /O2 /W4 /WX /utf-8 `
  /Fobuild/allocation-capture-agent-tests/mock.obj `
  /Febuild/allocation-capture-agent-tests/mock.exe `
  docs/validation/2026-11-24-public-feed-allocations/collector_fixture.cpp
powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
  docs/validation/2026-11-24-public-feed-allocations/collector_contract_probe.ps1
```

Create build/allocation-capture-agent-tests first if absent. Repository and
mock executable paths can be supplied to the harness. Child-process execution
policy is local to that process, not a machine configuration edit. Mock output
is retained only under ignored build/ and never mixed into real results.

## Identity and remaining gates

source_git_blobs.txt contains123 normalized Git blob IDs: preceding116 paths
recomputed plus4 former parked files, the companion and2 collector fixtures.
It is not raw checkout SHA, an archive of all dependencies or reproducible
toolchain proof. Previous manifests and results remain unchanged.

The new Linux allocation-instrumentation job is configured to build/run only
the two small-load Release targets without sanitizers. Its runner status is
pending at this capture; any later evidence must name the actual source commit
and run. Prior6183893 CI covers the older43-test code, not these new hooks.

Real licensed equity source/reference, recovery, full-layer timing and M5
acceptance remain open. No lock-free queues, parallel matching or risk/account
features were added. New hook calls are not an OOM simulator in the real probe,
nor should observed allocation counts be read as production cost guarantees.
