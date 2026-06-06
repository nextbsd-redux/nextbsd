#!/bin/sh
# nextbsd-iokit/run.sh — verify kextd (#217) populates the in-kernel IOCatalogue
# (#215) with the shipped kexts' IOKitPersonalities. Emits a single marker the
# boot test greps:
#
#   IOCATALOGUE-OK    — catalogue populated; IntelWiFi's match table is present
#   IOCATALOGUE-FAIL  — kextd ran but the catalogue is empty / missing IntelWiFi
#   IOCATALOGUE-SKIP  — no /dev/iocatalogue (pre-K2 kernel) — nothing to test
#
# SKIP keeps this safe on older images (e.g. when nextbsd-kernel's smoke test
# boots a continuous image built before the K2 kernel landed).

set -u

if [ ! -c /dev/iocatalogue ]; then
	echo "IOCATALOGUE-SKIP: no /dev/iocatalogue (kernel without the K2 IOCatalogue)"
	exit 0
fi

# Deterministic + idempotent: run kextd ourselves (it flushes then re-pushes),
# rather than rely on the boot-time launchd one-shot having finished.
if [ -x /usr/libexec/kextd ]; then
	/usr/libexec/kextd -v || echo "WARN: kextd exited nonzero"
else
	echo "WARN: /usr/libexec/kextd missing; relying on boot-time push"
fi

count=$(sysctl -n hw.iokit.catalogue_count 2>/dev/null || echo 0)
dump=$(sysctl -n hw.iokit.catalogue 2>/dev/null || echo "")
echo "hw.iokit.catalogue_count = ${count}"
echo "hw.iokit.catalogue:"
echo "${dump}"

if [ "${count}" -lt 1 ] 2>/dev/null; then
	echo "IOCATALOGUE-FAIL: catalogue is empty after kextd push"
	exit 1
fi

# The shipped IntelWiFi.kext (P1, #213) carries the Intel match table; its
# 8260 id (0x24f38086) flowing through proves kext plist -> kextd -> kernel.
if echo "${dump}" | grep -q "org.nextbsd.kext.intelwifi" &&
   echo "${dump}" | grep -qi "24f38086"; then
	echo "IOCATALOGUE-OK: IntelWiFi personalities (incl. 8260) in the catalogue"
	exit 0
fi

echo "IOCATALOGUE-FAIL: IntelWiFi match table not found in the catalogue"
exit 1
