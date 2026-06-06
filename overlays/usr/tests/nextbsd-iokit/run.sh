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

# Run kextd ourselves (it flushes then re-pushes, so it's idempotent). kextd is
# NOT a boot-time launchd daemon yet — running a CF/OSKext-heavy job before the
# system is up wedges launchd's boot dispatch; the launchd boot-push integration
# lands with K3 (#216) when kextd becomes persistent and its ordering is
# designed. Bound it with a watchdog so a hang surfaces as an empty catalogue
# (-> IOCATALOGUE-FAIL) instead of stalling the whole boot test.
if [ -x /usr/libexec/kextd ]; then
	/usr/libexec/kextd -v &
	kpid=$!
	( sleep 90; kill "$kpid" 2>/dev/null ) &
	wpid=$!
	wait "$kpid" || echo "WARN: kextd exited nonzero or was killed (watchdog)"
	kill "$wpid" 2>/dev/null
else
	echo "WARN: /usr/libexec/kextd missing"
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
else
	echo "IOCATALOGUE-FAIL: IntelWiFi match table not found in the catalogue"
	exit 1
fi

# K3 matcher (#216): ask the kernel which bundle claims the 8260 (0x24f38086),
# via IOCATIOCLOOKUP — the same lookup the in-kernel device_nomatch matcher uses.
# Proves the matcher resolves a real PCI id to its driver bundle without the
# physical NIC. SKIP on a kernel that predates IOCATIOCLOOKUP (K3a).
if [ -x /usr/libexec/kextd ]; then
	lk=$(/usr/libexec/kextd -l 0x24f38086 2>/dev/null || true)
	echo "matcher lookup: ${lk}"
	case "${lk}" in
	*org.nextbsd.kext.intelwifi*)
		echo "IOKIT-LOOKUP-OK: kernel matcher resolves 0x24f38086 -> IntelWiFi"
		;;
	*unsupported*)
		echo "IOKIT-LOOKUP-SKIP: kernel without IOCATIOCLOOKUP (pre-K3a)"
		;;
	*)
		echo "IOKIT-LOOKUP-FAIL: matcher did not resolve the 8260 (catalogue has it)"
		exit 1
		;;
	esac
fi

# K3b round-trip (#216): the kernel->kextd Mach load request. test_kextd_mach
# registers HOST_KEXTD_PORT, drives the matcher's send via IOCATIOCTESTSEND, and
# receives the Mach message — proving the kernel can hand a load request to
# userland over the faithful channel (no devd/devctl). The catalogue was just
# populated by the kextd push above, so the 8260 lookup inside the ioctl hits.
# Emits KEXTD-MACH-OK / -FAIL / -SKIP (SKIP on a kernel predating K3b).
KM=/usr/tests/freebsd-launchd-mach/test_kextd_mach
if [ -x "$KM" ]; then
	"$KM" || true		# the marker (not the exit code) is the signal
else
	echo "KEXTD-MACH-SKIP: test_kextd_mach not present"
fi
exit 0
