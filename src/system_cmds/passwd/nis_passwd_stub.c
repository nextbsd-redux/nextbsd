/*
 * Stub for the NIS (Sun Yellow Pages) passwd backend.
 *
 * Apple's nis_passwd.c uses xdr_yppasswd from librpcsvc; modern
 * systems rarely run NIS. Stub it the same way we stub od_passwd:
 * passwd -i NIS prints a clear error and exits non-zero.
 */
#include <stdio.h>

int
nis_passwd(char *uname, char *locn)
{
	(void)uname; (void)locn;
	fprintf(stderr,
	    "passwd: NIS backend not built on this system; "
	    "use -i pam or -i file\n");
	return 1;
}
