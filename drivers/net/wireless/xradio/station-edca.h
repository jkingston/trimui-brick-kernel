/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef XRS_EDCA_H
#define XRS_EDCA_H
#include "xr829-wire.h"
/* mac80211 TXOP is in 32-us units; WSM uses a 16-bit microsecond field.
 * Validate before conversion so invalid AP parameters cannot wrap silently.
 */
static inline int xrs_edca_validate(unsigned int link,unsigned int ac,
 unsigned int aifs,unsigned int cw_min,unsigned int cw_max,
 unsigned int txop,unsigned int uapsd)
{
 if(link || ac>=4 || uapsd || !aifs || aifs>15 ||
    cw_min>cw_max || cw_max>65535 ||
    (cw_min&(cw_min+1)) || (cw_max&(cw_max+1)) || txop>65535/32)
  return -EOPNOTSUPP;
 return 0;
}
#endif
