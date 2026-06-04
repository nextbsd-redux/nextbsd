#!/bin/sh
# smoke.sh — runtime smoke test for kextdeps (#182 Phase 1).
#
# Proves the OSKext engine doesn't just link but *works*: bundle discovery,
# Info.plist parsing, and the OSBundleLibraries dependency-graph topological
# sort. Runs the built kextdeps against two fixture kexts (Leaf -> Base) and
# asserts the emitted load order lists Base BEFORE Leaf.
#
# Usage:  smoke.sh <path-to-kextdeps> <smoke-repo-dir>
#   The caller must make the NextBSD runtime libs reachable, e.g.
#   LD_LIBRARY_PATH=<sysroot>/usr/lib/system smoke.sh ...
set -u

KEXTDEPS="${1:?usage: smoke.sh <kextdeps> <repo>}"
REPO="${2:?usage: smoke.sh <kextdeps> <repo>}"
LEAF="$REPO/Leaf.kext"

echo "==> kextdeps smoke: $KEXTDEPS -r $REPO $LEAF"
out=$("$KEXTDEPS" -r "$REPO" "$LEAF" 2>&1)
rc=$?
echo "----- kextdeps output -----"
echo "$out"
echo "---------------------------"

if [ "$rc" -ne 0 ]; then
	echo "FAIL: kextdeps exited $rc"
	exit 1
fi

echo "$out" | grep -q "org.nextbsd.test.base" || {
	echo "FAIL: base identifier missing from load order"; exit 1; }
echo "$out" | grep -q "org.nextbsd.test.leaf" || {
	echo "FAIL: leaf identifier missing from load order"; exit 1; }

base_line=$(echo "$out" | grep -n "org.nextbsd.test.base" | head -1 | cut -d: -f1)
leaf_line=$(echo "$out" | grep -n "org.nextbsd.test.leaf" | head -1 | cut -d: -f1)
if [ "$base_line" -ge "$leaf_line" ]; then
	echo "FAIL: load order is wrong — base ($base_line) must precede leaf ($leaf_line)"
	exit 1
fi

echo "PASS: dependency resolution emits base before leaf"
exit 0
