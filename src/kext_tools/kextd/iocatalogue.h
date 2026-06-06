/*
 * NextBSD in-kernel IOKit catalogue (K2, nextbsd#215).
 *
 * A flat, in-kernel database of IOKit driver personalities (device-id match
 * records) — the Apple-faithful IOCatalogue. Userland (kextd) parses each kext
 * bundle's Info.plist IOKitPersonalities and PUSHES a flat match-record per
 * personality here via ioctl on /dev/iocatalogue. This is mechanism (a): the
 * kernel never parses XML — userland owns plist parsing, the kernel owns the
 * store + matching. The in-kernel matcher (K3, nextbsd#216) reads these records
 * to bind an unmatched device to its driver bundle.
 *
 * See pkgdemon.github.io/nextbsd-inkernel-iokit-feasibility.html §9.
 */
#ifndef _SYS_IOCATALOGUE_H_
#define _SYS_IOCATALOGUE_H_

#include <sys/types.h>
#include <sys/ioccom.h>

#define	IOCAT_BUNDLE_ID_MAX	128	/* max CFBundleIdentifier incl. NUL */
#define	IOCAT_MAX_MATCH		512	/* max device-id entries per personality */

/* IOProviderClass — stored opaque by K2; interpreted by the K3 matcher. */
#define	IOCAT_PROVIDER_UNKNOWN		0
#define	IOCAT_PROVIDER_IOPCIDEVICE	1

/*
 * One personality, as pushed from userland. `match` points at an array of
 * `nmatch` uint32_t PCI match words, each encoded 0x<device><vendor> (device in
 * the high 16 bits, vendor in the low 16) — the IOPCIPrimaryMatch form, e.g.
 * 0x24f38086 for the Intel 8260. `match` is a uint64_t so the ABI is identical
 * for 32- and 64-bit userland; `_pad` keeps it 8-byte aligned deterministically.
 */
struct iocat_add {
	char		bundle_id[IOCAT_BUNDLE_ID_MAX];
	uint32_t	provider_class;		/* IOCAT_PROVIDER_* */
	int32_t		probe_score;		/* IOProbeScore; higher wins */
	uint32_t	nmatch;			/* number of entries in match[] */
	uint32_t	_pad;
	uint64_t	match;			/* user ptr to uint32_t[nmatch] */
};

/*
 * Look up the best driver bundle for a PCI match word (0x<device><vendor>).
 * Userland sets `match`; the kernel fills `bundle_id` + `score` and returns 0,
 * or ENOENT if nothing matches. This is the same lookup the in-kernel
 * device_nomatch matcher (K3) uses — exposed so userland can verify it
 * deterministically (e.g. is the 8260 bound to IntelWiFi?).
 */
struct iocat_lookup {
	uint32_t	match;				/* in: 0x<device><vendor> */
	int32_t		score;				/* out: winning IOProbeScore */
	char		bundle_id[IOCAT_BUNDLE_ID_MAX];	/* out: winning bundle */
};

#define	IOCATIOCADD	_IOW('K', 1, struct iocat_add)		/* add a personality */
#define	IOCATIOCFLUSH	_IO('K', 2)				/* drop all (re-push) */
#define	IOCATIOCLOOKUP	_IOWR('K', 3, struct iocat_lookup)	/* match a PCI word */
/*
 * K3b PoC (#216): look up a PCI match word and, on a hit, fire the kernel->kextd
 * Mach load request (HOST_KEXTD_PORT) for the winning bundle — the de-risk test
 * for the matcher's real send. Throwaway: the production matcher sends from its
 * device_nomatch taskqueue, not via an ioctl. Returns 0 / ENOENT (no match) /
 * ENXIO (no kextd registered) / ENOSYS (kernel built without COMPAT_MACH).
 */
#define	IOCATIOCTESTSEND	_IOW('K', 4, uint32_t)

#ifdef _KERNEL
#include <sys/queue.h>

/* In-kernel record (one personality). match[] is malloc'd, nmatch words. */
struct iocat_record {
	TAILQ_ENTRY(iocat_record) link;
	char		bundle_id[IOCAT_BUNDLE_ID_MAX];
	uint32_t	provider_class;
	int32_t		probe_score;
	uint32_t	nmatch;
	uint32_t       *match;
};

/*
 * K3 matcher entry point: find the best (highest probe_score) IOPCIDevice
 * personality whose match table contains `match_word` (0x<device><vendor>).
 * On a hit, copies the winning bundle id into buf and returns 0 (and the score
 * via score_out if non-NULL); returns ENOENT if nothing matches. Takes its own
 * lock; caller holds none.
 */
int	iocat_lookup_pci(uint32_t match_word, char *buf, size_t buflen,
	    int32_t *score_out);
#endif /* _KERNEL */

#endif /* _SYS_IOCATALOGUE_H_ */
