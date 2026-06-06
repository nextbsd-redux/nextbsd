/*
 * test_kextd_mach — K3b round-trip: prove the kernel can send a Mach
 * load-request to a userland receive right registered as HOST_KEXTD_PORT.
 *
 * This is the de-risk for the in-kernel IOKit matcher's kextd hand-off (#216).
 * It mimics exactly what kextd will do, minus the actual load:
 *
 *   1. Allocate a port (RECEIVE + MAKE_SEND) and register it host-wide as
 *      HOST_KEXTD_PORT (the slot the kernel matcher sends to).
 *   2. ioctl(/dev/iocatalogue, IOCATIOCTESTSEND, 0x24f38086) — ask the kernel
 *      to look up the Intel 8260 and fire iokit_kextd_send() for the winning
 *      bundle to HOST_KEXTD_PORT. (Requires kextd to have pushed the IntelWiFi
 *      personality first — run.sh does that.)
 *   3. mach_msg(MACH_RCV) on our port and confirm the message carries
 *      bundle_id "org.nextbsd.kext.intelwifi" and match 0x24f38086.
 *
 * Prints KEXTD-MACH-OK / KEXTD-MACH-FAIL / KEXTD-MACH-SKIP for the boot test.
 * SKIP when /dev/iocatalogue or the IOCATIOCTESTSEND/HOST_KEXTD_PORT plumbing
 * is absent (a kernel predating K3b).
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>

#include <mach/mach_traps.h>
#include <mach/mach_port.h>
#include <mach/host_special_ports.h>
#include <mach/message.h>

#include <sys/iocatalogue.h>	/* IOCATIOCTESTSEND */

#ifndef HOST_KEXTD_PORT
#define HOST_KEXTD_PORT 15
#endif

#define	IWL_8260_MATCH	0x24f38086u
#define	WANT_BUNDLE	"org.nextbsd.kext.intelwifi"

/* Mirror of the kernel's iokit_kextd_load_msg_t body (sys/mach/iokit_kextd.h),
 * minus the trailer the kernel appends on receive. NDR_record_t is 8 bytes. */
typedef struct {
	mach_msg_header_t	hdr;
	unsigned char		ndr[8];
	char			bundle_id[128];
	char			device[64];
	uint32_t		match_word;
} kextd_load_body_t;

int
main(void)
{
	mach_port_name_t task = mach_task_self();
	mach_port_name_t host = mach_host_self();
	mach_port_name_t port = MACH_PORT_NULL;
	kern_return_t kr;
	uint32_t mw = IWL_8260_MATCH;
	int fd, rc;
	union {
		kextd_load_body_t body;
		unsigned char raw[sizeof(kextd_load_body_t) + 64]; /* trailer slack */
	} buf;
	mach_msg_return_t mr;

	fd = open("/dev/iocatalogue", O_RDWR);
	if (fd < 0) {
		printf("KEXTD-MACH-SKIP: no /dev/iocatalogue (pre-K2 kernel)\n");
		return (0);
	}

	kr = mach_port_allocate(task, MACH_PORT_RIGHT_RECEIVE, &port);
	if (kr != KERN_SUCCESS) {
		printf("KEXTD-MACH-FAIL: mach_port_allocate 0x%x\n", (unsigned)kr);
		return (1);
	}
	kr = mach_port_insert_right(task, port, port, MACH_MSG_TYPE_MAKE_SEND);
	if (kr != KERN_SUCCESS) {
		printf("KEXTD-MACH-FAIL: mach_port_insert_right 0x%x\n", (unsigned)kr);
		return (1);
	}
	kr = host_set_special_port(host, HOST_KEXTD_PORT, port);
	if (kr != KERN_SUCCESS) {
		printf("KEXTD-MACH-FAIL: host_set_special_port(HOST_KEXTD_PORT) 0x%x\n",
		    (unsigned)kr);
		return (1);
	}
	printf("registered HOST_KEXTD_PORT = 0x%x\n", port);

	/* Ask the kernel to match the 8260 and send the load request to us.
	 * NOTE: the serial console splits long lines and the boot test matches
	 * the marker token as soon as it appears, so always print the diagnostic
	 * on its OWN line first, then a bare KEXTD-MACH-{OK,FAIL,SKIP} marker. */
	rc = ioctl(fd, IOCATIOCTESTSEND, &mw);
	printf("IOCATIOCTESTSEND rc=%d errno=%d\n", rc, rc != 0 ? errno : 0);
	if (rc != 0) {
		(void) host_set_special_port(host, HOST_KEXTD_PORT, MACH_PORT_NULL);
		if (errno == ENOTTY) {
			printf("(no IOCATIOCTESTSEND — pre-K3b kernel)\n");
			printf("KEXTD-MACH-SKIP\n");
			return (0);
		}
		printf("(errno %d: %s)\n", errno,
		    errno == ENOENT ? "8260 not in catalogue" :
		    errno == ENXIO ? "kernel saw no kextd port" : "?");
		printf("KEXTD-MACH-FAIL\n");
		return (1);
	}

	/* The send is synchronous in the ioctl, so the message is already
	 * queued; receive it (timeout as a guard). */
	memset(&buf, 0, sizeof(buf));
	mr = mach_msg(&buf.body.hdr, MACH_RCV_MSG | MACH_RCV_TIMEOUT,
	    0, sizeof(buf), port, 5000 /* ms */, MACH_PORT_NULL);
	(void) host_set_special_port(host, HOST_KEXTD_PORT, MACH_PORT_NULL);
	printf("mach_msg(RCV) = 0x%x\n", (unsigned)mr);

	if (mr != MACH_MSG_SUCCESS) {
		printf("(receive failed)\n");
		printf("KEXTD-MACH-FAIL\n");
		return (1);
	}

	buf.body.bundle_id[sizeof(buf.body.bundle_id) - 1] = '\0';
	printf("received: msgid=0x%x size=%u bundle='%s' match=0x%08x\n",
	    buf.body.hdr.msgh_id, (unsigned)buf.body.hdr.msgh_size,
	    buf.body.bundle_id, buf.body.match_word);

	if (strcmp(buf.body.bundle_id, WANT_BUNDLE) == 0 &&
	    buf.body.match_word == IWL_8260_MATCH) {
		printf("KEXTD-MACH-OK\n");
		return (0);
	}
	printf("(contents mismatch — want '%s'/0x%08x)\n", WANT_BUNDLE, IWL_8260_MATCH);
	printf("KEXTD-MACH-FAIL\n");
	return (1);
}
