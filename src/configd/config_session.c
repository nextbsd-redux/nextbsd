/*
 * config_session.c — configd's per-session state, key watches and
 * change-notification fan-out (configd iter 4). See config_session.h
 * for the model.
 *
 * The session table is a small dynamically grown array scanned
 * linearly by port: a live store has a handful of clients, and every
 * access is on configd's single mach_msg receive thread, so neither a
 * hash table nor locking is warranted.
 */

#include "config_session.h"
#include "config_types.h"		/* kSCStatus*, CONFIG_DATA_MAX */

#include <mach/notify.h>		/* MACH_NOTIFY_NO_SENDERS */

#include <regex.h>			/* regcomp / regexec — pattern watches */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* One key — a watch-list entry or a pending changed key. */
struct keyref {
	void	*key;
	size_t	 klen;
};

/*
 * One regex watch. `src` keeps the client's original (un-anchored)
 * pattern bytes so session_pattern_remove can match by value; `preg`
 * is the compiled, anchored expression. preg is heap-allocated so the
 * pattern array can be realloc'd without relocating compiled state.
 */
struct pattern {
	void	*src;
	size_t	 slen;
	regex_t	*preg;
};

struct session {
	int		in_use;
	mach_port_t	port;		/* per-session receive right */
	mach_port_t	notify_port;	/* client notification port, or NULL */
	struct keyref	*watch;		/* explicit watched keys */
	size_t		n_watch;
	size_t		watch_cap;
	struct pattern	*patterns;	/* regex watches */
	size_t		n_patterns;
	size_t		patterns_cap;
	struct keyref	*changed;	/* keys changed since the last drain */
	size_t		n_changed;
	size_t		changed_cap;
};

static struct session	*sessions;
static size_t		n_sessions;
static size_t		sessions_cap;

/* The port set every per-session port joins — recorded by configd. */
static mach_port_t	config_pset = MACH_PORT_NULL;

static int
keyref_eq(const struct keyref *e, const void *key, size_t klen)
{
	return e->klen == klen && memcmp(e->key, key, klen) == 0;
}

/*
 * keylist_add — append a copy of `key` to a keyref list, growing it.
 * Already-present keys are skipped (the list is a set). Returns 0 on
 * success, -1 on an allocation failure.
 */
static int
keylist_add(struct keyref **list, size_t *n, size_t *cap,
    const void *key, size_t klen)
{
	struct keyref	*entry;
	void		*kcopy;
	size_t		i;

	for (i = 0; i < *n; i++) {
		if (keyref_eq(&(*list)[i], key, klen))
			return 0;	/* already present */
	}

	if (*n == *cap) {
		size_t		ncap = (*cap != 0) ? *cap * 2 : 8;
		struct keyref	*nl = realloc(*list, ncap * sizeof(*nl));

		if (nl == NULL)
			return -1;
		*list = nl;
		*cap = ncap;
	}

	kcopy = malloc(klen != 0 ? klen : 1);
	if (kcopy == NULL)
		return -1;
	if (klen != 0)
		memcpy(kcopy, key, klen);

	entry = &(*list)[(*n)++];
	entry->key = kcopy;
	entry->klen = klen;
	return 0;
}

/* keylist_remove — drop `key`; returns 0 if removed, -1 if absent. */
static int
keylist_remove(struct keyref *list, size_t *n, const void *key, size_t klen)
{
	size_t i;

	for (i = 0; i < *n; i++) {
		if (keyref_eq(&list[i], key, klen)) {
			free(list[i].key);
			list[i] = list[--(*n)];	/* compact */
			return 0;
		}
	}
	return -1;
}

/* keylist_free — free every key and the backing array. */
static void
keylist_free(struct keyref *list, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++)
		free(list[i].key);
	free(list);
}

/* patternlist_free — free every regex watch and the backing array. */
static void
patternlist_free(struct pattern *list, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++) {
		regfree(list[i].preg);
		free(list[i].preg);
		free(list[i].src);
	}
	free(list);
}

static struct session *
session_find(mach_port_t port)
{
	size_t i;

	if (port == MACH_PORT_NULL)
		return NULL;
	for (i = 0; i < n_sessions; i++) {
		if (sessions[i].in_use && sessions[i].port == port)
			return &sessions[i];
	}
	return NULL;
}

/*
 * send_notification — wake a session's client with a bare-header Mach
 * message. Non-blocking (MACH_SEND_TIMEOUT 0): a dead or backed-up
 * client must never stall configd. A lost notification is harmless —
 * the change stays queued and the client still drains it via the next
 * notification or notifychanges poll.
 */
static void
send_notification(mach_port_t notify_port)
{
	mach_msg_header_t msg;

	memset(&msg, 0, sizeof(msg));
	msg.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	msg.msgh_size = sizeof(msg);
	msg.msgh_remote_port = notify_port;
	msg.msgh_local_port = MACH_PORT_NULL;
	msg.msgh_id = CONFIGD_NOTIFY_MSGID;

	(void)mach_msg(&msg, MACH_SEND_MSG | MACH_SEND_TIMEOUT,
	    sizeof(msg), 0, MACH_PORT_NULL, 0, MACH_PORT_NULL);
}

void
session_layer_init(mach_port_t port_set)
{
	config_pset = port_set;
}

int
session_open(mach_port_t *port)
{
	mach_port_t	sport = MACH_PORT_NULL;
	mach_port_t	prev = MACH_PORT_NULL;
	struct session	*s = NULL;
	size_t		i;

	*port = MACH_PORT_NULL;

	if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE,
	    &sport) != KERN_SUCCESS)
		return -1;

	/* A MAKE_SEND right for the client; the configopen reply moves
	 * it out. While it exists ip_srights is 1, so the no-senders
	 * notification armed below only registers (it fires later). */
	if (mach_port_insert_right(mach_task_self(), sport, sport,
	    MACH_MSG_TYPE_MAKE_SEND) != KERN_SUCCESS)
		goto fail;

	/* Serve the session port from configd's single receive loop. */
	if (mach_port_move_member(mach_task_self(), sport, config_pset)
	    != KERN_SUCCESS)
		goto fail;

	/*
	 * Arm a no-senders notification. When the client's last send
	 * right to the session port dies — it exits, or the configopen
	 * reply fails to send — the kernel posts MACH_NOTIFY_NO_SENDERS
	 * to the port and configd_serve() calls session_close().
	 */
	(void)mach_port_request_notification(mach_task_self(), sport,
	    MACH_NOTIFY_NO_SENDERS, 1, sport, MACH_MSG_TYPE_MAKE_SEND_ONCE,
	    &prev);
	if (prev != MACH_PORT_NULL)
		(void)mach_port_deallocate(mach_task_self(), prev);

	/* Take a free table slot, or grow the table. */
	for (i = 0; i < n_sessions; i++) {
		if (!sessions[i].in_use) {
			s = &sessions[i];
			break;
		}
	}
	if (s == NULL) {
		if (n_sessions == sessions_cap) {
			size_t		ncap = (sessions_cap != 0)
			    ? sessions_cap * 2 : 16;
			struct session	*ns = realloc(sessions,
			    ncap * sizeof(*ns));

			if (ns == NULL)
				goto fail;
			sessions = ns;
			sessions_cap = ncap;
		}
		s = &sessions[n_sessions++];
	}

	memset(s, 0, sizeof(*s));
	s->in_use = 1;
	s->port = sport;
	*port = sport;
	return 0;

fail:
	/*
	 * Drop the send right (if insert_right got that far — harmless
	 * if not), then the receive right; destroying the receive right
	 * removes the port from the set and tears down any armed
	 * notification.
	 */
	(void)mach_port_deallocate(mach_task_self(), sport);
	(void)mach_port_mod_refs(mach_task_self(), sport,
	    MACH_PORT_RIGHT_RECEIVE, -1);
	return -1;
}

int
session_close(mach_port_t port)
{
	struct session *s = session_find(port);

	if (s == NULL)
		return -1;

	keylist_free(s->watch, s->n_watch);
	keylist_free(s->changed, s->n_changed);
	patternlist_free(s->patterns, s->n_patterns);
	if (s->notify_port != MACH_PORT_NULL)
		(void)mach_port_deallocate(mach_task_self(), s->notify_port);
	/*
	 * The client's send right is already gone (no-senders fired);
	 * dropping configd's receive right destroys the port and
	 * removes it from the configd port set.
	 */
	(void)mach_port_mod_refs(mach_task_self(), s->port,
	    MACH_PORT_RIGHT_RECEIVE, -1);
	memset(s, 0, sizeof(*s));
	return 0;
}

int
session_valid(mach_port_t port)
{
	return session_find(port) != NULL;
}

int
session_set_notify_port(mach_port_t port, mach_port_t notify)
{
	struct session *s = session_find(port);

	if (s == NULL) {
		if (notify != MACH_PORT_NULL)
			(void)mach_port_deallocate(mach_task_self(), notify);
		return kSCStatusNoStoreSession;
	}
	if (notify == MACH_PORT_NULL)
		return kSCStatusInvalidArgument;

	/* Replace any previously registered port. */
	if (s->notify_port != MACH_PORT_NULL)
		(void)mach_port_deallocate(mach_task_self(), s->notify_port);
	s->notify_port = notify;
	return kSCStatusOK;
}

int
session_clear_notify_port(mach_port_t port)
{
	struct session *s = session_find(port);

	if (s == NULL)
		return kSCStatusNoStoreSession;
	if (s->notify_port != MACH_PORT_NULL) {
		(void)mach_port_deallocate(mach_task_self(), s->notify_port);
		s->notify_port = MACH_PORT_NULL;
	}
	return kSCStatusOK;
}

int
session_watch_add(mach_port_t port, const void *key, size_t klen)
{
	struct session *s = session_find(port);

	if (s == NULL)
		return kSCStatusNoStoreSession;
	if (keylist_add(&s->watch, &s->n_watch, &s->watch_cap, key, klen) != 0)
		return kSCStatusFailed;
	return kSCStatusOK;
}

int
session_watch_remove(mach_port_t port, const void *key, size_t klen)
{
	struct session *s = session_find(port);

	if (s == NULL)
		return kSCStatusNoStoreSession;
	if (keylist_remove(s->watch, &s->n_watch, key, klen) != 0)
		return kSCStatusNoKey;
	return kSCStatusOK;
}

int
session_pattern_add(mach_port_t port, const void *pat, size_t plen)
{
	struct session	*s = session_find(port);
	const char	*p = pat;
	struct pattern	*entry;
	regex_t		*preg;
	void		*scopy;
	char		buf[CONFIG_DATA_MAX + 3];	/* ^ + pattern + $ + NUL */
	size_t		off = 0;
	size_t		i;

	if (s == NULL)
		return kSCStatusNoStoreSession;
	if (plen > CONFIG_DATA_MAX)
		return kSCStatusInvalidArgument;

	/* Already watching this exact pattern — idempotent success. */
	for (i = 0; i < s->n_patterns; i++) {
		if (s->patterns[i].slen == plen &&
		    memcmp(s->patterns[i].src, pat, plen) == 0)
			return kSCStatusOK;
	}

	/*
	 * Anchor the expression to a full-key match: prepend '^' unless
	 * it is already there, append '$' unless the pattern already
	 * ends in an unescaped '$' (mirrors Apple's pattern.c).
	 */
	if (plen == 0 || p[0] != '^')
		buf[off++] = '^';
	memcpy(buf + off, p, plen);
	off += plen;
	if (plen == 0 || p[plen - 1] != '$' ||
	    (plen >= 2 && p[plen - 2] == '\\'))
		buf[off++] = '$';
	buf[off] = '\0';

	if (s->n_patterns == s->patterns_cap) {
		size_t		ncap = (s->patterns_cap != 0)
		    ? s->patterns_cap * 2 : 4;
		struct pattern	*np = realloc(s->patterns,
		    ncap * sizeof(*np));

		if (np == NULL)
			return kSCStatusFailed;
		s->patterns = np;
		s->patterns_cap = ncap;
	}

	preg = malloc(sizeof(*preg));
	if (preg == NULL)
		return kSCStatusFailed;
	if (regcomp(preg, buf, REG_EXTENDED) != 0) {
		free(preg);
		return kSCStatusInvalidArgument;	/* uncompilable regex */
	}
	scopy = malloc(plen != 0 ? plen : 1);
	if (scopy == NULL) {
		regfree(preg);
		free(preg);
		return kSCStatusFailed;
	}
	if (plen != 0)
		memcpy(scopy, pat, plen);

	entry = &s->patterns[s->n_patterns++];
	entry->src = scopy;
	entry->slen = plen;
	entry->preg = preg;
	return kSCStatusOK;
}

int
session_pattern_remove(mach_port_t port, const void *pat, size_t plen)
{
	struct session	*s = session_find(port);
	size_t		i;

	if (s == NULL)
		return kSCStatusNoStoreSession;
	for (i = 0; i < s->n_patterns; i++) {
		if (s->patterns[i].slen == plen &&
		    memcmp(s->patterns[i].src, pat, plen) == 0) {
			regfree(s->patterns[i].preg);
			free(s->patterns[i].preg);
			free(s->patterns[i].src);
			s->patterns[i] = s->patterns[--s->n_patterns];
			return kSCStatusOK;
		}
	}
	return kSCStatusNoKey;
}

ssize_t
session_drain_changes(mach_port_t port, void *buf, size_t cap)
{
	struct session	*s = session_find(port);
	uint8_t		*p = buf;
	size_t		i;
	size_t		off = 0;

	if (s == NULL)
		return -1;

	for (i = 0; i < s->n_changed; i++) {
		size_t		klen = s->changed[i].klen;
		uint32_t	l32 = (uint32_t)klen;

		if (off + sizeof(l32) + klen > cap)
			return -1;	/* would not fit the reply array */
		memcpy(p + off, &l32, sizeof(l32));
		off += sizeof(l32);
		memcpy(p + off, s->changed[i].key, klen);
		off += klen;
	}

	/* The client has now seen these — clear the pending list. */
	keylist_free(s->changed, s->n_changed);
	s->changed = NULL;
	s->n_changed = 0;
	s->changed_cap = 0;
	return (ssize_t)off;
}

void
session_key_changed(const void *key, size_t klen)
{
	char	keystr[CONFIG_DATA_MAX + 1];	/* NUL-terminated for regexec */
	int	have_keystr = 0;
	size_t	i, w;

	for (i = 0; i < n_sessions; i++) {
		struct session	*s = &sessions[i];
		int		interested = 0;
		int		was_empty;

		if (!s->in_use)
			continue;

		/* explicit-key watch? */
		for (w = 0; w < s->n_watch; w++) {
			if (keyref_eq(&s->watch[w], key, klen)) {
				interested = 1;
				break;
			}
		}

		/*
		 * regex watch? Build the NUL-terminated key string once,
		 * lazily — only if some session has a pattern to test.
		 */
		if (!interested && s->n_patterns != 0 &&
		    klen < sizeof(keystr)) {
			if (!have_keystr) {
				memcpy(keystr, key, klen);
				keystr[klen] = '\0';
				have_keystr = 1;
			}
			for (w = 0; w < s->n_patterns; w++) {
				if (regexec(s->patterns[w].preg, keystr,
				    0, NULL, 0) == 0) {
					interested = 1;
					break;
				}
			}
		}

		if (!interested)
			continue;

		/*
		 * Record the change. A notification is sent only on the
		 * empty -> non-empty edge: further changes before the
		 * client drains coalesce under the one notification.
		 */
		was_empty = (s->n_changed == 0);
		if (keylist_add(&s->changed, &s->n_changed, &s->changed_cap,
		    key, klen) != 0)
			continue;	/* allocation failure — drop */
		if (was_empty && s->n_changed != 0 &&
		    s->notify_port != MACH_PORT_NULL)
			send_notification(s->notify_port);
	}
}
