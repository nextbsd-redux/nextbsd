/*
 * config_session.h — configd's per-session state, key watches and
 * change-notification fan-out (configd iter 4).
 *
 * Apple's configd keeps a `serverSession` per client, keyed by the
 * client's session Mach port, and a watcher list alongside each store
 * value (configd.tproj/session.c, _SCD.c). This port mirrors that with
 * a CF-free model:
 *
 *   - configopen allocates a fresh per-session receive right, hands the
 *     client a send right to it, and arms a MACH_NOTIFY_NO_SENDERS
 *     notification so the session is torn down when the client exits.
 *     Every per-session port joins configd's port set, so the single
 *     mach_msg receive loop serves every session; the port a request
 *     arrives on (the MIG `server` argument) identifies the session.
 *   - a session can watch explicit store keys (SCDynamicStoreAddWatchedKey)
 *     and register one notification port (SCDynamicStoreNotifyMachPort).
 *   - a session can also watch a POSIX regular expression (iter 5):
 *     any store key matching the pattern triggers the same fan-out.
 *   - when a watched key changes, the key is appended to the watching
 *     session's pending-changes list and — on the 0->1 edge — an empty
 *     Mach message is sent to its notification port; the client then
 *     drains the list with notifychanges.
 *
 * Everything here runs on configd's single mach_msg receive thread, so
 * no locking is needed.
 */

#ifndef _CONFIG_SESSION_H
#define _CONFIG_SESSION_H

#include <sys/types.h>
#include <mach/mach.h>

/*
 * Mach msgh_id of the change notification configd sends to a session's
 * notification port. The payload is a bare header — it only wakes the
 * client, which then calls notifychanges to learn which keys changed
 * (Apple's SCDynamicStore notification port works the same way: the
 * message id carries no information).
 */
#define CONFIGD_NOTIFY_MSGID	0x434e4654	/* 'CNFT' */

/*
 * session_layer_init — record the port set every per-session port is
 * added to. configd creates the set; session_open() moves new session
 * ports into it so the main receive loop serves them.
 */
void session_layer_init(mach_port_t port_set);

/*
 * session_open — create a session on a fresh per-session port. On
 * success *port holds a new receive right (already a member of the
 * configd port set, with a no-senders notification armed) and a
 * MACH_MSG_TYPE_MAKE_SEND right has been inserted for the caller to
 * hand to the client. Returns 0, or -1 on failure (nothing leaks).
 */
int session_open(mach_port_t *port);

/*
 * session_close — tear the session bound to `port` down: drop its
 * watches, deallocate its notification port, and destroy the session
 * port (which also removes it from the port set). Called when the
 * no-senders notification fires. Returns 0 if a session was closed,
 * -1 if `port` named no session.
 */
int session_close(mach_port_t port);

/*
 * session_valid — 1 if `port` names a live session, else 0. The notify
 * routines require an open session; the data routines act on the
 * global store and do not.
 */
int session_valid(mach_port_t port);

/*
 * session_set_notify_port — register `notify` (a send right configd
 * takes ownership of) as the session's change-notification port. Any
 * previously registered port is deallocated. Returns a kSCStatus code.
 */
int session_set_notify_port(mach_port_t port, mach_port_t notify);

/*
 * session_clear_notify_port — drop the session's notification port
 * (SCDynamicStoreNotifyCancel). Returns a kSCStatus code.
 */
int session_clear_notify_port(mach_port_t port);

/*
 * session_watch_add / session_watch_remove — add or drop an explicit
 * watched key for the session. Adding a key already watched is a
 * no-op success. Returns a kSCStatus code.
 */
int session_watch_add(mach_port_t port, const void *key, size_t klen);
int session_watch_remove(mach_port_t port, const void *key, size_t klen);

/*
 * session_pattern_add / session_pattern_remove — add or drop a regex
 * watch for the session. The pattern bytes are a POSIX extended regular
 * expression; configd anchors it (^...$) and compiles it. Any store key
 * matching it triggers a change notification, exactly like an explicit
 * watch. Adding a pattern already watched is a no-op success; an
 * uncompilable pattern returns kSCStatusInvalidArgument. Returns a
 * kSCStatus code.
 */
int session_pattern_add(mach_port_t port, const void *pat, size_t plen);
int session_pattern_remove(mach_port_t port, const void *pat, size_t plen);

/*
 * session_drain_changes — copy the session's pending changed-key list
 * into `buf` (capacity `cap`) and clear it. The list is encoded as a
 * run of records, each a uint32 little-endian key length followed by
 * that many key bytes. Returns the number of bytes written (0 if no
 * keys are pending), or -1 if the encoding would not fit in `cap`.
 */
ssize_t session_drain_changes(mach_port_t port, void *buf, size_t cap);

/*
 * session_key_changed — a store key changed (configset / configremove /
 * confignotify). Every session watching `key` records it as pending
 * and, if it has a notification port and had no pending changes, is
 * sent a change notification message.
 */
void session_key_changed(const void *key, size_t klen);

#endif /* !_CONFIG_SESSION_H */
