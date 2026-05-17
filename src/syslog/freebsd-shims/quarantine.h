/* quarantine.h — FreeBSD shim. Apple's TrustedBSD quarantine API
 * (qtn_proc_*, qtn_file_*). Stubs return success — quarantine is
 * a macOS app-launch metadata feature with no FreeBSD analogue. */
#ifndef _FREEBSD_SHIM_QUARANTINE_H_
#define _FREEBSD_SHIM_QUARANTINE_H_

#include <stddef.h>

typedef void * qtn_proc_t;
typedef void * qtn_file_t;

#define QTN_NOT_QUARANTINED		0
#define QTN_FLAG_DOWNLOAD		0x0001
#define QTN_FLAG_SANDBOX		0x0002
#define QTN_FLAG_HARD_QUARANTINE	0x0004
#define QTN_FLAG_HARD			QTN_FLAG_HARD_QUARANTINE

static inline qtn_proc_t qtn_proc_alloc(void) { return NULL; }
static inline void qtn_proc_free(qtn_proc_t qp) { (void)qp; }
static inline int qtn_proc_apply_to_self(qtn_proc_t qp) { (void)qp; return 0; }
static inline int qtn_proc_init(qtn_proc_t qp) { (void)qp; return 0; }
static inline int qtn_proc_init_with_self(qtn_proc_t qp) { (void)qp; return 0; }
static inline int qtn_proc_init_with_data(qtn_proc_t qp, const void *d, size_t s) {
	(void)qp; (void)d; (void)s; return 0;
}

#endif
