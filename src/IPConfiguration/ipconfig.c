/*
 * ipconfig — Apple's IPConfiguration CLI, ported to freebsd-launchd-mach.
 *
 * Iter 8 (the closer for the IPConfiguration port story): a small
 * Mach RPC client of `com.apple.IPConfiguration` mirroring Apple's
 * bootp/ipconfig.tproj/client.c shape — a `commands[]` lookup table
 * with one tiny dispatch function per subcommand. Iter 8 ships the
 * two subcommands the existing 2-routine MIG surface can serve:
 *
 *   ipconfig getifaddr <ifname>     →  ipconfig_if_addr
 *   ipconfig ifcount                →  ipconfig_if_count
 *
 * Apple's client.c is 1630 LOC because it covers ~25 subcommands
 * (waitall, getoption, getsummary, getpacket, getv6packet, getra,
 * getdhcpduid, set, addService, BSDP, NetBoot, CLAT46, …). Every
 * one of those needs a MIG routine our ipconfig.defs doesn't have
 * yet; they grow in later iters by appending rows to commands[] +
 * one routine each in ipconfig.defs. Fresh ~150 LOC keeps the
 * binary clean and the path to "vendor-shape" growth obvious.
 *
 * Install: /usr/sbin/ipconfig.
 */
#include <mach/mach.h>
#include <servers/bootstrap.h>

#include <arpa/inet.h>
#include <netinet/in.h>

#include "ipconfig_mig_types.h"
#include "ipconfig.h"		/* MIG: ipconfig_* client stubs */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *progname = "ipconfig";

static int
S_getifaddr(mach_port_t svc, int argc, char **argv)
{
	InterfaceName name;
	ip_address_t addr = 0;
	ipconfig_status_t status = ipconfig_status_internal_error_e;
	kern_return_t kr;
	struct in_addr in_a;
	char buf[INET_ADDRSTRLEN];

	if (argc < 1) {
		(void)fprintf(stderr, "usage: %s getifaddr <ifname>\n",
		    progname);
		return (1);
	}

	(void)memset(name, 0, sizeof(name));
	(void)strlcpy(name, argv[0], sizeof(name));

	kr = ipconfig_if_addr(svc, name, &addr, &status);
	if (kr != KERN_SUCCESS) {
		(void)fprintf(stderr, "%s: getifaddr %s: mach 0x%x\n",
		    progname, name, (unsigned)kr);
		return (1);
	}
	if (status != ipconfig_status_success_e) {
		(void)fprintf(stderr, "%s: getifaddr %s: status %d\n",
		    progname, name, (int)status);
		return (1);
	}

	in_a.s_addr = addr;
	if (inet_ntop(AF_INET, &in_a, buf, sizeof(buf)) == NULL) {
		(void)fprintf(stderr, "%s: getifaddr %s: inet_ntop failed\n",
		    progname, name);
		return (1);
	}
	(void)printf("%s\n", buf);
	return (0);
}

static int
S_ifcount(mach_port_t svc, int argc, char **argv)
{
	int count = -1;
	kern_return_t kr;

	(void)argc;
	(void)argv;

	kr = ipconfig_if_count(svc, &count);
	if (kr != KERN_SUCCESS) {
		(void)fprintf(stderr, "%s: ifcount: mach 0x%x\n", progname,
		    (unsigned)kr);
		return (1);
	}
	(void)printf("%d\n", count);
	return (0);
}

static const struct command_info {
	const char	*name;
	int		(*func)(mach_port_t, int, char **);
	int		min_argc;
	const char	*usage;
} commands[] = {
	{ "getifaddr",	S_getifaddr,	1, "<ifname>" },
	{ "ifcount",	S_ifcount,	0, "" },
	{ NULL,		NULL,		0, NULL },
};

static void
usage(void)
{
	int i;

	(void)fprintf(stderr, "usage: %s <command> [args]\n", progname);
	(void)fprintf(stderr, "commands:\n");
	for (i = 0; commands[i].name != NULL; i++) {
		(void)fprintf(stderr, "  %s %s\n", commands[i].name,
		    commands[i].usage);
	}
	exit(2);
}

int
main(int argc, char **argv)
{
	mach_port_t svc = MACH_PORT_NULL;
	kern_return_t kr;
	int i;

	if (argv[0] != NULL) {
		const char *slash = strrchr(argv[0], '/');

		progname = (slash != NULL) ? slash + 1 : argv[0];
	}

	if (argc < 2)
		usage();

	for (i = 0; commands[i].name != NULL; i++) {
		if (strcasecmp(argv[1], commands[i].name) != 0)
			continue;
		if ((argc - 2) < commands[i].min_argc) {
			(void)fprintf(stderr,
			    "%s: %s needs %d arg(s): %s %s %s\n",
			    progname, commands[i].name,
			    commands[i].min_argc, progname,
			    commands[i].name, commands[i].usage);
			return (2);
		}
		kr = bootstrap_look_up(bootstrap_port,
		    "com.apple.IPConfiguration", &svc);
		if (kr != KERN_SUCCESS || svc == MACH_PORT_NULL) {
			(void)fprintf(stderr,
			    "%s: bootstrap_look_up failed: 0x%x\n",
			    progname, (unsigned)kr);
			return (1);
		}
		return (commands[i].func(svc, argc - 2, &argv[2]));
	}

	(void)fprintf(stderr, "%s: unknown command '%s'\n", progname, argv[1]);
	usage();
	return (2);
}
