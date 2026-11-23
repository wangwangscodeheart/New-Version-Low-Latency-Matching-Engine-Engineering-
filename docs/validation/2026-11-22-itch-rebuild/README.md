# M3 second-batch validation evidence

Date: 2026-11-22, Asia/Tokyo. Baseline HEAD:
`b889136644264e61ff9b8106330283330d7cb7a4` plus uncommitted worktree additions
from the synthetic public-feed benchmark and first ITCH batch.

## Scope

Production changes: new `src/itch_book_rebuilder.hpp` and additive native-lifecycle
APIs in `src/market_data_book.hpp`. No V1 matching, TradingEngine, journal, risk,
monitor or M2 publisher changes. All wire bytes are invented in tests. No raw
market file was downloaded, inspected, hashed or uploaded.

Three test targets were added: market_data_book_native, itch_book_rebuilder,
native_allocation. CTest increased from 30 to 33. Existing tests were retained.
Module design, exact profile and reproduction commands are in
[M3 second-batch documentation](../../M3_NATIVE_REBUILD.md).

## Actual executions

Toolchain: MSVC 19.44.35228.0, x64, C++17, Ninja. VS developer environment and
code page 65001 initialized before CMake dependency detection.

| Captured output | Actual result |
|---|---|
| release-build-and-ctest.txt | Fresh Release build 72/72, 33/33 CTest, 10.17s |
| asan-build-and-ctest.txt | Fresh RelWithDebInfo ASan build 72/72, 33/33 CTest, 16.44s |
| release-final-retest.txt | Two targets rebuilt after final header comment, 33/33, 3.87s |
| asan-final-retest.txt | Final source up to date, 33/33, 10.18s |
| strict-contract-and-fault-tests.txt | Three final translation units independently compiled with /O2 /W4 /WX and executed successfully |

Strict compilation used `/std:c++17 /EHsc /MD /O2 /W4 /WX /utf-8 /I src` with
one `.obj` and `.exe` output per file under build/itch-rebuild-release. Sources:
`tests/market_data_book_native_tests.cpp`, `tests/itch_book_rebuilder_tests.cpp`,
`tests/market_data_book_native_allocation_tests.cpp`.

The native book has seven groups; the rebuilder has nine. The separate fault
program reports 20 book scans / 38 failures and three session R/A/U scans /
15 failures. Allocation boundaries are found at runtime: the observed MSVC
growth boundary was eight existing orders, not a portable constant or benchmark.
Each failure checks prior book state/depth/BBO/invariants and global index
coherence, and the session failures remain STALE with errors=1 and book=null.
Retries use a fresh session, never reset a failed instance.

The finalization reentry case was explicitly reviewed: a custom streambuf may
catch a native-API reentry rejection and return EOF/only a marker. Finalization
must still reject STALE, not resurrect COMPLETE. Tests include this and an IO
error after the framing marker. Q zero-share notifications remain valid.

## Source identity and limitations

`source_git_blobs.txt` contains 98 recomputed normalized Git blob IDs: the prior
94-path inventory plus the new rebuilder header and three test files. Hashing
uses `git hash-object --path=<path> -- <path>` and respects Git attributes, not
raw checkout bytes. The old dated 89/94-path inventories remain unchanged.
This is not a full source archive or bit-reproducible toolchain proof; actual
sources are still present in the worktree. The index was not staged or committed.

Final read-only checks matched 98/98 source IDs, the four 33/33 log summaries,
and the 23 scans / 53 injected failures. Among existing tracked src files only
MarketDataBook has additive changes. All 121 local Markdown links in the selected
entry/module/validation documents resolved (fenced code excluded); whitespace
checks passed and the index remained unchanged. The source review still reports
zero imported records, null source SHA-256 and no real-session validation.

The two allocation-failure executables replace their own new/delete operators;
ASan allocation-family coverage is limited there. The other 31 CTest executables
do not contain these replacements. Existing D9025/C4702 warnings were retained;
only the three separate strict builds have a warning-as-error success claim.

There is no native full-ITCH/real-session, licensed data, independent external
reference, gzip verification, throughput/latency, network/recovery, Linux,
UBSan or TSan result for this worktree. Nine known historical metadata kinds
remain rejected, P/Q/B are counted rather than a complete trade ledger, and
business-hours scheduling is not comprehensively validated. COMPLETE refers
only to the declared profile and supplied input. The older b889136 Linux CI
does not validate these changes. Parked allocation-probe files remain unlinked.
