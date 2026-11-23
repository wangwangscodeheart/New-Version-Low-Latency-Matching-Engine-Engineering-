# M3 fourth-batch validation evidence

Date: 2026-11-22, Asia/Tokyo. Baseline remains
`b889136644264e61ff9b8106330283330d7cb7a4` plus the uncommitted benchmark and
three ITCH batches. No stage/commit/push or market-record download occurred.

## Scope and profile

New temporal/date helper, bounded trade-correlation ledger, explicit rebuilder
validation options, and an uncompressed offline inspector. Legacy library
defaults remain book/metadata only; the CLI enables range, E/C/P-break correlation
and six-system-event checks after a caller local-use attestation. It does not
verify that attestation, authenticate dates, compute hashes or accept gzip.

[Design and commands](../../M3_ITCH_REPORT_VALIDATION.md) specify limits and
non-goals. No V1 matching, MarketDataBook or M2 publisher changes in this batch.
All reports/bytes/files are self-generated; date1900..9999 checks are calendar
syntax/range checks, not a market calendar or source authentication.

## Actual executions

MSVC 19.44.35228.0 x64, C++17, Ninja, code page65001 before configuration.
CTest increased35 to40: ledger, temporal, report-session, inspector tests and
inspector executable --help. Existing35 cases remain unchanged in purpose.

| Captured output | Result |
|---|---|
| release-build-and-ctest.txt | Fresh86/86 Release build,40/40,13.39s |
| asan-build-and-ctest.txt | Fresh86/86 RelWithDebInfo ASan build,40/40,20.00s |
| release-final-retest.txt | Report fault/output/Unicode changes rebuilt,40/40,3.69s |
| asan-final-retest.txt | Affected final target rebuilt/up to date,40/40,9.26s |
| release-io-retest.txt | Help exception/error/warning refinements rebuilt,40/40,3.14s |
| asan-io-retest.txt | Same final source rebuilt,40/40,9.79s |
| strict-contract-and-fault-tests.txt | Four new test translation units and native fault test separately /O2 /W4 /WX compiled/executed |
| strict-final-io-tests.txt | Final inspector tests and MSVC Unicode entry separately /O2 /W4 /WX compiled/executed |
| cli-final-synthetic-smoke.txt | Final actual exe, Chinese path, six self-generated S records;exit0 with declaration,exit2 without |

Strict flags were `/std:c++17 /EHsc /MD /O2 /W4 /WX /utf-8 /I src`; .obj/.exe
artifacts stay under build/itch-report-release. MSVC uses wmain and UTF16->UTF8
conversion, followed by filesystem paths. The actual CLI smoke contains zero
stocks/orders/trades and explicitly labels its self-generated source; it is
not a market session. PowerShell wraps the expected denial stderr in a
NativeCommandError record; the checked native process exit was2.

The earlier cli-synthetic-smoke.txt is retained but the final smoke refers to
the final wrapper. Final tests cover ostream short writes, flush failures and
enabled exception masks, including help/diagnostic failures, plus count-only
time regression warnings. An error exit invalidates any partial summary text.

Temporal helper has5 groups, ledger6, strict-session7, inspector2 plus help
integration. The native fault program now reports book20/38, R-A-U3/16, L1/1,
correlated execution1/1:25 scans,56 failures. Allocation failure in the new report
node does not deplete displayed quantity or advance ledger/time/counters.
Valid B runs with zero allocation attempts and never restores executed shares.
These are scoped tests, not whole-path allocation/performance measurements.

## Identity and remaining limits

source_git_blobs.txt contains108 normalized Git blob IDs: previous100 inventory
recomputed plus four new source/header files and four new test files. Hashing
with `git hash-object --path=<path> -- <path>` respects attributes. It is not raw
checkout SHA-256, a full source archive or bit-reproducible toolchain proof.
Old89/94/98/100 manifests and raw outputs are not overwritten. Source files are
still present in the worktree, with the index unchanged.

Final read-only checks matched108/108 identities and six40/40 test summaries,
verified25/56 fault totals and both zero-attempt paths, checked the final Unicode
synthetic CLI success/denial exits, and resolved163 selected local Markdown
links (fenced code excluded). Whitespace checks passed; data status still shows
zero imported market records, null source SHA and no real-session validation.

Two fault executables replace their own new/delete and retain ASan allocation
family limitations; the other38 CTest executables do not. The complete build
retains existing D9025/C4702 warnings; only explicit /WX targets claim strict
warning success. Older b889136 Linux CI does not cover this code.

Q-break correlation, full trading-hours business rules, official tape/account
semantics, timezone/DST/leap seconds, genuine source/date/hash/gzip integrity,
real licensed dataset/reference, network/recovery and native-path performance
are not validated. New rules are explicit opt-ins in the library, not universal
entry guarantees. No new Linux/UBSan/TSan evidence or M3 overall acceptance.
