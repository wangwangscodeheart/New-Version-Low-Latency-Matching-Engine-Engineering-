# M3 first-batch validation evidence

Date: 2026-11-21 (Asia/Tokyo). Baseline HEAD:
`b889136644264e61ff9b8106330283330d7cb7a4` plus the current uncommitted worktree.
The worktree includes the previous synthetic public-feed benchmark additions.
Nothing was committed, staged or pushed during this batch.

## Scope

New header-only modules:

- `src/itch_binary_reader.hpp`: bounded BinaryFILE framing, completion policy,
  error positions, borrowed views, terminal failure and public-operation reentry guards.
- `src/itch50_decoder.hpp`: 13 typed wire kinds; nine historical kinds are
  length-recognized only and explicitly unsupported.

New tests: `tests/itch_binary_reader_tests.cpp`, `tests/itch50_decoder_tests.cpp`.
CMake adds two independent targets/CTest cases. Existing matching, simulated
publication, Journal, risk and monitor sources were not changed.
All framing/ITCH bytes in the tests are invented; no market records were downloaded.

## Actual executions

MSVC 19.44.35228.0, C++17, x64, Ninja. Initialize the VS x64 Developer cmd and
run `chcp 65001` before CMake. Commands are in [the module document](../../M3_NASDAQ_ITCH.md).

| Output file | Result |
|---|---|
| release-build-and-ctest.txt | Fresh Release configure/build 66/66; 30/30 CTest, 10.33s |
| asan-build-and-ctest.txt | Fresh RelWithDebInfo ASan configure/build 66/66; 30/30 CTest, 12.90s |
| release-final-retest.txt | Rebuilt both new tests after review; 30/30, 4.71s |
| asan-final-retest.txt | Final source already up to date; 30/30, 7.63s |
| strict-contract-tests.txt | Both final test translation units built separately with /W4 /WX and executed successfully |

The final review adds noncopyable/nonmovable reader identity, whole-read reentry
guards (12 streambuf scenarios), physically short allocations for truncation tests,
and an S/C payload followed by another payload before the BinaryFILE end marker.
The decoder and reader are exercised together at every split, bytewise, and via istream.
Reader tests also generate 100,000 records without materializing a whole file and
observe the fixed 4096-byte maximum stream read request; this is not a performance result.

Strict compiler commands, run from the repository root after VS initialization:

```bat
cl /nologo /std:c++17 /EHsc /MD /O2 /W4 /WX /utf-8 /I src /Fobuild\itch-core-release\itch-reader-strict.obj /Febuild\itch-core-release\itch-reader-strict.exe tests\itch_binary_reader_tests.cpp
build\itch-core-release\itch-reader-strict.exe
cl /nologo /std:c++17 /EHsc /MD /O2 /W4 /WX /utf-8 /I src /Fobuild\itch-core-release\itch-decoder-strict.obj /Febuild\itch-core-release\itch-decoder-strict.exe tests\itch50_decoder_tests.cpp
build\itch-core-release\itch-decoder-strict.exe
```

## Source identity and limits

`source_git_blobs.txt` contains 94 normalized Git blob IDs: the prior 89-path
build/source/script inventory, recomputed for this worktree, plus .gitignore and
the four new header/test files. IDs use `git hash-object --path=<path> -- <path>`
and respect Git attributes. They are not raw-checkout SHA-256 hashes, do not
overwrite the dated previous inventory, and are not proof of bit-reproducible
compiler/toolchain dependencies. The actual source files remain in the worktree;
this directory does not contain a self-contained source archive.

Final read-only checks matched all 94 source IDs, verified the four 30/30 log
summaries, confirmed no existing tracked src file changed and no index changes,
and checked 103 local Markdown links (excluding fenced code). Four private/raw
path patterns matched git check-ignore; the source record still reports zero
downloaded market records and null source SHA-256.

Parked allocation-meter/probe files are not linked by CMake and are not counted
as completed tooling or allocation measurements. ASan does not remove the existing
limitations of the separate allocation_failure executable's new/delete overrides;
the two new test executables do not replace allocators.

No real-session validation, licensed data manifest, stock router, native book
application, gzip checksum verification, native-path benchmark, MoldUDP64, packet
loss, resynchronization, Linux, UBSan or TSan evidence is claimed here. The older
GitHub runner at b889136 does not validate these new files. Existing D9025 and
ASan fault-sink C4702 build warnings were retained.
