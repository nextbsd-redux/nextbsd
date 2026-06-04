#!/bin/sh
# compile-check.sh — syntax-check the vendored Apple OSKext (kext.subproj)
# against a NextBSD sysroot, for the faithful port (#182, Phase 0).
#
# Runs inside a FreeBSD VM with the `continuous` image mounted at $SYSROOT
# (default /mnt). -fsyntax-only means no libs are needed — just headers — so
# we surface the header/type/symbol gaps fast. -ferror-limit=0 collects ALL
# errors per file so we fix in batches, not one-error-per-CI-run.
#
# This is an ITERATION tool, not a gate: it's expected to fail loudly while we
# add compat headers (compat/) and stub out the XNU-only code paths
# (kext_request / kxld / mkext / prelink). Green here == OSKext compiles
# against NextBSD; then we re-back load/unload/query onto kld (Phase 1).
set -u

SYSROOT="${SYSROOT:-/mnt}"
HERE=$(cd "$(dirname "$0")" && pwd)         # .../src/kext_tools/kext.subproj
KT=$(cd "$HERE/.." && pwd)                   # .../src/kext_tools
ROOT=$(cd "$KT/../.." && pwd)                # repo root

# Include path, lowest-priority last:
#   compat/         — our stubs for XNU-only headers (IOKitServer, kxld, ...)
#   kext.subproj/   — OSKext's own headers
#   libIOKit source — IOKit/* headers (in case the image doesn't install them)
#   image /usr/include — NextBSD's real CoreFoundation/, mach/, uuid/, base
INCS="-I$KT/compat -I$HERE -I$ROOT/src/libIOKit -isystem $SYSROOT/usr/include"

# Apple-ish dialect the CF/OSKext sources expect (mirrors launchctl's flags).
CFLAGS="-fsyntax-only -ferror-limit=0 -fblocks -fconstant-cfstrings \
  -include $KT/compat/apple-availability-shim.h \
  -Wno-error -Wno-everything \
  -D__FreeBSD__=15 -DPRIVATE=1 -D__APPLE_API_PRIVATE \
  -DTARGET_OS_OSX=1 -DTARGET_OS_MAC=1"

echo "==> SYSROOT=$SYSROOT"
echo "==> clang: $(cc --version | head -1)"
echo "==> include path: $INCS"
echo

rc=0
# OSKext + the helpers it actually links against. macho_util.c / fat_util.c are
# the Mach-O executable parsers — bypassed on NextBSD (kext executables are ELF
# .ko, not Mach-O), so they're NOT built; OSKext.c only needs their *headers'*
# declarations (macho_util.h/fat_util.h, which pull mach-o/{loader,nlist}.h +
# uuid — all provided). KextManager.c (MIG client to kextd) comes later.
for f in OSKext.c OSKextVersion.c misc_util.c printPList_new.c; do
  src="$HERE/$f"
  [ -f "$src" ] || continue
  echo "================= $f ================="
  if cc $CFLAGS $INCS -c "$src"; then
    echo "  OK: $f syntax-clean"
  else
    echo "  FAIL: $f has errors (see above)"
    rc=1
  fi
  echo
done

echo "==> compile-check rc=$rc (nonzero == still has gaps; expected during Phase 0)"
exit $rc
