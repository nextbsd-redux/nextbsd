/*
 * config_link_monitor.c — in-process KernelEventMonitor (#168 follow-up, #257).
 *
 * Apple's KernelEventMonitor is a configd plugin sharing configd's event loop;
 * this port previously ran it as a standalone daemon (src/KernelEventMonitor,
 * PR #167) because a stock kernel had no EVFILT_MACHPORT and configd's single
 * mach_msg loop couldn't multiplex a Mach port and the PF_ROUTE socket. Now
 * that native EVFILT_MACHPORT delivery works (#168 Stages 0-4) the daemon is
 * redundant, so the route->Link translation moves in-process here, retiring the
 * standalone daemon + its launchd plist.
 *
 * Job (unchanged from the daemon): read the PF_ROUTE socket for RTM_IFINFO
 * link-state changes and publish Apple's canonical key
 *   State:/Network/Interface/<ifname>/Link = { Active : <bool> }
 * so ipconfigd (sc_link_watch) starts DHCP when a NIC links up.
 *
 * Mechanism: a dedicated thread inside configd. Rather than reach into the
 * store directly (which would need a lock around config_store / config_session,
 * both written for configd's single serve thread), it publishes the way every
 * other client does — as a configd session over the MIG config.defs client
 * stubs (configUser.c, already linked for --selftest). The store write therefore
 * happens on configd's serve thread, fully serialized; this thread only reads
 * the route socket and issues RPCs to configd's own service. The value is the
 * same XML-plist byte blob SCDynamicStoreSetValue would send (configd stores it
 * opaque; ipconfigd's SCDynamicStoreCopyValue parses it back), and the key is
 * the raw UTF-8 string _SCSerializeString produces — so the published key/value
 * are byte-for-byte what an external SC client would have written.
 *
 * Because the monitor lives in configd's own address space, the session never
 * sees a "configd restarted" dead-server error (they share a lifetime), so the
 * standalone daemon's reopen/re-seed dance is unnecessary.
 */
#include <sys/types.h>
#include <sys/socket.h>

#include <net/if.h>
#include <net/route.h>

#include <errno.h>
#include <ifaddrs.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <mach/mach.h>
#include <servers/bootstrap.h>

#include "config_types.h"		/* xmlData, SCD_SERVER, kSCStatusOK */
#include "config_link_monitor.h"

/* config.defs MIG client stubs (configUser.c). */
extern kern_return_t configopen(mach_port_t, xmlData, mach_msg_type_number_t,
    xmlData, mach_msg_type_number_t, mach_port_t *, int *);
extern kern_return_t configset(mach_port_t, xmlData, mach_msg_type_number_t,
    xmlData, mach_msg_type_number_t, int, int *, int *);

/* liblaunch global — the bootstrap port configd checked its service in on. */
extern mach_port_t bootstrap_port;

/* The configd session this monitor publishes through (monitor thread only). */
static mach_port_t g_session = MACH_PORT_NULL;

static void
lmlog(const char *fmt, ...)
{
	va_list ap;

	(void)fprintf(stderr, "configd[linkmon] ");
	va_start(ap, fmt);
	(void)vfprintf(stderr, fmt, ap);
	va_end(ap);
	(void)fputc('\n', stderr);
	(void)fflush(stderr);
}

/*
 * Map a (link_state, if_flags) pair to Apple's "Active" boolean — verbatim
 * from the standalone daemon. LINK_STATE_UP is yes; LINK_STATE_DOWN is no;
 * LINK_STATE_UNKNOWN (media-status-less NICs) falls back to IFF_UP.
 */
static int
link_active(int link_state, int if_flags)
{
	if (link_state == LINK_STATE_UP)
		return (1);
	if (link_state == LINK_STATE_DOWN)
		return (0);
	return ((if_flags & IFF_UP) != 0);	/* LINK_STATE_UNKNOWN */
}

/*
 * The XML-plist value byte blob for { Active : <bool> } — exactly the form
 * CFPropertyListCreateData(kCFPropertyListXMLFormat_v1_0) produces and that
 * configd stores opaque + ipconfigd's SCDynamicStoreCopyValue parses back.
 */
static const char *
link_value_xml(int active)
{
	static const char xml_true[] =
	    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
	    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
	    "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
	    "<plist version=\"1.0\">\n"
	    "<dict>\n\t<key>Active</key>\n\t<true/>\n</dict>\n</plist>\n";
	static const char xml_false[] =
	    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
	    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
	    "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
	    "<plist version=\"1.0\">\n"
	    "<dict>\n\t<key>Active</key>\n\t<false/>\n</dict>\n</plist>\n";

	return (active ? xml_true : xml_false);
}

/*
 * Publish State:/Network/Interface/<ifname>/Link = { Active:bool } via the
 * configd session. The key is the raw UTF-8 string (what _SCSerializeString
 * yields); the value is the XML-plist blob. Loopback never DHCPs, so skip it.
 */
static void
publish_link(const char *ifname, int active)
{
	char		key[128];
	const char	*val;
	int		keylen, vallen;
	int		newInstance = 0, status = 0;
	kern_return_t	kr;

	if (strcmp(ifname, "lo0") == 0)
		return;
	if (g_session == MACH_PORT_NULL)
		return;

	keylen = snprintf(key, sizeof(key),
	    "State:/Network/Interface/%s/Link", ifname);
	if (keylen <= 0 || (size_t)keylen >= sizeof(key))
		return;
	val = link_value_xml(active);
	vallen = (int)strlen(val);

	kr = configset(g_session, (uint8_t *)key,
	    (mach_msg_type_number_t)keylen, (uint8_t *)(uintptr_t)val,
	    (mach_msg_type_number_t)vallen, 0, &newInstance, &status);
	if (kr != KERN_SUCCESS || status != kSCStatusOK) {
		lmlog("KEM-LINK-FAIL: configset(%s) kr=0x%x status=%d",
		    ifname, (unsigned)kr, status);
		return;
	}
	/* KEM-LINK-OK gates in boot-test.sh: a link-state change reached the
	 * store — the signal ipconfigd's watch depends on. */
	lmlog("KEM-LINK-OK: %s Active=%d", ifname, active);
}

/*
 * Publish the current link state of every interface (startup snapshot, so a
 * watcher already up before any RTM_IFINFO still sees the state). The AF_LINK
 * ifaddr's ifa_data is a `struct if_data *` carrying ifi_link_state.
 */
static void
publish_all(void)
{
	struct ifaddrs *ifa, *p;

	if (getifaddrs(&ifa) != 0) {
		lmlog("getifaddrs failed: %s", strerror(errno));
		return;
	}
	for (p = ifa; p != NULL; p = p->ifa_next) {
		const struct if_data *ifd;

		if (p->ifa_addr == NULL ||
		    p->ifa_addr->sa_family != AF_LINK ||
		    p->ifa_data == NULL)
			continue;
		ifd = (const struct if_data *)p->ifa_data;
		publish_link(p->ifa_name,
		    link_active(ifd->ifi_link_state, (int)p->ifa_flags));
	}
	freeifaddrs(ifa);
}

/*
 * Open the configd session this monitor publishes through. Looks the configd
 * service up via the bootstrap port (configd has already checked it in by the
 * time the monitor starts) and issues configopen, retrying so a momentary race
 * with the serve loop reaching its first receive is harmless.
 */
static int
open_session(void)
{
	mach_port_t	server = MACH_PORT_NULL;
	uint8_t		name[] = "KernelEventMonitor";
	uint8_t		empty[1] = { 0 };
	int		status = 0;
	int		tries;

	for (tries = 0; tries < 120; tries++) {
		kern_return_t kr;

		if (server == MACH_PORT_NULL) {
			kr = bootstrap_look_up(bootstrap_port, SCD_SERVER,
			    &server);
			if (kr != KERN_SUCCESS || server == MACH_PORT_NULL) {
				(void)sleep(1);
				continue;
			}
		}
		kr = configopen(server, name,
		    (mach_msg_type_number_t)(sizeof(name) - 1), empty, 0,
		    &g_session, &status);
		if (kr == KERN_SUCCESS && status == kSCStatusOK &&
		    g_session != MACH_PORT_NULL)
			return (0);
		(void)sleep(1);
	}
	lmlog("KEM-FAIL: could not open a configd session after 120s");
	return (-1);
}

static void *
monitor_thread(void *arg)
{
	int rs;

	(void)arg;

	if (open_session() != 0)
		return (NULL);

	rs = socket(PF_ROUTE, SOCK_RAW, 0);
	if (rs < 0) {
		lmlog("KEM-FAIL: socket(PF_ROUTE): %s", strerror(errno));
		return (NULL);
	}

	/* Seed the store before blocking on the route socket. */
	publish_all();

	for (;;) {
		char			buf[2048];
		struct rt_msghdr	*rtm;
		struct if_msghdr	*ifm;
		char			ifname[IFNAMSIZ];
		ssize_t			n;

		n = read(rs, buf, sizeof(buf));
		if (n <= 0) {
			if (n < 0 && errno == EINTR)
				continue;
			lmlog("KEM-FAIL: read(PF_ROUTE): %s",
			    n < 0 ? strerror(errno) : "EOF");
			break;
		}
		if ((size_t)n < sizeof(struct rt_msghdr))
			continue;
		rtm = (struct rt_msghdr *)(void *)buf;
		if (rtm->rtm_version != RTM_VERSION)
			continue;
		if (rtm->rtm_type != RTM_IFINFO)
			continue;
		ifm = (struct if_msghdr *)(void *)buf;
		if (if_indextoname(ifm->ifm_index, ifname) == NULL)
			continue;

		publish_link(ifname,
		    link_active(ifm->ifm_data.ifi_link_state, ifm->ifm_flags));
	}

	(void)close(rs);
	return (NULL);
}

void
config_link_monitor_start(void)
{
	pthread_t	th;

	if (pthread_create(&th, NULL, monitor_thread, NULL) != 0) {
		lmlog("KEM-FAIL: pthread_create: %s", strerror(errno));
		return;
	}
	(void)pthread_detach(th);
	lmlog("link monitor started (PF_ROUTE -> "
	    "State:/Network/Interface/<if>/Link, in-process)");
}
