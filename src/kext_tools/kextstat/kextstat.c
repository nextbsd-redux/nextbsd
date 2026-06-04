/*
 * kextstat — list loaded kernel extensions (NextBSD proof-of-concept).
 *
 * Minimal and kld-backed: Apple's kextstat queries the kernel through the
 * kmod_get_info() Mach RPC; here we enumerate FreeBSD's loaded kld files
 * with kldnext(2)/kldstat(2). No OSKext. This is the proof-of-concept trio
 * (nextbsd#183); the faithful kext_tools/OSKext port is tracked in
 * nextbsd#182.
 */
#include <sys/param.h>
#include <sys/linker.h>

#include <err.h>
#include <stdint.h>
#include <stdio.h>

int
main(void)
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
	return (0);
}
