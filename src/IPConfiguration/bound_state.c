/*
 * bound_state.c — single global, mutex-guarded.
 *
 * iter 5a tracks one binding (em0 in CI). iter 5b widens to a small
 * array of per-interface entries when the threading rework lands.
 */
#include "bound_state.h"

#include <pthread.h>
#include <string.h>

struct {
	pthread_mutex_t		lock;
	bool			bound;
	char			ifname[IF_NAMESIZE];
	struct dhcp_lease	lease;
} g_bound = { PTHREAD_MUTEX_INITIALIZER, false, "", { {0}, {0}, {0}, {0}, 0, {{0}}, 0 } };

void
bound_state_init(void)
{
	/* Static initializer above suffices; the function exists so
	 * main() has a clear init-time hook for iter 5b's table init. */
}

void
bound_state_set(const char *ifname, const struct dhcp_lease *lease)
{
	(void)pthread_mutex_lock(&g_bound.lock);
	if (ifname == NULL || lease == NULL) {
		g_bound.bound = false;
		g_bound.ifname[0] = '\0';
		(void)memset(&g_bound.lease, 0, sizeof(g_bound.lease));
	} else {
		g_bound.bound = true;
		(void)strlcpy(g_bound.ifname, ifname,
		    sizeof(g_bound.ifname));
		g_bound.lease = *lease;
	}
	(void)pthread_mutex_unlock(&g_bound.lock);
}

bool
bound_state_any(char *name_out, size_t name_out_sz)
{
	bool any;

	(void)pthread_mutex_lock(&g_bound.lock);
	any = g_bound.bound;
	if (any && name_out != NULL && name_out_sz > 0)
		(void)strlcpy(name_out, g_bound.ifname, name_out_sz);
	(void)pthread_mutex_unlock(&g_bound.lock);
	return (any);
}

int
bound_state_count(void)
{
	int n;

	(void)pthread_mutex_lock(&g_bound.lock);
	n = g_bound.bound ? 1 : 0;
	(void)pthread_mutex_unlock(&g_bound.lock);
	return (n);
}

bool
bound_state_get_addr(const char *ifname, uint32_t *addr_out)
{
	bool hit = false;

	if (ifname == NULL || addr_out == NULL)
		return (false);
	(void)pthread_mutex_lock(&g_bound.lock);
	if (g_bound.bound && strcmp(ifname, g_bound.ifname) == 0) {
		*addr_out = g_bound.lease.addr.s_addr;
		hit = true;
	}
	(void)pthread_mutex_unlock(&g_bound.lock);
	return (hit);
}
