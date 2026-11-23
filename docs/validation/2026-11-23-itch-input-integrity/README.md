# M3 fifth-batch file-integrity evidence

Date: 2026-11-23, Asia/Tokyo. Captured from baseline
`b889136644264e61ff9b8106330283330d7cb7a4` plus the pending benchmark/M3
increments. Captures precede publication; subsequent commit/push status is in
Git history. No genuine market records were downloaded, read or uploaded.

[Scope and commands](../../M3_ITCH_INPUT_INTEGRITY.md): single-pass raw-source
SHA-256, optional expected hash, optional explicitly enabled gzip, all-member
CRC/ISIZE/truncation/trailing validation, existing single-owner reconstruction
guard through final EOF and summary. No V1 matching/M2 publisher changes here.

## Actual executions

MSVC19.44.35228.0 x64, C++17, CMake4.4.2, Ninja, code page65001 before configure.
Fresh configurations default ENABLE_ZLIB_INPUT=OFF Release, ON Release and ON
RelWithDebInfo/ENABLE_ASAN=ON (UBSan OFF). ON builds use installed local zlib1.4,
ZLIB_ROOT=build/zlib-1.4-local/release-install and ZLIB_USE_STATIC_LIBS=ON.
No auto dependency download or machine-wide installation.

| Raw output | Actual result |
|---|---|
| release-no-zlib-build-and-ctest.txt | Fresh92/92,43/43,15.70s; gzip disabled/rejection only |
| release-zlib-build-and-ctest.txt | Fresh92/92,43/43,14.95s; enabled gzip |
| asan-zlib-build-and-ctest.txt | Fresh92/92,43/43,19.65s; enabled C++ ASan |
| release-no-zlib-final-retest.txt | Rebuilt final CLI/gzip tests;41/43; shared temporary-file interference |
| release-zlib-final-retest.txt | Same final tests;42/43; shared temporary-file interference |
| asan-zlib-final-retest.txt | Same final tests;43/43,15.15s |
| release-no-zlib-isolated-retest.txt | Final source up to date;43/43,2.34s |
| release-zlib-isolated-retest.txt | Final source up to date;43/43,2.43s |
| asan-zlib-isolated-retest.txt | Final source up to date;43/43,9.98s |
| strict-integrity-tests.txt | Seven independent /O2 /W4 /WX compiles/executions passed |
| cli-synthetic-smoke.txt | Direct .ps1 invocation denied by existing machine execution policy; not a test pass |
| cli-synthetic-final-smoke.txt | Reviewed script block run directly, seven actual executable cases passed; no machine policy changed |

Old tests use fixed filenames under temp_directory_path(). Running configurations
concurrently exposed Journal file-lock/count and generator-fixture interference,
not a CRC/SHA failure. Both failed logs are preserved. Final runs set TEMP and
TMP to the corresponding build/<configuration>/isolated-temp before launching
CTest; no tested source was changed to hide the failures. Tests within each
CTest invocation remain serial. These runs do not certify old tests safe for
concurrent invocations with a shared TEMP, or measure application performance.

Commands: after configuring the three build directories, use the MSVC developer
environment, chcp65001, set distinct absolute TEMP and TMP directories, then:

```text
cmake --build build/<configuration> --parallel 3
ctest --test-dir build/<configuration> --output-on-failure
```

Strict flags: /std:c++17 /EHsc /MD /O2 /W4 /WX /utf-8 /I src, adding
/DMATCHING_ENGINE_HAS_ZLIB=0 or1 to gzip, verified-input and inspector tests;
ON additionally has the local include directory and libzs.lib. SHA compiled
once without zlib. Normal complete builds retain existing D9025/C4702 warnings;
strict success is only claimed for these explicitly /WX translation units.

Actual CLI smoke uses six invented S records OSQMEC + the zero BinaryFILE
marker, under a Chinese path. Independent .NET Get-FileHash raw SHA:
`3afc0c0eb85db019a30ff6231ea2442471c9d48ee0a518230cb207e504961787`.
.NET GZipStream produced49 bytes decoding to86. Both raw builds and enabled
gzip accepted the independent expected digest; disabled gzip, corrupt CRC,
wrong hash and missing attestation returned1/1/1/2. All three success summaries
explicitly show source/date unauthenticated, zero stocks/orders/reports and
PROFILE_COMPLETE_NOT_REAL_DATA_ACCEPTANCE. No real-session conclusion follows.

## Identity, dependency and limits

source_git_blobs.txt contains114 normalized Git blob IDs: the previous108
paths recomputed plus3 headers/3 tests. `git hash-object --path=<path> -- <path>`
respects attributes; this is not raw-checkout SHA-256, a source archive or a
bit-reproducible toolchain claim. Old manifests/logs remain unchanged. The four
parked allocation-meter/probe files are not CMake wired, tested here or included
in the publication. They remain locally preserved.

[Dependency provenance](dependency-provenance.md) records verified source asset
size/hash, rejected HTML response, isolated local build and library identity.
The linked zlib C Release static library is NOT ASan instrumented. C++ ASan
success does not validate its internal memory accesses. Two existing fault
programs replace new/delete and retain their allocation-family diagnostic
limits; the other41 tests do not contain those replacements.

Checksums/CRC are not source authentication, signatures, permissions or market
completeness. Hash covers the actual single raw byte stream, not an atomic
snapshot of a concurrently modified file. Late error makes the session STALE,
not rolled back or recovered. Byte/member limits are not total heap/time
budgets. No Linux/UBSan/TSan run, full business/official tape semantics, licensed
dataset/reference, network recovery or controlled native performance evidence
was added; M3 overall acceptance remains pending.
