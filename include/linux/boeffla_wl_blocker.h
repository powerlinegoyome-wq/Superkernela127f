#ifndef _LINUX_BOEFFLA_WL_BLOCKER_H
#define _LINUX_BOEFFLA_WL_BLOCKER_H

#include <linux/types.h>

#ifdef CONFIG_BOEFFLA_WL_BLOCKER
extern bool boeffla_wl_blocker_active(const char *name);
#else
static inline bool boeffla_wl_blocker_active(const char *name)
{
	return false;
}
#endif

#endif /* _LINUX_BOEFFLA_WL_BLOCKER_H */
