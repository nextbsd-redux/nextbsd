/* test_bsd_logger.c — minimal RFC3164 syslog client for Phase J
 * runtime validation. FreeBSD's logger(1) writes RFC5424 which
 * Apple's asl_input_parse doesn't fully extract — specifically
 * ASL_KEY_MSG remains empty so _act_file_final returns early.
 *
 * libc's syslog(3) writes RFC3164 format to /var/run/log SOCK_DGRAM
 * which our bsd_in_init has bound. Apple's parser handles RFC3164
 * cleanly.
 *
 * Usage: test_bsd_logger <tag> <message>
 */
#include <stdio.h>
#include <syslog.h>

int
main(int argc, char *argv[])
{
	if (argc != 3) {
		fprintf(stderr, "usage: %s <tag> <message>\n", argv[0]);
		return 1;
	}
	openlog(argv[1], LOG_PID, LOG_USER);
	syslog(LOG_NOTICE, "%s", argv[2]);
	closelog();
	return 0;
}
