// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * BGP SRv6 per-prefix explicit SID configuration
 * Copyright (C) 2026 Dawid Kopec <dkopec@akamai.com>
 */

#ifndef _BGP_SRV6_PER_PREFIX_H_
#define _BGP_SRV6_PER_PREFIX_H_

#include "prefix.h"
#include "srv6.h"

struct bgp;

struct bgp_srv6_per_prefix_sid;
PREDECL_RBTREE_UNIQ(bgp_srv6_per_prefix_sid);

extern int bgp_srv6_per_prefix_sid_cmp(const struct bgp_srv6_per_prefix_sid *a,
					const struct bgp_srv6_per_prefix_sid *b);

/*
 * Per-prefix explicit SRv6 SID entry.
 *
 * Maps a specific VRF route prefix to a user-configured End.DX4/DX6 SID so
 * that BGP can advertise the route with that exact SID and (optionally)
 * install the corresponding seg6local localsid in the kernel.
 */
struct bgp_srv6_per_prefix_sid {
	/* RB-tree linkage (keyed by prefix) */
	struct bgp_srv6_per_prefix_sid_item entry;

	/* Route prefix (the lookup key) */
	struct prefix prefix;

	/* User-specified SID value */
	struct in6_addr sid;

	/* Output interface for the End.DX behaviour (may be empty) */
        char ifname[IFNAMSIZ];
	/* IPv4 nexthop for End.DX4 */
	struct in_addr nh4;
	bool has_nh4;

	/* IPv6 nexthop for End.DX6 */
	struct in6_addr nh6;
	bool has_nh6;

	/*
	 * Locator resolved lazily at first export time.
	 * Used for the SRv6 Structure Sub-Sub TLV field lengths.
	 */
	struct srv6_locator *locator;

	/* Last SID sent to zebra for localsid install tracking */
	struct in6_addr *zebra_sid_last_sent;

	/* Back-pointer to the owning BGP VRF instance */
	struct bgp *bgp;
};

DECLARE_RBTREE_UNIQ(bgp_srv6_per_prefix_sid, struct bgp_srv6_per_prefix_sid,
		    entry, bgp_srv6_per_prefix_sid_cmp);

extern struct bgp_srv6_per_prefix_sid *
bgp_srv6_per_prefix_sid_new(struct bgp_srv6_per_prefix_sid_head *tree,
			    const struct prefix *p);

extern struct bgp_srv6_per_prefix_sid *
bgp_srv6_per_prefix_sid_lookup(struct bgp_srv6_per_prefix_sid_head *tree,
				const struct prefix *p);

extern void
bgp_srv6_per_prefix_sid_localsid(struct bgp_srv6_per_prefix_sid *entry,
				  bool install);

extern void bgp_srv6_per_prefix_sid_free(struct bgp_srv6_per_prefix_sid *entry);

/* Free all entries for afi in a BGP VRF instance (called on VRF cleanup). */
extern void bgp_srv6_per_prefix_sids_cleanup(struct bgp *bgp, afi_t afi);

#endif /* _BGP_SRV6_PER_PREFIX_H_ */
