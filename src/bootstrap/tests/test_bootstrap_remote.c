/*
 * test_bootstrap_remote — Phase G2d cross-process validation.
 *
 * Runs as a separate process from bootstrap_server (run.sh starts
 * the daemon in the background before invoking us). This tests
 * the *real* bootstrap path: a fresh process whose per-task
 * itk_bootstrap is null finds the daemon's port via the
 * HOST_BOOTSTRAP_PORT fallback wired up in Phase G2b.
 *
 * Flow:
 *   1. task_get_bootstrap_port(self, &bp) — pulls a send right
 *      from realhost.special[HOST_BOOTSTRAP_PORT] (the daemon
 *      published this at startup).
 *   2. bootstrap_check_in(bp, "com.example.remote") — daemon
 *      allocates a port, returns it.
 *   3. bootstrap_look_up(bp, "com.example.remote") — daemon
 *      returns a send right to the same kernel port.
 *   4. Assert both round-trips returned non-NULL, and that the
 *      port name matches between the two calls (Mach guarantees
 *      that the same kernel port maps to the same name in a given
 *      task's IPC space).
 *
 * Exit codes:
 *   0 — round-trip succeeded.
 *   1 — task_get_bootstrap_port returned null (daemon not running
 *       or host slot empty).
 *   2 — bootstrap_check_in failed or returned null.
 *   3 — bootstrap_look_up failed or returned null.
 *   4 — check_in port != look_up port (different ports for same
 *       service name — protocol bug or daemon state issue).
 */
#include <stdio.h>
#include <stdlib.h>

#include <mach/mach_traps.h>
#include <mach/mach_port.h>
#include <mach/task_special_ports.h>
#include <servers/bootstrap.h>

int
main(void)
{
	mach_port_name_t task = mach_task_self();
	mach_port_name_t bp   = MACH_PORT_NULL;
	mach_port_t checkin_port = MACH_PORT_NULL;
	mach_port_t lookup_port  = MACH_PORT_NULL;
	kern_return_t kr;

	printf("task = 0x%x\n", task);

	/* Task #39 (2026-05-19): launchd's fork eventhandler now propagates
	 * its bootstrap_port to child tasks (see kern/task.c
	 * mach_task_fork_bsport). That means run.sh → getty → login → run.sh
	 * → us inherits launchd's per-task bootstrap port instead of NULL.
	 *
	 * This test specifically validates the HOST_BOOTSTRAP_PORT fallback
	 * path (bootstrap_port NULL → kernel uses realhost.special slot).
	 * Force that path by clearing our inherited per-task slot. The
	 * launchd-as-bootstrap path is exercised elsewhere via the real
	 * MachServices CHECKIN flow.
	 */
	(void)task_set_bootstrap_port(task, MACH_PORT_NULL);

	/* 1. Bootstrap-port discovery via HOST_BOOTSTRAP_PORT fallback.
	 * Per-task slot now null (just cleared above); kernel falls back
	 * to realhost.special[HOST_BOOTSTRAP_PORT] which the daemon
	 * populated at its startup. */
	kr = task_get_bootstrap_port(task, &bp);
	if (kr != KERN_SUCCESS || bp == MACH_PORT_NULL) {
		printf("FAIL: task_get_bootstrap_port: kr=0x%x bp=0x%x "
		    "(bootstrap_server not running, or host slot empty)\n",
		    (unsigned)kr, bp);
		return (1);
	}
	printf("bootstrap_port = 0x%x\n", bp);

	/* 2. check_in. */
	kr = bootstrap_check_in(bp, "com.example.remote", &checkin_port);
	if (kr != KERN_SUCCESS) {
		printf("FAIL: bootstrap_check_in: 0x%x\n", (unsigned)kr);
		return (2);
	}
	if (checkin_port == MACH_PORT_NULL) {
		printf("FAIL: bootstrap_check_in returned MACH_PORT_NULL\n");
		return (2);
	}
	printf("bootstrap_check_in OK: port = 0x%x\n", checkin_port);

	/* 3. look_up. */
	kr = bootstrap_look_up(bp, "com.example.remote", &lookup_port);
	if (kr != KERN_SUCCESS) {
		printf("FAIL: bootstrap_look_up: 0x%x\n", (unsigned)kr);
		return (3);
	}
	if (lookup_port == MACH_PORT_NULL) {
		printf("FAIL: bootstrap_look_up returned MACH_PORT_NULL\n");
		return (3);
	}
	printf("bootstrap_look_up  OK: port = 0x%x\n", lookup_port);

	/* 4. Same-kernel-port invariant: Mach gives the same name for
	 * the same kernel port in a given IPC space. check_in and
	 * look_up return send rights to the same daemon-allocated
	 * port, so we should see the same name twice. */
	if (lookup_port != checkin_port) {
		printf("FAIL: check_in returned 0x%x but look_up returned 0x%x\n",
		    checkin_port, lookup_port);
		return (4);
	}

	printf("BOOTSTRAP-REMOTE-OK: cross-process check_in/look_up "
	    "round-trip succeeded (port=0x%x)\n", checkin_port);
	return (0);
}
