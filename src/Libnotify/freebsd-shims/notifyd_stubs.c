/*
 * notifyd_stubs.c — link-time stubs for Apple-private symbols
 * notifyd references but we don't yet provide. Each returns a
 * reasonable failure value; daemon won't fully function until real
 * impls land in mach.ko / libxpc / libdispatch, but the binary
 * compiles + installs.
 *
 * Pattern matches launchd-842's forward_stubs.c.
 */
#include <bsm/libbsm.h>
#include <mach/mach.h>
#include <pthread.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>

/* audit_token_to_* — FreeBSD libbsm has audit_token_to_au32 but
 * not these individual extractors. Provide them by calling au32. */
pid_t audit_token_to_pid(audit_token_t atoken) __attribute__((weak));
pid_t
audit_token_to_pid(audit_token_t atoken)
{
	uid_t uid; gid_t gid; pid_t pid; au_asid_t asid;
	audit_token_to_au32(atoken, NULL, &uid, &gid, NULL, NULL, &pid, &asid, NULL);
	(void)uid; (void)gid;
	return pid;
}

uid_t audit_token_to_euid(audit_token_t atoken) __attribute__((weak));
uid_t
audit_token_to_euid(audit_token_t atoken)
{
	uid_t euid;
	audit_token_to_au32(atoken, NULL, &euid, NULL, NULL, NULL, NULL, NULL, NULL);
	return euid;
}

gid_t audit_token_to_egid(audit_token_t atoken) __attribute__((weak));
gid_t
audit_token_to_egid(audit_token_t atoken)
{
	gid_t egid;
	audit_token_to_au32(atoken, NULL, NULL, &egid, NULL, NULL, NULL, NULL, NULL);
	return egid;
}

/* sandbox stub — always return 0 (allowed). */
int
sandbox_check_by_audit_token(audit_token_t target, const char *op, int filter, ...)
{
	(void)target; (void)op; (void)filter;
	return 0;
}

/* pthread_setugid_np — Apple-private thread credential. FreeBSD
 * has no per-thread credentials; setuid would change the whole
 * process. notifyd uses this to drop privs for specific RPC
 * handlers; stub no-op (handler runs with daemon's normal creds). */
int
pthread_setugid_np(uid_t uid, gid_t gid)
{
	(void)uid; (void)gid;
	return 0;
}

/* Apple MIG special-reply-port helpers — used by sync MIG calls
 * for priority inheritance. Return MACH_PORT_NULL. */
mach_port_t
mig_get_special_reply_port(void)
{
	return MACH_PORT_NULL;
}

void
mig_dealloc_special_reply_port(mach_port_t port)
{
	(void)port;
}

/* current_task — Apple's per-thread task pointer. Same as
 * mach_task_self() for our purposes. */
mach_port_t
current_task(void)
{
	return mach_task_self();
}

/* mach_port_construct / mach_port_destruct — Apple's extended
 * port-creation API (with mach_port_options_t). For our runtime,
 * fall back to plain mach_port_allocate. */
struct mach_port_options;
kern_return_t
mach_port_construct(mach_port_t task, struct mach_port_options *opts,
                    mach_port_context_t context, mach_port_name_t *name)
{
	(void)opts; (void)context;
	return mach_port_allocate(task, MACH_PORT_RIGHT_RECEIVE, name);
}

kern_return_t
mach_port_destruct(mach_port_t task, mach_port_name_t name,
                   mach_port_delta_t srdelta, mach_port_context_t context)
{
	(void)srdelta; (void)context;
	return mach_port_deallocate(task, name);
}

/* XPC event publisher stubs. */
void
xpc_event_publisher_set_error_handler(void *publisher, void *handler)
{
	(void)publisher; (void)handler;
}

void
xpc_event_publisher_set_throttling(void *publisher, uint64_t interval)
{
	(void)publisher; (void)interval;
}

/* OS_BUG_INTERNAL — a "soft bug" marker function from os/log. */
void
OS_BUG_INTERNAL(const char *fmt, ...)
{
	(void)fmt;
}

/*
 * syslogd-specific stubs (re-used by syslogd via .PATH lift of this
 * same file).
 */

/* os_release — Apple's reference-counting release. No-op. */
void os_release(void *obj) { (void)obj; }

/* voucher_mach_msg_adopt / revert — voucher inheritance for sync
 * MIG calls. We don't have vouchers; no-op. */
typedef mach_port_t voucher_mach_msg_state_t_local;
voucher_mach_msg_state_t_local
voucher_mach_msg_adopt(mach_msg_header_t *msg)
{
	(void)msg;
	return MACH_PORT_NULL;
}

void
voucher_mach_msg_revert(voucher_mach_msg_state_t_local state)
{
	(void)state;
}

/* qtn_proc_set_* stubs (quarantine.h has the alloc/init versions). */
int
qtn_proc_set_identifier(void *qp, const char *id) { (void)qp; (void)id; return 0; }
int
qtn_proc_set_flags(void *qp, uint32_t flags) { (void)qp; (void)flags; return 0; }

/* _malloc_no_asl_log — Apple's flag variable, NOT a function. syslogd
 * does `_malloc_no_asl_log = 1;` (syslogd.c:517) to tell libmalloc
 * to skip recursive ASL logging on malloc errors. Earlier stub
 * mistakenly defined it as a function, which caused syslogd to
 * SIGSEGV with SEGV_ACCERR when writing 1 to the function's text-
 * segment address (read-only). Define as int — extern decl in
 * syslogd.c sees it correctly. */
int _malloc_no_asl_log;

/* configuration_profile_create_notification_key — Apple MDM API.
 * Stub returns NULL (no MDM profile so no notification key). */
void *
configuration_profile_create_notification_key(const char *path)
{
	(void)path;
	return NULL;
}

/* vm_allocate / vm_deallocate — Apple's Mach VM wrappers. Our libmach
 * doesn't have mach_vm_*; fall through to mmap/munmap. */
#include <sys/mman.h>
kern_return_t
vm_allocate(mach_port_name_t target_task, vm_address_t *address,
            vm_size_t size, int flags)
{
	(void)target_task; (void)flags;
	void *p = mmap(NULL, size, PROT_READ|PROT_WRITE,
	    MAP_ANON|MAP_PRIVATE, -1, 0);
	if (p == MAP_FAILED) return KERN_NO_SPACE;
	*address = (vm_address_t)p;
	return KERN_SUCCESS;
}

kern_return_t
vm_deallocate(mach_port_name_t target_task, vm_address_t address, vm_size_t size)
{
	(void)target_task;
	if (munmap((void *)address, size) != 0) return KERN_FAILURE;
	return KERN_SUCCESS;
}

/* copyfile — aslmanager uses this to move a single ASL log file
 * into the Archive/ directory. We don't implement the full Darwin
 * copyfile(3) API; just enough for that single-file case. The
 * COPYFILE_RECURSIVE flag is set in the call site but the source
 * is always a regular file in practice. */
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
int
copyfile(const char *from, const char *to, void *state, uint32_t flags)
{
	(void)state; (void)flags;
	int sfd = -1, dfd = -1;
	struct stat st;
	char buf[64 * 1024];
	ssize_t n;
	int rc = -1;

	if ((sfd = open(from, O_RDONLY)) < 0) goto out;
	if (fstat(sfd, &st) < 0) goto out;
	if ((dfd = open(to, O_WRONLY|O_CREAT|O_TRUNC,
	    st.st_mode & 0777)) < 0) goto out;
	while ((n = read(sfd, buf, sizeof(buf))) > 0) {
		ssize_t off = 0;
		while (off < n) {
			ssize_t w = write(dfd, buf + off, n - off);
			if (w <= 0) goto out;
			off += w;
		}
	}
	if (n < 0) goto out;
	rc = 0;
out:
	if (sfd >= 0) close(sfd);
	if (dfd >= 0) close(dfd);
	return rc;
}
