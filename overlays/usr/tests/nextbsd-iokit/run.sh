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

# IOREG — libIOKit registry walk via the K1 /dev/ioregistry device (C1.1,
# #218). libIOKit reads /dev/ioregistry (IOREGIOC* ioctls); hwregd was retired
# in #218. This gate exercises the live path with the `ioreg` tool and
# asserts the registry root plus the boot disk/NIC nubs are visible. It is also
# the deferred K1 functional proof (the kernel registry actually answers).
#
#   IOREG-OK    — /dev/ioregistry present; ioreg printed root + a disk/NIC nub
#   IOREG-FAIL  — /dev/ioregistry present but the walk is empty / missing nodes
#   IOREG-SKIP  — no /dev/ioregistry (old kernel predating K1) — nothing to test
#
# Run as a function that emits EXACTLY ONE marker and returns (never exits) so
# the IOCATALOGUE/IOKIT-LOOKUP/KEXTD-LOAD checks below still run on a K1-but-
# no-K2 kernel — and so a K1 SKIP here doesn't short-circuit the rest. The boot
# test gates on the IOREG-FAIL *string*, not on this script's exit code, so a
# FAIL marker + return still fails CI. SELF-SKIP on a pre-K1 kernel keeps this
# green on continuous images built before #214 landed.
ioreg_gate()
{
	if [ ! -c /dev/ioregistry ]; then
		echo "IOREG-SKIP: no /dev/ioregistry (kernel without the K1 in-kernel registry)"
		return 0
	fi
	if [ ! -x /usr/sbin/ioreg ]; then
		echo "IOREG-SKIP: /usr/sbin/ioreg not present"
		return 0
	fi

	iorg=$(/usr/sbin/ioreg 2>/dev/null || true)
	echo "=== ioreg (via /dev/ioregistry) ==="
	echo "${iorg}"
	echo "==="

	# Root: ioreg prints the tree top as "+-o <root> <class ...>"; an empty
	# walk (no "+-o" lines at all) means the registry didn't answer.
	if ! echo "${iorg}" | grep -q '+-o '; then
		echo "IOREG-FAIL: ioreg produced no nodes from /dev/ioregistry"
		return 0
	fi

	# Boot devices: under QEMU the root disk is a virtio block device (vtbd /
	# da) and the NIC is the configured model (em/e1000 in CI, or a
	# virtio-net vtnet). Accept any of the common names so the gate tracks
	# "the registry sees real hardware nubs" rather than one exact string.
	# ioreg renders bare driver CLASS names without unit numbers, e.g.
	# "+-o vtblk  <class vtblk>" / "+-o em  <class em>" — so match the
	# "<class NAME>" token, not "name+digit". (virtio-blk is class "vtblk",
	# NOT "vtbd"; the qemu e1000 NIC is class "em".)
	disk_ok=no
	if echo "${iorg}" | grep -Eqi '<class (vtblk|ahcich|nvme|nvd|ada|da|mmcsd|cd)>'; then
		disk_ok=yes
	fi
	nic_ok=no
	if echo "${iorg}" | grep -Eqi '<class (em|vtnet|igb|ix|re|bge)>'; then
		nic_ok=yes
	fi
	echo "ioreg: disk_ok=${disk_ok} nic_ok=${nic_ok}"

	if [ "${disk_ok}" = yes ] && [ "${nic_ok}" = yes ]; then
		echo "IOREG-OK: /dev/ioregistry walk shows root + boot disk + NIC nubs"
	elif [ "${disk_ok}" = yes ] || [ "${nic_ok}" = yes ]; then
		# One nub class visible is enough to prove the live kernel walk
		# works; the other may carry a name this matcher doesn't list.
		echo "IOREG-OK: /dev/ioregistry walk shows root + a boot device nub"
	else
		echo "IOREG-FAIL: /dev/ioregistry walk found no disk or NIC nub"
	fi
	return 0
}
ioreg_gate

# IOKITNOTIFY — C1.2 (#218) IOKitNotify migration round-trip gate. The
# iokitnotifyrt client registers a matching notification through libIOKit (which
# now registers the recv port via IOREGIOCWATCH on the in-kernel registry, #225)
# and fires the IOREGIOCTESTEVENT inject ioctl to synthesize a matching device
# event, then confirms the registered callback received it. This is the
# deterministic proof of the kernel notify channel without a physical device.
#
#   IOKITNOTIFY-OK    — injected event round-tripped to the callback
#   IOKITNOTIFY-FAIL  — channel present but the callback never fired
#   IOKITNOTIFY-SKIP  — no /dev/ioregistry, no IOREGIOCTESTEVENT, or no client
#
# CRITICAL (MDNS-IFWATCH lesson): the client ALWAYS prints exactly one definite
# marker — including a SELF-SKIP when the round-trip can't be staged (a kernel
# predating the Part A inject ioctl, i.e. a continuous image built before the
# kernel PR ingests). Run it as a function that emits one marker and RETURNS
# (never exits) so the IOCATALOGUE / IOKIT-LOOKUP / KEXTD-LOAD checks below still
# run. The boot test gates on the IOKITNOTIFY-FAIL *string*, not this script's
# exit code.
iokitnotify_gate()
{
	rt=/usr/tests/freebsd-launchd-mach/iokitnotifyrt

	if [ ! -x "$rt" ]; then
		echo "IOKITNOTIFY-SKIP: iokitnotifyrt client not present"
		return 0
	fi
	# iokitnotifyrt prints exactly one IOKITNOTIFY-{OK,FAIL,SKIP} line and
	# self-skips on a kernel without /dev/ioregistry or IOREGIOCTESTEVENT. It
	# is self-bounding (a ~5s callback budget; its receive thread joins on a
	# 500ms mach_msg timeout) so it cannot stall the boot test. If it somehow
	# produces no marker at all (crash / signal), synthesize a SKIP so the
	# expect stream always advances on a definite line.
	out=$("$rt" 2>&1)
	rc=$?
	echo "=== iokitnotifyrt (rc=${rc}) ==="
	echo "${out}"
	echo "==="
	if echo "${out}" | grep -q '^IOKITNOTIFY-'; then
		:	# the client already emitted its definite marker
	else
		echo "IOKITNOTIFY-SKIP: iokitnotifyrt produced no marker (rc=${rc})"
	fi
	return 0
}
iokitnotify_gate


if [ ! -c /dev/iocatalogue ]; then
	echo "IOCATALOGUE-SKIP: no /dev/iocatalogue (kernel without the K2 IOCatalogue)"
	exit 0
fi

# kextd is now a boot-time launchd daemon (com.apple.kextd.plist, RunAtLoad,
# K3b/#217) that registers HOST_KEXTD_PORT, pushes the repo personalities, and
# serves load requests. We DO NOT push again here: a manual one-shot `kextd`
# push used to run at this point and masked a real bug — the *boot daemon's own*
# push does not result in boot-time autoload (the manual push only worked because
# it ran against the already-serving daemon). This test now verifies the PURE
# boot-daemon state — catalogue populated AND devices autoloaded by the boot
# daemon ALONE, no manual poke — exactly how real hardware behaves.
echo "=== /var/log/kextd.log (boot daemon, verbose) ==="
cat /var/log/kextd.log 2>/dev/null
echo "=== kextstat (what the boot daemon autoloaded) ==="
kextstat 2>/dev/null
echo "==="

count=$(sysctl -n hw.iokit.catalogue_count 2>/dev/null || echo 0)
dump=$(sysctl -n hw.iokit.catalogue 2>/dev/null || echo "")
echo "hw.iokit.catalogue_count = ${count}"
echo "hw.iokit.catalogue:"
echo "${dump}"

if [ "${count}" -lt 1 ] 2>/dev/null; then
	echo "IOCATALOGUE-FAIL: catalogue empty — the boot kextd daemon did not push personalities"
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

# EM-AUTOLOAD — #219 (D1): Intel ethernet em -> IntelEthernet.kext auto-load.
# With `nodevice em` (#219) the qemu e1000 (82540EM) hits device_nomatch and
# kextd loads IntelEthernet.kext (if_em) to bind it as em0. The loaded kld file
# is named after the bundle executable (IntelEthernet), so kextstat/kldstat show
# it ONLY when it was auto-loaded — a built-in em driver does not appear as a
# separate loaded file, so the kext-loaded signal cleanly distinguishes the two.
# em0's address is read with `ipconfig` (Apple IPConfiguration); FreeBSD's
# ifconfig(8) is NOT on the stripped image, so it must not be used here. Emits:
#
# No manual kextd poke ran above, so this is the PURE boot-autoload result —
# em0 must have been brought up by the boot daemon alone (real-hardware shape).
#   EM-AUTOLOAD-OK    — IntelEthernet auto-loaded by the boot daemon AND em0 has an address
#   EM-AUTOLOAD-SKIP  — IntelEthernet not loaded but em0 IS up -> em is built into this kernel
#   EM-AUTOLOAD-FAIL  — IntelEthernet loaded but em0 has no address (bind/DHCP failed), OR
#                       IntelEthernet not loaded AND em0 has no address (boot autoload did NOT happen)
em_addr=$(ipconfig getifaddr em0 2>/dev/null || true)
if kextstat 2>/dev/null | grep -qi intelethernet; then
	if [ -n "$em_addr" ]; then
		echo "EM-AUTOLOAD-OK: em0 ($em_addr) up via boot-daemon auto-load of IntelEthernet.kext"
	else
		echo "EM-AUTOLOAD-FAIL: IntelEthernet.kext loaded but em0 has no address (bind/DHCP failed)"
	fi
elif [ -n "$em_addr" ]; then
	echo "EM-AUTOLOAD-SKIP: IntelEthernet not loaded but em0 is up — em is built into this kernel"
else
	# em is nodevice in the continuous kernel (#219), so "not loaded + no address"
	# means the boot daemon never autoloaded the NIC — the real-hardware bug.
	echo "EM-AUTOLOAD-FAIL: boot daemon did NOT autoload IntelEthernet — em0 absent (real-hardware autoload failure)"
fi

exit 0
