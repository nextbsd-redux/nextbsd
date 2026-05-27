/*
 * Stub for Apple Open Directory backend.
 *
 * We don't ship libopendirectory on freebsd-launchd-mach (it's an
 * Apple-only directory service). passwd.c's CLI accepts -i
 * OpenDirectory but we make that case fail cleanly instead of linking
 * the real od_passwd.c (which #include's OpenDirectory framework
 * headers).
 *
 * The signature must match the prototype passwd.c expects:
 *   int od_passwd(char *uname, char *locn, char *aname);
 */
#include <stdio.h>

int
od_passwd(char *uname, char *locn, char *aname)
{
	(void)uname; (void)locn; (void)aname;
	fprintf(stderr,
	    "passwd: OpenDirectory backend not built on this system; "
	    "use -i pam or -i file\n");
	return 1;
}
