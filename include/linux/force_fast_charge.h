#ifndef _LINUX_FORCE_FAST_CHARGE_H
#define _LINUX_FORCE_FAST_CHARGE_H

#include <linux/types.h>

#ifdef CONFIG_FORCE_FAST_CHARGE
extern bool force_fast_charge;
#else
#define force_fast_charge false
#endif

#endif /* _LINUX_FORCE_FAST_CHARGE_H */
