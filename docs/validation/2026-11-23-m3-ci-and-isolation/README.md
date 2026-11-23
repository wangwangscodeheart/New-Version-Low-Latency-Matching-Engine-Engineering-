# M3 sixth-batch CI and test-fixture evidence

Date: 2026-11-23, Asia/Tokyo. Baseline05f003b. Scope is only workflow and tests;
production sources and CMake are unchanged. No real market data fetched/read.
[Design and limits](../../M3_CI_AND_TEST_ISOLATION.md).

## Preceding commit, not the new matrix

Actual [run45181504978](https://github.com/wangwangscodeheart/New-Version-Low-Latency-Matching-Engine-Engineering-/actions/runs/45181504978),
commit05f003b404b4ccdff8db8b559440842b0492d834, push event, attempt1:

| Job | Actual outcome | Full decoded log |
|---|---|---|
| Release |43/43,0.82s|previous-05f003b-release.txt|
| ASan+UBSan |43/43,28.02s|previous-05f003b-asan-ubsan.txt|
| TSan async subset |4/4,0.79s|previous-05f003b-tsan.txt|
| Benchmark smoke |success, not performance acceptance|previous-05f003b-benchmark-smoke.txt|
| Manual differential-stress |skipped on push, not a pass|job137716176419|

Logs obtained from GitHub's job-log API after completion, not simulated Linux
output. Runner Ubuntu26.04.1, GNU15.2.0. Original timestamped warnings retained.
CI and strict captures are marked -text/-diff in .gitattributes so checkout
does not rewrite evidence bytes or treat original log whitespace as source
formatting. View the raw files; source/docs/scripts still receive whitespace
checks. This does not suppress compiler warnings or change test outcomes.
All these jobs have zlib default OFF. They do NOT validate positive gzip or the
new fixture helper. New workflow includes OFF/ON Release and ASan+UBSan; its
first publication/runner outcome is pending at this capture, and any later
outcome must be identified by its own commit/run, not overwritten above.

## Completed new matrix: code commit6183893

[Run45183239078](https://github.com/wangwangscodeheart/New-Version-Low-Latency-Matching-Engine-Engineering-/actions/runs/45183239078)
completed successfully for618389322b4a38749257a85fd13af98dbbe6039b,
push event, attempt1. [Selected API status fields](ci-6183893-status.json) and
six complete decoded job logs are retained. This is actual runner execution,
not the pending expectation described in the preceding historical capture.
ci-evidence-checksums.sha256 identifies all16 captured text-output files in
this directory, excluding the source identity list. These are repository
capture-byte hashes, not provider ZIP signatures, source authentication or
hashes of Markdown/API JSON. Staged blobs were compared to unfiltered local
file IDs to rule out commit-time byte conversion.

| Mode/job | Full CTest | Additional verbose integrity tests | Log |
|---|---|---|---|
| Release OFF |43/43,0.86s|5/5,0.02s; gzip disabled guards|ci-6183893-release-off.txt|
| Release ON |43/43,0.86s|5/5,0.03s; seven positive gzip groups|ci-6183893-release-on.txt|
| ASan+UBSan OFF |43/43,14.09s|5/5,0.19s; gzip disabled guards|ci-6183893-asan-ubsan-off.txt|
| ASan+UBSan ON |43/43,28.49s|5/5,0.92s; seven positive gzip groups|ci-6183893-asan-ubsan-on.txt|
| TSan async subset |4/4,0.52s|no claim for matching core|ci-6183893-tsan.txt|
| Benchmark smoke |success|not controlled performance evidence|ci-6183893-benchmark-smoke.txt|

Manual10M differential-stress job137722149629 was skipped on push, not passed.
ON logs show system /usr/lib/x86_64-linux-gnu/libz.so version1.3.1,
gzip_support=enabled, inspector/reader positive success, and gzip seven groups;
OFF explicitly says enabled CRC/member tests NOT EXECUTED. Ubuntu26.04.1 and
GNU15.2.0 remain the actual runner/toolchain. System zlib C library is not
ASan/UBSan-instrumented by this project. Test durations are not benchmark data.

The subsequent archival commit changes documentation/logs only and retains all
116 recorded build/verification input identities. Its [skip ci] message avoids
rerunning the identical source solely to archive its own preceding run, per
[GitHub's documented skip behavior](https://docs.github.com/en/actions/how-tos/manage-workflow-runs/skip-workflow-runs).
It is not a new tested code commit and must not be advertised as another CI run.

## Current worktree actual Windows executions

MSVC19.44.35228.0 x64, C++17, CMake4.4.2, Ninja. Reused fifth-batch build dirs,
each rebuilt16 steps for the8 affected translation units; not a fresh92-step
build. No per-configuration TEMP/TMP separation. Three invocations overlapped:

| Log | Result |
|---|---|
|release-no-zlib-build-and-ctest.txt|43/43,9.99s|
|release-zlib-build-and-ctest.txt|43/43,5.59s|
|asan-zlib-build-and-ctest.txt|43/43,14.38s|
|strict-test-isolation.txt|all8 affected programs separately compiled and run with /W4 /WX|
|parallel-shared-temp-probe.txt|32 Release passes;16 ASan DLL-not-found exits before tests; NOT a complete pass|
|parallel-shared-temp-final-probe.txt|48/48 process exits0, zero shared-temp entries|

Strict command flags: /nologo /std:c++17 /EHsc /MD /O2 /W4 /WX /utf-8 /I src,
dedicated /Fo and /Fe under build/test-isolation-strict. Includes fixture-owner
positive/negative assertions in generator test. Existing full-build D9025/C4702
warnings are not hidden by the scoped strict success.

The [probe script](parallel_fixture_probe.ps1) starts eight affected binaries
from each of three configurations, two copies each. Every child shares one
newly created dedicated TEMP/TMP/TMPDIR; working directories and stdout/stderr
captures are private, under ignored build/fixture-isolation-probe/<GUID>/.
All are launched before waiting. It records exit codes and shared root entries,
never deletes existing user files or installs machine-wide dependencies.

ASan first failed to load because the launcher lacked the MSVC developer
runtime PATH (`0xC0000135`, -1073741515). That log is preserved. Exactly the
same script/source was rerun after VsDevCmd x64/chcp65001, and48/48 passed.
The executable change is not a test workaround; no machine policy/PATH edit.
Use an MSVC developer environment, then:

```powershell
$probe = Get-Content docs/validation/2026-11-23-m3-ci-and-isolation/parallel_fixture_probe.ps1 -Raw -Encoding utf8
& ([ScriptBlock]::Create($probe)) -Repository (Get-Location).Path
```

This direct script-block invocation matches the environment's existing script
execution-policy restriction without changing that machine policy. Custom
BuildDirectories must be direct build/ children; all eight binaries must exist.
Probe runs are test isolation evidence, NOT parallel matching, throughput or
latency measurements. Per-process data remains ignored locally for inspection.

## Identity and caveats

source_git_blobs.txt has116 normalized Git blob IDs: preceding114 recomputed
plus temporary_test_directory.hpp and the probe script. Hash-object uses --path
attributes; not raw-checkout SHA-256, full source archive or reproducible-build
proof. Previous manifests/logs remain unchanged. The four parked allocation
tools are neither built nor included in this publication.

New helper removes only registered direct files and its empty owned directory.
Unregistered/replaced/busy paths may remain. Other established test fixtures
keep their earlier cleanup implementations; not all tests use nonrecursive
cleanup. No hostile filesystem-race security claim. Production matching stays
single-writer. Linked zlib C libraries remain uninstrumented; existing two
replacement-new/delete fault executables retain their diagnostic limits.
No licensed real dataset/reference, network recovery or native performance
acceptance added; M3 is not complete.
