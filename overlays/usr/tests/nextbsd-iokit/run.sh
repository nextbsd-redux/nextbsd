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

# K3b step 3 (#217): kextd AUTO-STARTS at boot (com.apple.kextd.plist,
# RunAtLoad System) — it registers HOST_KEXTD_PORT, pushes personalities, and
# serves load requests. Its startup markers (kextd: listening / opening repo /
# repo opened / pushed / ready) already appear in this serial log above, since
# the plist routes kextd to /dev/console. Here we verify the AUTO-STARTED daemon
# actually loads a kext on a kernel request: inject one with `kextd -t`
# (IOCATIOCTESTSEND -> the kernel sends to the running daemon over HOST_KEXTD_PORT)
# and confirm IntelWiFi kldloaded. This is the full auto-load path minus the
# physical device (the 8260 bind is the t420 test).
if [ -x /usr/libexec/kextd ]; then
	pgrep -f "kextd -w" >/dev/null 2>&1 && echo "kextd -w daemon is running" \
	    || echo "WARN: kextd -w daemon not found running"
	/usr/libexec/kextd -t 0x24f38086 || true	# inject a load request
	loaded=no
	i=0
	while [ "$i" -lt 12 ]; do
		# The loaded kld file is named after the bundle executable
		# (IntelWiFi); `kextstat` lists loaded files, so it shows that.
		# The driver module inside (if_iwlwifi) is found by module name
		# via `kextstat -m` (modfind(2)). The kld* CLIs were retired
		# (#193); kextstat rides the same kld*(2) syscalls. Check both.
		if kextstat 2>/dev/null | grep -qi intelwifi ||
		   kextstat -m if_iwlwifi >/dev/null 2>&1; then loaded=yes; break; fi
		sleep 1; i=$((i + 1))
	done
	echo "=== /var/log/kextd.log (daemon) ==="; cat /var/log/kextd.log 2>/dev/null; echo "==="
	if [ "$loaded" = yes ]; then
		echo "KEXTD-LOAD-OK: auto-started kextd loaded IntelWiFi on a kernel request"
	elif ! pgrep -f "kextd -w" >/dev/null 2>&1; then
		echo "KEXTD-LOAD-SKIP: no kextd -w daemon (image predates the boot launch)"
	else
		echo "KEXTD-LOAD-FAIL: IntelWiFi not loaded after a kernel request"
	fi
else
	echo "KEXTD-LOAD-SKIP: kextd not present"
fi
exit 0
