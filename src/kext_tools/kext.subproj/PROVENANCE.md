# kext.subproj — vendored Apple OSKext (faithful port, #182)

Apple's `OSKext` engine + helpers, vendored **verbatim** from:

- **`IOKitUser-907.100.13`** (OS X 10.8.5 — deliberately **pre-SIP**, so no
  codesign enforcement, no kernelcache/AuxKC collection machinery to strip)
- Source: `github.com/apple-oss-distributions/IOKitUser` @ tag
  `IOKitUser-907.100.13`, path `kext.subproj/`
- License: **APSL 2.0** (headers intact)

This is the `OSKext` bundle/dependency/load engine that the faithful
`kext_tools` port (#182) is built on — bundle discovery, Info.plist parsing,
`OSBundleLibraries` dependency-graph resolution, version/compat, validation.

## Status: VENDORED, NOT YET BUILT (Phase 0 groundwork)

These files are **not** wired into the build yet — they sit here verbatim as
the starting point. The phased port (see the plan) is:

1. **Phase 0** — vendor (this commit) → a fast standalone `libkext` compile
   (compile-only CI job, sysroot from the `continuous` image) so we can
   iterate the 15.7k-LOC `OSKext.c` against NextBSD's `libCoreFoundation` /
   `libIOKit` in minutes, not full ISO builds.
2. **Re-back XNU → kld** — `OSKextLoad`/unload/query swap from the
   `kext_request` Mach trap to `kldload`/`kldstat`/`kldunload`; personalities
   from `IOCatalogueSendData` to the libIOKit/hwregd matcher; KXLD/mkext/
   prelink deleted (the kernel's `kld` links). Codesign stubbed (no SIP).
3. **Phase 1** — dependency-graph resolution (first user-visible win) feeding
   the bulk conversion (#179) and kextd (#177).

Plan: https://pkgdemon.github.io/nextbsd-oskext-port-plan.html

The minimal kld-backed `kextload`/`kextstat`/`kextunload` trio in the parent
directory remains the shipping tooling until this port graduates.
