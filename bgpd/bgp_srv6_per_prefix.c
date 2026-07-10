// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * BGP SRv6 per-prefix explicit SID configuration
 * Copyright (C) 2026 Dawid Kopec <dkopec@akamai.com>
 *
 * Stores a user-specified End.DX4/DX6 SID for an individual route prefix so
 * that the VPN export path can advertise the route with that exact SID rather
 * than choosing one via the auto per-CE or per-VRF allocators.
 *
 * Lifecycle
 * ---------
 *  configure:  bgp_srv6_per_prefix_sid_new()   + fill fields
 *  export:     bgp_srv6_per_prefix_sid_lookup() in vpn_leak_from_vrf_*
 *  cleanup:    bgp_srv6_per_prefix_sid_free()  /
 *              bgp_srv6_per_prefix_sids_cleanup()
 */

#include <zebra.h>

#include "prefix.h"
#include "srv6.h"
#include "if.h"
#include "vrf.h"

#include "bgpd/bgpd.h"
#include "bgpd/bgp_debug.h"
#include "bgpd/bgp_memory.h"
#include "bgpd/bgp_zebra.h"
#include "bgpd/bgp_mplsvpn.h"
#include "bgpd/bgp_srv6_per_prefix.h"

DEFINE_MTYPE_STATIC(BGPD, BGP_SRV6_PER_PREFIX_SID,
		    "BGP SRv6 per-prefix explicit SID");

/* ------------------------------------------------------------------ */
/* RB-tree                                                             */
/* ------------------------------------------------------------------ */

int bgp_srv6_per_prefix_sid_cmp(const struct bgp_srv6_per_prefix_sid *a,
				 const struct bgp_srv6_per_prefix_sid *b)
{
	return prefix_cmp(&a->prefix, &b->prefix);
}

/* ------------------------------------------------------------------ */
/* Entry management                                                    */
/* ------------------------------------------------------------------ */

struct bgp_srv6_per_prefix_sid *
bgp_srv6_per_prefix_sid_new(struct bgp_srv6_per_prefix_sid_head *tree,
			    const struct prefix *p)
{
	struct bgp_srv6_per_prefix_sid *entry;

	entry = XCALLOC(MTYPE_BGP_SRV6_PER_PREFIX_SID, sizeof(*entry));
	prefix_copy(&entry->prefix, p);
	bgp_srv6_per_prefix_sid_add(tree, entry);
	return entry;
}

struct bgp_srv6_per_prefix_sid *
bgp_srv6_per_prefix_sid_lookup(struct bgp_srv6_per_prefix_sid_head *tree,
				const struct prefix *p)
{
	struct bgp_srv6_per_prefix_sid key = {};

	prefix_copy(&key.prefix, p);
	return bgp_srv6_per_prefix_sid_find(tree, &key);
}

/* ------------------------------------------------------------------ */
/* Localsid install / withdraw                                         */
/* ------------------------------------------------------------------ */

void bgp_srv6_per_prefix_sid_localsid(struct bgp_srv6_per_prefix_sid *entry,
				       bool install)
{
	int debug = BGP_DEBUG(vpn, VPN_LEAK_LABEL);
	struct seg6local_context ctx = {};
	enum seg6local_action_t act;
	struct interface *ifp_out;
	struct in6_addr *sid_ls;

	/*
	 * For withdrawal we only need the SID prefix — zclient_send_localsid()
	 * with ZEBRA_ROUTE_DELETE ignores the interface, action, and context.
	 * Skip directly to the withdrawal logic if we have a previously-sent SID.
	 */
	if (!install)
		goto do_withdraw;

	/* Interface and nexthop are both required for DX behaviour */
	if (!entry->ifname[0] || (!entry->has_nh4 && !entry->has_nh6))
		return;

	if (!entry->locator)
		return;

	/* Look up the output interface – it may live in the VRF or the
	 * default VRF (for representor/rep ports). */
	ifp_out = if_lookup_by_name(entry->ifname, entry->bgp->vrf_id);
	if (!ifp_out)
		ifp_out = if_lookup_by_name(entry->ifname, VRF_DEFAULT);
	if (!ifp_out) {
		if (debug)
			zlog_debug("%s: interface %s not found for per-prefix SID %pI6",
				   __func__, entry->ifname, &entry->sid);
		return;
	}

	ctx.block_len = entry->locator->block_bits_length;
	ctx.node_len = entry->locator->node_bits_length;
	ctx.function_len = entry->locator->function_bits_length;
	ctx.argument_len = entry->locator->argument_bits_length;

	if (CHECK_FLAG(entry->locator->flags, SRV6_LOCATOR_USID))
		SET_SRV6_FLV_OP(ctx.flv.flv_ops, ZEBRA_SEG6_LOCAL_FLV_OP_NEXT_CSID);

	if (entry->has_nh4) {
		act = ZEBRA_SEG6_LOCAL_ACTION_END_DX4;
		ctx.nh4 = entry->nh4;
	} else {
		act = ZEBRA_SEG6_LOCAL_ACTION_END_DX6;
		ctx.nh6 = entry->nh6;
	}

	if (debug)
		zlog_debug("%s: installing seg6local %s SID %pI6 for prefix %pFX interface %s VRF %s",
			   __func__,
			   (act == ZEBRA_SEG6_LOCAL_ACTION_END_DX4) ? "End.DX4"
								    : "End.DX6",
			   &entry->sid, &entry->prefix, entry->ifname,
			   entry->bgp->name_pretty);

	zclient_send_localsid(bgp_zclient, ZEBRA_ROUTE_ADD, &entry->sid,
			      IPV6_MAX_BITLEN, ifp_out->ifindex, act, &ctx);

	sid_ls = XCALLOC(MTYPE_BGP_SRV6_SID, sizeof(struct in6_addr));
	*sid_ls = entry->sid;
	XFREE(MTYPE_BGP_SRV6_SID, entry->zebra_sid_last_sent);
	entry->zebra_sid_last_sent = sid_ls;
	return;

do_withdraw:
	if (entry->zebra_sid_last_sent) {
		if (debug)
			zlog_debug("%s: withdrawing seg6local SID %pI6 for prefix %pFX VRF %s",
				   __func__,
				   entry->zebra_sid_last_sent, &entry->prefix,
				   entry->bgp ? entry->bgp->name_pretty : "(null)");

		zclient_send_localsid(bgp_zclient, ZEBRA_ROUTE_DELETE,
				      entry->zebra_sid_last_sent,
				      IPV6_MAX_BITLEN, 0,
				      ZEBRA_SEG6_LOCAL_ACTION_UNSPEC, &ctx);
		XFREE(MTYPE_BGP_SRV6_SID, entry->zebra_sid_last_sent);
		entry->zebra_sid_last_sent = NULL;
	}
}

void bgp_srv6_per_prefix_sid_free(struct bgp_srv6_per_prefix_sid *entry)
{
	bgp_srv6_per_prefix_sid_localsid(entry, false);
	srv6_locator_free(entry->locator);
	XFREE(MTYPE_BGP_SRV6_PER_PREFIX_SID, entry);
}

void bgp_srv6_per_prefix_sids_cleanup(struct bgp *bgp, afi_t afi)
{
	struct bgp_srv6_per_prefix_sid *entry;

	while ((entry = bgp_srv6_per_prefix_sid_pop(&bgp->srv6_sids_per_prefix[afi])))
		bgp_srv6_per_prefix_sid_free(entry);
}
