# M3 third-batch validation evidence

Date: 2026-11-22, Asia/Tokyo. Baseline HEAD remains
`b889136644264e61ff9b8106330283330d7cb7a4` plus the uncommitted benchmark,
reader/decoder and native-rebuild worktree. No staging, commit or push occurred.

## Scope

Extend src/itch50_decoder.hpp with nine historical metadata payloads; extend
src/itch_book_rebuilder.hpp with latest per-stock/per-MPID/per-venue and global
metadata. The prior core decoder/rebuild tests are adjusted for known versus
unknown kinds. Add two test files and CTest targets; extend the existing isolated
native allocation test with L insertion failure/update checks.

No V1 matching, MarketDataBook or M2 publisher modifications in this batch.
No raw market records were fetched, read, hashed, uploaded or published.
[Design and reproduction commands](../../M3_ITCH_METADATA.md) define the exact
historical profile and what COMPLETE does not prove.

## Actual executions

MSVC 19.44.35228.0, x64, C++17, Ninja, code page 65001 initialized before CMake.

| Output | Result |
|---|---|
| release-build-and-ctest.txt | Fresh Release build 76/76, all35 passed, 12.50s |
| asan-build-and-ctest.txt | Fresh RelWithDebInfo ASan build 76/76, all35 passed, 14.35s |
| release-final-retest.txt | New L fault test rebuilt, all35 passed, 3.09s |
| asan-final-retest.txt | Same final test rebuilt, all35 passed, 11.27s |
| strict-contract-and-fault-tests.txt | Four test translation units separately compiled with /O2 /W4 /WX and executed successfully |

Strict flags: `/std:c++17 /EHsc /MD /O2 /W4 /WX /utf-8 /I src`. Sources:
tests/itch50_decoder_tests.cpp, tests/itch50_metadata_decoder_tests.cpp,
tests/itch_metadata_session_tests.cpp, tests/market_data_book_native_allocation_tests.cpp.
Separate .obj/.exe artifacts are under build/itch-metadata-release, not source.

New decoder: four groups, literal field goldens plus full-width/zero/raw codes,
owning values, unaligned buffers, exact-size heap truncations, framing integration.
New session: seven groups, all22 wire-kind mixed lifecycle stream with every
split/bytewise/istream, unchanged book/depth/BBO across metadata, global V/W and
body-routed K, venue-isolated h, MPID bounds, historical flags and terminal errors.

The native fault program reports book20/38, R-A-U3/16, L1/1: 24 scans, 55 injected
failures. Existing L update performs zero allocation attempts in the scoped
fault check, not a whole-path allocation or latency claim. R construction has
one more MSVC map allocation than the prior batch because metadata now contains
a per-MPID map. Prior dated outputs (3/15, total53) remain unchanged and valid
for their earlier source; counts are not portable STL constants.

## Identity and limitations

source_git_blobs.txt contains 100 normalized Git blob IDs: the prior98 inventory
recomputed plus the two new test files. `git hash-object --path=<path> -- <path>`
respects Git attributes; these are not raw-checkout SHA-256 hashes, a full source
archive or a bit-reproducible compiler/toolchain proof. Source files remain in
the worktree. The old 89/94/98 dated manifests are not overwritten.

Final read-only checks matched 100/100 source identities and all four35/35 log
summaries, verified the 24/55 fault totals and zero-attempt existing-MPID update,
and resolved 141 selected-document local Markdown links (fenced code excluded).
Whitespace checks passed, the index remained unchanged, and source review still
reports zero imported market records, null SHA-256 and no real-session validation.

Two independent fault executables replace their own new/delete; retain ASan
allocation-family coverage limitations there. Other33 CTest executables do not
contain those replacements. Existing full-build D9025/C4702 warnings were not
suppressed; only the four explicit strict test builds have a /WX success claim.

All22 describes targeted historical layouts, not every ITCH version or complete
business semantics. Metadata stores latest bodies, not a time series, freshness,
trading permission or compliance decision. There is no real-session/licensed
dataset, fixed2018 PDF artifact, external reference, gzip, complete trade ledger,
full schedule, native-path benchmark, network/resync, Linux/UBSan/TSan evidence
for this worktree. The older b889136 Linux CI cannot validate this source.
