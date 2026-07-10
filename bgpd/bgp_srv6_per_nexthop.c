// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * BGP SRv6 per-nexthop (per-CE) SID cache
 * Copyright (C) 2026 FRRouting Contributors
 *
 * Implements a refcounted cache of SRv6 SIDs allocated per (VRF, BGP nexthop)
 * tuple for End.DX4 / End.DX6 behavior.  Modeled on the MPLS per-nexthop label
 * cache in bgp_labelpool.c.
 */

#include <zebra.h>

#include "command.h"
#include "log.h"
#include "memory.h"
#include "nexthop.h"
#include "prefix.h"
#include "srv6.h"
#include "vrf.h"
#include "vty.h"
#include "zclient.h"

#include "bgpd/bgp_debug.h"
#include "bgpd/bgp_mplsvpn.h"
#include "bgpd/bgp_rd.h"
#include "bgpd/bgp_route.h"
#include "bgpd/bgp_srv6_per_nexthop.h"
#include "bgpd/bgp_table.h"
#include "bgpd/bgp_zebra.h"
#include "bgpd/bgpd.h"

extern struct zclient *bgp_zclient;

DEFINE_MTYPE_STATIC(BGPD, BGP_SRV6_PER_NEXTHOP_CACHE,
		    "BGP SRv6 per-nexthop SID cache");

int bgp_srv6_per_nexthop_cache_cmp(const struct bgp_srv6_per_nexthop_cache *a,
				   const struct bgp_srv6_per_nexthop_cache *b)
{
	return prefix_cmp(&a->nexthop, &b->nexthop);
}

struct bgp_srv6_per_nexthop_cache *
bgp_srv6_per_nexthop_new(struct bgp_srv6_per_nexthop_cache_head *tree,
			 struct prefix *nexthop)
{
	struct bgp_srv6_per_nexthop_cache *bsnc;

	bsnc = XCALLOC(MTYPE_BGP_SRV6_PER_NEXTHOP_CACHE,
		       sizeof(struct bgp_srv6_per_nexthop_cache));
	bsnc->tree = tree;
	bsnc->sid_valid = false;
	bsnc->sid_transpose_label = MPLS_INVALID_LABEL;
	prefix_copy(&bsnc->nexthop, nexthop);
	LIST_INIT(&(bsnc->paths));
	bgp_srv6_per_nexthop_cache_add(tree, bsnc);

	return bsnc;
}

struct bgp_srv6_per_nexthop_cache *
bgp_srv6_per_nexthop_find(struct bgp_srv6_per_nexthop_cache_head *tree,
			  struct prefix *nexthop)
{
	struct bgp_srv6_per_nexthop_cache bsnc = {};

	if (!tree)
		return NULL;

	memcpy(&bsnc.nexthop, nexthop, sizeof(struct prefix));
	return bgp_srv6_per_nexthop_cache_find(tree, &bsnc);
}

/*
 * Install or withdraw the local seg6local End.DX{4,6} route for this
 * per-CE SID.
 */
void bgp_srv6_per_nexthop_localsid(struct bgp_srv6_per_nexthop_cache *bsnc,
				   bool install)
{
	int debug = BGP_DEBUG(vpn, VPN_LEAK_LABEL);
	struct seg6local_context ctx = {};
	enum seg6local_action_t act;
	struct interface *ifp;
	struct in6_addr *sid_ls;

	if (!bsnc->sid_valid)
		return;

	if (!bsnc->locator)
		return;

	if (!bsnc->nh)
		return;

	ifp = if_get_vrf_loopback(bsnc->bgp->vrf_id);
	if (!ifp)
		return;

	ctx.block_len = bsnc->locator->block_bits_length;
	ctx.node_len = bsnc->locator->node_bits_length;
	if (bsnc->sid_func_wide)
		ctx.function_len = BGP_PREFIX_SID_SRV6_MAX_FUNCTION_LENGTH_FOR_BGP;
	else
		ctx.function_len = bsnc->locator->function_bits_length;
	ctx.argument_len = bsnc->locator->argument_bits_length;

	if (CHECK_FLAG(bsnc->locator->flags, SRV6_LOCATOR_USID))
		SET_SRV6_FLV_OP(ctx.flv.flv_ops, ZEBRA_SEG6_LOCAL_FLV_OP_NEXT_CSID);

	if (install) {
		/* Determine action and fill nexthop */
		if (bsnc->nexthop.family == AF_INET) {
			act = ZEBRA_SEG6_LOCAL_ACTION_END_DX4;
			ctx.nh4 = bsnc->nexthop.u.prefix4;
		} else {
			act = ZEBRA_SEG6_LOCAL_ACTION_END_DX6;
			ctx.nh6 = bsnc->nexthop.u.prefix6;
		}

		if (debug)
			zlog_debug("%s: installing seg6local %s SID %pI6 for nexthop %pFX VRF %s",
				   __func__,
				   (act == ZEBRA_SEG6_LOCAL_ACTION_END_DX4) ? "End.DX4" : "End.DX6",
				   &bsnc->sid, &bsnc->nexthop, bsnc->bgp->name_pretty);

		zclient_send_localsid(bgp_zclient, ZEBRA_ROUTE_ADD, &bsnc->sid,
				      IPV6_MAX_BITLEN, ifp->ifindex, act, &ctx);

		sid_ls = XCALLOC(MTYPE_BGP_SRV6_SID, sizeof(struct in6_addr));
		*sid_ls = bsnc->sid;
		if (bsnc->zebra_sid_last_sent)
			XFREE(MTYPE_BGP_SRV6_SID, bsnc->zebra_sid_last_sent);
		bsnc->zebra_sid_last_sent = sid_ls;
	} else {
		if (bsnc->nexthop.family == AF_INET)
			act = ZEBRA_SEG6_LOCAL_ACTION_END_DX4;
		else
			act = ZEBRA_SEG6_LOCAL_ACTION_END_DX6;

		if (bsnc->zebra_sid_last_sent) {
			if (debug)
				zlog_debug("%s: withdrawing seg6local %s SID %pI6 for nexthop %pFX VRF %s",
					   __func__,
					   (act == ZEBRA_SEG6_LOCAL_ACTION_END_DX4)
						   ? "End.DX4" : "End.DX6",
					   bsnc->zebra_sid_last_sent, &bsnc->nexthop,
					   bsnc->bgp->name_pretty);

			zclient_send_localsid(bgp_zclient, ZEBRA_ROUTE_DELETE,
					      bsnc->zebra_sid_last_sent,
					      IPV6_MAX_BITLEN, ifp->ifindex,
					      ZEBRA_SEG6_LOCAL_ACTION_UNSPEC, &ctx);
			XFREE(MTYPE_BGP_SRV6_SID, bsnc->zebra_sid_last_sent);
			bsnc->zebra_sid_last_sent = NULL;
		}
	}
}

void bgp_srv6_per_nexthop_free(struct bgp_srv6_per_nexthop_cache *bsnc)
{
	int debug = BGP_DEBUG(vpn, VPN_LEAK_LABEL);

	if (debug)
		zlog_debug("%s: freeing per-CE SRv6 SID cache for nexthop %pFX",
			   __func__, &bsnc->nexthop);

	/* Withdraw the local seg6local route */
	bgp_srv6_per_nexthop_localsid(bsnc, false);

	/* Release SID back to zebra SRv6 manager */
	if (bsnc->sid_valid && bsnc->locator) {
		struct srv6_sid_ctx ctx = {};

		if (bsnc->nexthop.family == AF_INET) {
			ctx.behavior = ZEBRA_SEG6_LOCAL_ACTION_END_DX4;
			ctx.nh4 = bsnc->nexthop.u.prefix4;
		} else {
			ctx.behavior = ZEBRA_SEG6_LOCAL_ACTION_END_DX6;
			ctx.nh6 = bsnc->nexthop.u.prefix6;
		}
		ctx.vrf_id = bsnc->bgp->vrf_id;
		bgp_zebra_release_srv6_sid(&ctx, bsnc->locator->name);

		sid_unregister(bgp_get_default(), &bsnc->sid);
	}

	bgp_srv6_per_nexthop_cache_del(bsnc->tree, bsnc);

	if (bsnc->locator)
		srv6_locator_free(bsnc->locator);
	if (bsnc->nh)
		nexthop_free(bsnc->nh);
	bsnc->nh = NULL;

	XFREE(MTYPE_BGP_SRV6_PER_NEXTHOP_CACHE, bsnc);
}

void bgp_srv6_per_nexthop_path_unlink(struct bgp_path_info *pi)
{
	struct bgp_srv6_per_nexthop_cache *bsnc;

	if (!pi)
		return;

	if (!CHECK_FLAG(pi->flags, BGP_PATH_SRV6_SID_PER_CE))
		return;

	bsnc = pi->mplsvpn.blnc.label_nexthop_cache_srv6;

	if (!bsnc)
		return;

	LIST_REMOVE(pi, mplsvpn.blnc.label_nh_thread);
	bsnc->path_count--;
	pi->mplsvpn.blnc.label_nexthop_cache_srv6 = NULL;
	UNSET_FLAG(pi->flags, BGP_PATH_SRV6_SID_PER_CE);

	if (LIST_EMPTY(&(bsnc->paths)))
		bgp_srv6_per_nexthop_free(bsnc);
}

/*
 * Show helper: display per-CE SRv6 SID cache for one AFI.
 */
static void show_bgp_srv6_nexthop_afi(struct vty *vty, afi_t afi,
				      struct bgp *bgp, bool detail)
{
	struct bgp_srv6_per_nexthop_cache_head *tree;
	struct bgp_srv6_per_nexthop_cache *iter;
	void *src;
	char buf[PREFIX2STR_BUFFER];
	struct bgp_path_info *path;
	struct bgp_dest *dest;
	struct bgp_table *table;
	struct bgp *bgp_path;
	safi_t safi;

	vty_out(vty, "Current BGP SRv6 per-CE SID cache for %s, VRF %s\n",
		afi2str(afi), bgp->name_pretty);

	tree = &bgp->srv6_sids_per_nexthop[afi];
	frr_each (bgp_srv6_per_nexthop_cache, tree, iter) {
		if (afi2family(afi) == AF_INET)
			src = (void *)&iter->nexthop.u.prefix4;
		else
			src = (void *)&iter->nexthop.u.prefix6;

		vty_out(vty, " %s, SID %pI6 (%s) #paths %u\n",
			inet_ntop(afi2family(afi), src, buf, sizeof(buf)),
			&iter->sid,
			iter->sid_valid
				? srv6_endpoint_behavior_codepoint2str(
					  iter->endpoint_behavior)
				: "pending",
			iter->path_count);
		if (iter->nh)
			vty_out(vty, "  if %s\n",
				ifindex2ifname(iter->nh->ifindex,
					       iter->nh->vrf_id));
		vty_out(vty, "  Last update: %s",
			time_to_string(iter->last_update, buf));
		if (!detail)
			continue;
		vty_out(vty, "  Paths:\n");
		LIST_FOREACH(path, &(iter->paths),
			     mplsvpn.blnc.label_nh_thread) {
			dest = path->net;
			table = bgp_dest_table(dest);
			assert(dest && table);
			afi = family2afi(bgp_dest_get_prefix(dest)->family);
			safi = table->safi;
			bgp_path = table->bgp;

			if (dest->pdest) {
				vty_out(vty, "    %d/%d %pBD RD ", afi, safi,
					dest);
				vty_out(vty,
					BGP_RD_AS_FORMAT(bgp->asnotation),
					(struct prefix_rd *)bgp_dest_get_prefix(
						dest->pdest));
				vty_out(vty, " %s flags 0x%x\n",
					bgp_path->name_pretty, path->flags);
			} else
				vty_out(vty, "    %d/%d %pBD %s flags 0x%x\n",
					afi, safi, dest, bgp_path->name_pretty,
					path->flags);
		}
	}
}

DEFUN(show_bgp_srv6_nexthop, show_bgp_srv6_nexthop_cmd,
      "show bgp [<view|vrf> VIEWVRFNAME] srv6-sid-nexthop [detail]",
      SHOW_STR BGP_STR BGP_INSTANCE_HELP_STR
      "BGP SRv6 per-CE SID table\n"
      "Show detailed information\n")
{
	int idx = 0;
	char *vrf = NULL;
	struct bgp *bgp;
	bool show_detail = false;
	int show_afi;

	if (argv_find(argv, argc, "vrf", &idx)) {
		vrf = argv[++idx]->arg;
		bgp = bgp_lookup_by_name(vrf);
	} else
		bgp = bgp_get_default();

	if (!bgp)
		return CMD_SUCCESS;

	if (argv_find(argv, argc, "detail", &idx))
		show_detail = true;

	for (show_afi = AFI_IP; show_afi <= AFI_IP6; show_afi++)
		show_bgp_srv6_nexthop_afi(vty, show_afi, bgp, show_detail);
	return CMD_SUCCESS;
}

void bgp_srv6_per_nexthop_init(void)
{
	install_element(VIEW_NODE, &show_bgp_srv6_nexthop_cmd);
}
