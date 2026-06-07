/*
 * kextstat — list loaded kernel extensions (NextBSD proof-of-concept).
 *
 * Minimal and kld-backed: Apple's kextstat queries the kernel through the
 * kmod_get_info() Mach RPC; here we enumerate FreeBSD's loaded kld files
 * with kldnext(2)/kldstat(2). No OSKext. This is the proof-of-concept trio
 * (nextbsd#183); the faithful kext_tools/OSKext port is tracked in
 * nextbsd#182.
 *
 * It also accepts `-m <module>` (mirroring the retired kldstat(1) flag):
 * exit 0 if a kernel MODULE of that name is registered (modfind(2)), else
 * exit 1. This lets kextstat stand in for `kldstat -m` after the kld* CLIs
 * were retired (nextbsd#193). modfind(2)/kldnext(2)/kldstat(2) are all KEPT
 * syscalls, so it works the same whether the module is a separate .ko or
 * compiled into the kernel.
 */
#include <sys/param.h>
#include <sys/linker.h>
#include <sys/module.h>

#include <err.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

static void
list_loaded(void)
{
	int fileid;

	printf("Id Refs Address              Size       Name\n");
	for (fileid = kldnext(0); fileid > 0; fileid = kldnext(fileid)) {
		struct kld_file_stat stat;

		stat.version = sizeof(stat);
		if (kldstat(fileid, &stat) < 0) {
			warn("kldstat(%d)", fileid);
			continue;
		}
		printf("%2d %4d 0x%-16jx 0x%-8zx %s\n",
		    stat.id, stat.refs, (uintmax_t)(uintptr_t)stat.address,
		    stat.size, stat.name);
	}
}

int
main(int argc, char **argv)
{
	const char *module = NULL;
	int ch;

	while ((ch = getopt(argc, argv, "m:")) != -1) {
		switch (ch) {
		case 'm':
			module = optarg;
			break;
		default:
			fprintf(stderr, "usage: kextstat [-m module]\n");
			return (2);
		}
	}

	if (module != NULL) {
		/* Module-presence query (replaces `kldstat -m <module>`). */
		if (modfind(module) < 0)
			return (1);
		return (0);
	}

	list_loaded();
	return (0);
}
