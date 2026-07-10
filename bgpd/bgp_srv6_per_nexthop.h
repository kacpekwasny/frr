// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * BGP SRv6 per-nexthop (per-CE) SID cache
 * Copyright (C) 2026 FRRouting Contributors
 */

#ifndef _BGP_SRV6_PER_NEXTHOP_H_
#define _BGP_SRV6_PER_NEXTHOP_H_

#include "srv6.h"

struct bgp_srv6_per_nexthop_cache;
PREDECL_RBTREE_UNIQ(bgp_srv6_per_nexthop_cache);

extern int
bgp_srv6_per_nexthop_cache_cmp(const struct bgp_srv6_per_nexthop_cache *a,
				const struct bgp_srv6_per_nexthop_cache *b);

/*
 * Per-CE SRv6 SID cache entry.
 * One entry per (VRF, BGP nexthop) tuple, holding an End.DX4 or End.DX6 SID.
 */
struct bgp_srv6_per_nexthop_cache {

	/* RB-tree entry */
	struct bgp_srv6_per_nexthop_cache_item entry;

	/* The nexthop is the key of the cache */
	struct prefix nexthop;

	/* Allocated SRv6 SID */
	struct in6_addr sid;
	bool sid_valid;

	/* SID transpose label for TLV encoding */
	mpls_label_t sid_transpose_label;

	/* Locator this SID was allocated from */
	struct srv6_locator *locator;

	/* Endpoint behavior (DX4/DX6, classic or uSID) */
	uint16_t endpoint_behavior;

	/* Number of path_infos referencing this entry */
	unsigned int path_count;

	/* Back pointer to bgp instance (VRF) */
	struct bgp *bgp;

	/* Copy of the nexthop resolution (for ifindex / adjacency) */
	struct nexthop *nh;

	/* List of bgp_path_infos using this cache entry */
	LIST_HEAD(srv6_pnc_paths, bgp_path_info) paths;

	time_t last_update;

	/* Back pointer to the cache tree this entry belongs to */
	struct bgp_srv6_per_nexthop_cache_head *tree;

	/* Last SID sent to zebra for local install  */
	struct in6_addr *zebra_sid_last_sent;

	/* Whether we have wide function for this SID */
	bool sid_func_wide;
};

DECLARE_RBTREE_UNIQ(bgp_srv6_per_nexthop_cache,
		    struct bgp_srv6_per_nexthop_cache, entry,
		    bgp_srv6_per_nexthop_cache_cmp);

extern struct bgp_srv6_per_nexthop_cache *
bgp_srv6_per_nexthop_new(struct bgp_srv6_per_nexthop_cache_head *tree,
			 struct prefix *nexthop);

extern struct bgp_srv6_per_nexthop_cache *
bgp_srv6_per_nexthop_find(struct bgp_srv6_per_nexthop_cache_head *tree,
			  struct prefix *nexthop);

extern void bgp_srv6_per_nexthop_free(struct bgp_srv6_per_nexthop_cache *bsnc);

extern void bgp_srv6_per_nexthop_path_unlink(struct bgp_path_info *pi);

extern void bgp_srv6_per_nexthop_localsid(struct bgp_srv6_per_nexthop_cache *bsnc,
					  bool install);

extern void bgp_srv6_per_nexthop_init(void);

#endif /* _BGP_SRV6_PER_NEXTHOP_H_ */
