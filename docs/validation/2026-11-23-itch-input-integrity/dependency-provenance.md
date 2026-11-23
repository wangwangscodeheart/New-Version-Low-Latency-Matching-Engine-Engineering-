# Local optional zlib dependency provenance

Date: 2026-11-23, Asia/Tokyo. This is a development dependency, not market data.
No provider records were downloaded, read or uploaded. The project itself does
not download dependencies; ENABLE_ZLIB_INPUT defaults OFF.

- Official homepage/version/hash: [zlib.net](https://zlib.net/), zlib1.4.
- Official release: [madler/zlib v1.4](https://github.com/madler/zlib/releases/tag/v1.4).
- Downloaded asset: https://github.com/madler/zlib/releases/download/v1.4/zlib-1.4.tar.gz
- Actual compressed bytes: 1,507,140.
- SHA-256, independently computed by PowerShell Get-FileHash and matched to
  official published value:
  `02dd159783677a749c04adbdd02b55b8a2bcc7e054f51b76b549199a916ad584`.
- No GPG/signature verification claimed. A matching downloaded/published hash
  is not an independent cryptographic source-authentication ceremony.

The earlier zlib.net archive URL returned a 11,966-byte HTML response rather
than this archive. Its digest did not match, so extraction/build was refused.
That response was not executed. The verified GitHub release asset above was
the only source extracted and built. Archive listing had302 entries, all
under zlib-1.4/, no traversal or symbolic/hard links. Source, build and install
are isolated under ignored build/zlib-1.4-local/, not vendored or system-wide.

Actual build: MSVC19.44.35228.0 x64, CMake4.4.2, Ninja, Release, static zlib,
MultiThreadedDLL runtime. Captured build/install output:
[zlib-local-build.txt](zlib-local-build.txt).

```text
cmake -S build/zlib-1.4-local/zlib-1.4
  -B build/zlib-1.4-local/release-build -G Ninja
  -DCMAKE_BUILD_TYPE=Release
  -DCMAKE_INSTALL_PREFIX=<repo>/build/zlib-1.4-local/release-install
  -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL
  -DBUILD_TESTING=OFF -DZLIB_BUILD_TESTING=OFF -DZLIB_BUILD_SHARED=OFF
cmake --build build/zlib-1.4-local/release-build --parallel 6
cmake --install build/zlib-1.4-local/release-build
```

`BUILD_TESTING` was unused by this dependency version; the diagnostic is
retained. Installed lib/libzs.lib SHA-256 at verification:
`c070e67e1f90c28a5069950c895b0b379e44f0953484c7b01ad5a6ac463c7d73`.
This artifact identity is machine-specific, not a reproducible-build claim.

Both enabled Release and C++ ASan configurations link this same uninstrumented
Release C static library. Wrapper/test ASan success does not cover zlib's
internal C memory operations; no separately instrumented zlib or upstream
test-suite execution is claimed. Dependency source/license remain upstream;
no source/binary is redistributed in the repository.
