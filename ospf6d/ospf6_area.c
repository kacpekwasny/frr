/*
 * Copyright (C) 2003 Yasuhiro Ohara
 *
 * This file is part of GNU Zebra.
 *
 * GNU Zebra is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2, or (at your option) any
 * later version.
 *
 * GNU Zebra is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with GNU Zebra; see the file COPYING.  If not, write to the 
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330, 
 * Boston, MA 02111-1307, USA.  
 */

#include <zebra.h>

#include "log.h"
#include "memory.h"
#include "linklist.h"
#include "thread.h"
#include "vty.h"
#include "command.h"
#include "if.h"
#include "prefix.h"
#include "table.h"

#include "ospf6_proto.h"
#include "ospf6_lsa.h"
#include "ospf6_lsdb.h"
#include "ospf6_route.h"
#include "ospf6_spf.h"
#include "ospf6_top.h"
#include "ospf6_area.h"
#include "ospf6_interface.h"
#include "ospf6_intra.h"
#include "ospf6_abr.h"
#include "ospf6d.h"

int
ospf6_area_cmp (void *va, void *vb)
{
  struct ospf6_area *oa = (struct ospf6_area *) va;
  struct ospf6_area *ob = (struct ospf6_area *) vb;
  return (ntohl (oa->area_id) - ntohl (ob->area_id));
}

int
ospf6_area_is_stub (struct ospf6_area *o6a)
{
  if (OSPF6_OPT_ISSET (o6a->options, OSPF6_OPT_E))
    return 0;
  return 1;
}

/* schedule routing table recalculation */
void
ospf6_area_lsdb_hook_add (struct ospf6_lsa *lsa)
{
  struct ospf6_area *oa;

  oa = (struct ospf6_area *) lsa->scope;
  switch (ntohs (lsa->header->type))
    {
    case OSPF6_LSTYPE_ROUTER:
    case OSPF6_LSTYPE_NETWORK:
      ospf6_spf_schedule (oa);
      break;

    case OSPF6_LSTYPE_INTRA_PREFIX:
      ospf6_intra_prefix_lsa_add (lsa);
      break;

    case OSPF6_LSTYPE_INTER_PREFIX:
    case OSPF6_LSTYPE_INTER_ROUTER:
      break;

    default:
      if (IS_OSPF6_DEBUG_LSA (RECV))
	zlog_info ("Unknown LSA in Area %s's lsdb", oa->name);
      break;
    }
}

void
ospf6_area_lsdb_hook_remove (struct ospf6_lsa *lsa)
{
  struct ospf6_area *oa;

  oa = (struct ospf6_area *) lsa->scope;
  switch (ntohs (lsa->header->type))
    {
    case OSPF6_LSTYPE_ROUTER:
    case OSPF6_LSTYPE_NETWORK:
      ospf6_spf_schedule (oa);
      break;

    case OSPF6_LSTYPE_INTRA_PREFIX:
      ospf6_intra_prefix_lsa_remove (lsa);
      break;

    case OSPF6_LSTYPE_INTER_PREFIX:
    case OSPF6_LSTYPE_INTER_ROUTER:
      break;

    default:
      if (IS_OSPF6_DEBUG_LSA (RECV))
	zlog_info ("Unknown LSA in Area %s's lsdb", oa->name);
      break;
    }
}

void
ospf6_area_route_hook_add (struct ospf6_route *route)
{
  struct ospf6_route *copy = ospf6_route_copy (route);
  ospf6_route_add (copy, ospf6->route_table);
}

void
ospf6_area_route_hook_remove (struct ospf6_route *route)
{
  struct ospf6_route *copy;

  copy = ospf6_route_lookup_identical (route, ospf6->route_table);
  if (copy)
    ospf6_route_remove (copy, ospf6->route_table);
}

/* Make new area structure */
struct ospf6_area *
ospf6_area_create (u_int32_t area_id, struct ospf6 *o)
{
  struct ospf6_area *oa;
  struct ospf6_route *route;

  oa = XCALLOC (MTYPE_OSPF6_AREA, sizeof (struct ospf6_area));

  inet_ntop (AF_INET, &area_id, oa->name, sizeof (oa->name));
  oa->area_id = area_id;
  oa->if_list = list_new ();

  oa->summary_table = ospf6_route_table_create ();

  oa->lsdb = ospf6_lsdb_create ();
  oa->lsdb->hook_add = ospf6_area_lsdb_hook_add;
  oa->lsdb->hook_remove = ospf6_area_lsdb_hook_remove;

  oa->spf_table = ospf6_route_table_create ();
  oa->route_table = ospf6_route_table_create ();
  oa->route_table->hook_add = ospf6_area_route_hook_add;
  oa->route_table->hook_remove = ospf6_area_route_hook_remove;

  /* set default options */
  OSPF6_OPT_SET (oa->options, OSPF6_OPT_V6);
  OSPF6_OPT_SET (oa->options, OSPF6_OPT_E);
  OSPF6_OPT_SET (oa->options, OSPF6_OPT_R);

  oa->ospf6 = o;
  listnode_add_sort (o->area_list, oa);

  /* import athoer area's routes as inter-area routes */
  for (route = ospf6_route_head (o->route_table); route;
       route = ospf6_route_next (route))
    ospf6_abr_originate_prefix_to_area (route, oa);

  return oa;
}

void
ospf6_area_delete (struct ospf6_area *oa)
{
  listnode n;
  struct ospf6_interface *oi;

  ospf6_route_table_delete (oa->summary_table);

  /* ospf6 interface list */
  for (n = listhead (oa->if_list); n; nextnode (n))
    {
      oi = (struct ospf6_interface *) getdata (n);
      ospf6_interface_delete (oi);
    }
  list_delete (oa->if_list);

  ospf6_lsdb_delete (oa->lsdb);
  ospf6_route_table_delete (oa->spf_table);
  ospf6_route_table_delete (oa->route_table);

#if 0
  ospf6_spftree_delete (oa->spf_tree);
  ospf6_route_table_delete (oa->topology_table);
#endif /*0*/

  THREAD_OFF (oa->thread_spf_calculation);
  THREAD_OFF (oa->thread_route_calculation);

  listnode_delete (oa->ospf6->area_list, oa);
  oa->ospf6 = NULL;

  /* free area */
  XFREE (MTYPE_OSPF6_AREA, oa);
}

struct ospf6_area *
ospf6_area_lookup (u_int32_t area_id, struct ospf6 *ospf6)
{
  struct ospf6_area *oa;
  listnode n;

  for (n = listhead (ospf6->area_list); n; nextnode (n))
    {
      oa = (struct ospf6_area *) getdata (n);
      if (oa->area_id == area_id)
        return oa;
    }

  return (struct ospf6_area *) NULL;
}

void
ospf6_area_enable (struct ospf6_area *oa)
{
  listnode i;
  struct ospf6_interface *oi;

  UNSET_FLAG (oa->flag, OSPF6_AREA_DISABLE);

  for (i = listhead (oa->if_list); i; nextnode (i))
    {
      oi = (struct ospf6_interface *) getdata (i);
      ospf6_interface_enable (oi);
    }
}

void
ospf6_area_disable (struct ospf6_area *oa)
{
  listnode i;
  struct ospf6_interface *oi;

  SET_FLAG (oa->flag, OSPF6_AREA_DISABLE);

  for (i = listhead (oa->if_list); i; nextnode (i))
    {
      oi = (struct ospf6_interface *) getdata (i);
      ospf6_interface_disable (oi);
    }
}


void
ospf6_area_show (struct vty *vty, struct ospf6_area *oa)
{
  listnode i;
  struct ospf6_interface *oi;

  vty_out (vty, " Area %s%s", oa->name, VNL);
  vty_out (vty, "     Number of Area scoped LSAs is %u%s",
           oa->lsdb->count, VNL);

  vty_out (vty, "     Interface attached to this area:");
  for (i = listhead (oa->if_list); i; nextnode (i))
    {
      oi = (struct ospf6_interface *) getdata (i);
      vty_out (vty, " %s", oi->interface->name);
    }
  vty_out (vty, "%s", VNL);
}


#define OSPF6_CMD_AREA_LOOKUP(str, oa)                     \
{                                                          \
  u_int32_t area_id = 0;                                   \
  if (inet_pton (AF_INET, str, &area_id) != 1)             \
    {                                                      \
      vty_out (vty, "Malformed Area-ID: %s%s", str, VNL);  \
      return CMD_SUCCESS;                                  \
    }                                                      \
  oa = ospf6_area_lookup (area_id, ospf6);                 \
  if (oa == NULL)                                          \
    {                                                      \
      vty_out (vty, "No such Area: %s%s", str, VNL);       \
      return CMD_SUCCESS;                                  \
    }                                                      \
}

DEFUN (show_ipv6_ospf6_area_route_intra,
       show_ipv6_ospf6_area_route_intra_cmd,
       "show ipv6 ospf6 area A.B.C.D route intra-area",
       SHOW_STR
       IP6_STR
       OSPF6_STR
       OSPF6_AREA_STR
       OSPF6_AREA_ID_STR
       ROUTE_STR
       "Display Intra-Area routes\n"
       )
{
  struct ospf6_area *oa;
  OSPF6_CMD_AREA_LOOKUP (argv[0], oa);
  argc--;
  argv++;
  return ospf6_route_table_show (vty, argc, argv, oa->route_table);
}

ALIAS (show_ipv6_ospf6_area_route_intra,
       show_ipv6_ospf6_area_route_intra_detail_cmd,
       "show ipv6 ospf6 area A.B.C.D route intra-area (X::X|X::X/M|detail)",
       SHOW_STR
       IP6_STR
       OSPF6_STR
       OSPF6_AREA_STR
       OSPF6_AREA_ID_STR
       ROUTE_STR
       "Display Intra-Area routes\n"
       "Specify IPv6 address\n"
       "Specify IPv6 prefix\n"
       "Detailed information\n"
       );

DEFUN (show_ipv6_ospf6_area_route_intra_match,
       show_ipv6_ospf6_area_route_intra_match_cmd,
       "show ipv6 ospf6 area A.B.C.D route intra-area X::X/M match",
       SHOW_STR
       IP6_STR
       OSPF6_STR
       ROUTE_STR
       "Display Intra-Area routes\n"
       OSPF6_AREA_STR
       OSPF6_AREA_ID_STR
       "Specify IPv6 prefix\n"
       "Display routes which match the specified route\n"
       )
{
  char *sargv[CMD_ARGC_MAX];
  int i, sargc;
  struct ospf6_area *oa;

  OSPF6_CMD_AREA_LOOKUP (argv[0], oa);
  argc--;
  argv++;

  /* copy argv to sargv and then append "match" */
  for (i = 0; i < argc; i++)
    sargv[i] = argv[i];
  sargc = argc;
  sargv[sargc++] = "match";
  sargv[sargc] = NULL;

  return ospf6_route_table_show (vty, sargc, sargv, oa->route_table);
}

DEFUN (show_ipv6_ospf6_area_route_intra_match_detail,
       show_ipv6_ospf6_area_route_intra_match_detail_cmd,
       "show ipv6 ospf6 area A.B.C.D route intra-area X::X/M match detail",
       SHOW_STR
       IP6_STR
       OSPF6_STR
       OSPF6_AREA_STR
       OSPF6_AREA_ID_STR
       ROUTE_STR
       "Display Intra-Area routes\n"
       "Specify IPv6 prefix\n"
       "Display routes which match the specified route\n"
       "Detailed information\n"
       )
{
  char *sargv[CMD_ARGC_MAX];
  int i, sargc;
  struct ospf6_area *oa;

  OSPF6_CMD_AREA_LOOKUP (argv[0], oa);
  argc--;
  argv++;

  /* copy argv to sargv and then append "match" and "detail" */
  for (i = 0; i < argc; i++)
    sargv[i] = argv[i];
  sargc = argc;
  sargv[sargc++] = "match";
  sargv[sargc++] = "detail";
  sargv[sargc] = NULL;

  return ospf6_route_table_show (vty, sargc, sargv, oa->route_table);
}

DEFUN (show_ipv6_ospf6_route_intra,
       show_ipv6_ospf6_route_intra_cmd,
       "show ipv6 ospf6 route intra-area",
       SHOW_STR
       IP6_STR
       OSPF6_STR
       ROUTE_STR
       "Display Intra-Area routes\n"
       )
{
  listnode node;
  struct ospf6_area *oa;

  for (node = listhead (ospf6->area_list); node; nextnode (node))
    {
      oa = (struct ospf6_area *) getdata (node);
      vty_out (vty, "Area %s%s", oa->name, VNL);
      ospf6_route_table_show (vty, argc, argv, oa->route_table);
    }

  return CMD_SUCCESS;
}

ALIAS (show_ipv6_ospf6_route_intra,
       show_ipv6_ospf6_route_intra_detail_cmd,
       "show ipv6 ospf6 route intra-area (X::X|X::X/M|detail|summary)",
       SHOW_STR
       IP6_STR
       OSPF6_STR
       ROUTE_STR
       "Display Intra-Area routes\n"
       "Specify IPv6 address\n"
       "Specify IPv6 prefix\n"
       "Detailed information\n"
       "Summary of route table\n"
       );

DEFUN (show_ipv6_ospf6_route_intra_match,
       show_ipv6_ospf6_route_intra_match_cmd,
       "show ipv6 ospf6 route intra-area X::X/M match",
       SHOW_STR
       IP6_STR
       OSPF6_STR
       ROUTE_STR
       "Display Intra-Area routes\n"
       "Specify IPv6 prefix\n"
       "Display routes which match the specified route\n"
       )
{
  char *sargv[CMD_ARGC_MAX];
  int i, sargc;
  listnode node;
  struct ospf6_area *oa;

  /* copy argv to sargv and then append "match" */
  for (i = 0; i < argc; i++)
    sargv[i] = argv[i];
  sargc = argc;
  sargv[sargc++] = "match";
  sargv[sargc] = NULL;

  for (node = listhead (ospf6->area_list); node; nextnode (node))
    {
      oa = (struct ospf6_area *) getdata (node);
      ospf6_route_table_show (vty, sargc, sargv, oa->route_table);
    }

  return CMD_SUCCESS;
}

DEFUN (show_ipv6_ospf6_route_intra_match_detail,
       show_ipv6_ospf6_route_intra_match_detail_cmd,
       "show ipv6 ospf6 route intra-area X::X/M match detail",
       SHOW_STR
       IP6_STR
       OSPF6_STR
       ROUTE_STR
       "Display Intra-Area routes\n"
       "Specify IPv6 prefix\n"
       "Display routes which match the specified route\n"
       "Detailed information\n"
       )
{
  char *sargv[CMD_ARGC_MAX];
  int i, sargc;
  listnode node;
  struct ospf6_area *oa;

  /* copy argv to sargv and then append "match" and "detail" */
  for (i = 0; i < argc; i++)
    sargv[i] = argv[i];
  sargc = argc;
  sargv[sargc++] = "match";
  sargv[sargc++] = "detail";
  sargv[sargc] = NULL;

  for (node = listhead (ospf6->area_list); node; nextnode (node))
    {
      oa = (struct ospf6_area *) getdata (node);
      ospf6_route_table_show (vty, sargc, sargv, oa->route_table);
    }

  return CMD_SUCCESS;
}

DEFUN (show_ipv6_ospf6_spf_tree,
       show_ipv6_ospf6_spf_tree_cmd,
       "show ipv6 ospf6 spf tree",
       SHOW_STR
       IP6_STR
       OSPF6_STR
       "Shortest Path First caculation\n"
       "Show SPF tree\n")
{
  listnode node;
  struct ospf6_area *oa;
  struct ospf6_vertex *root;
  struct ospf6_route *route;
  struct prefix prefix;

  ospf6_linkstate_prefix (ospf6->router_id, htonl (0), &prefix);
  for (node = listhead (ospf6->area_list); node; nextnode (node))
    {
      oa = (struct ospf6_area *) getdata (node);
      route = ospf6_route_lookup (&prefix, oa->spf_table);
      if (route == NULL)
        {
          vty_out (vty, "LS entry for root not found in area %s%s",
                   oa->name, VNL);
          continue;
        }
      root = (struct ospf6_vertex *) route->route_option;
      ospf6_spf_display_subtree (vty, "", 0, root);
    }

  return CMD_SUCCESS;
}

DEFUN (show_ipv6_ospf6_area_spf_tree,
       show_ipv6_ospf6_area_spf_tree_cmd,
       "show ipv6 ospf6 area A.B.C.D spf tree",
       SHOW_STR
       IP6_STR
       OSPF6_STR
       OSPF6_AREA_STR
       OSPF6_AREA_ID_STR
       "Shortest Path First caculation\n"
       "Show SPF tree\n")
{
  u_int32_t area_id;
  struct ospf6_area *oa;
  struct ospf6_vertex *root;
  struct ospf6_route *route;
  struct prefix prefix;

  ospf6_linkstate_prefix (ospf6->router_id, htonl (0), &prefix);

  if (inet_pton (AF_INET, argv[0], &area_id) != 1)
    {
      vty_out (vty, "Malformed Area-ID: %s%s", argv[0], VNL);
      return CMD_SUCCESS;
    }
  oa = ospf6_area_lookup (area_id, ospf6);
  if (oa == NULL)
    {
      vty_out (vty, "No such Area: %s%s", argv[0], VNL);
      return CMD_SUCCESS;
    }

  route = ospf6_route_lookup (&prefix, oa->spf_table);
  if (route == NULL)
    {
      vty_out (vty, "LS entry for root not found in area %s%s",
               oa->name, VNL);
      return CMD_SUCCESS;
    }
  root = (struct ospf6_vertex *) route->route_option;
  ospf6_spf_display_subtree (vty, "", 0, root);

  return CMD_SUCCESS;
}

DEFUN (show_ipv6_ospf6_area_spf_table,
       show_ipv6_ospf6_area_spf_table_cmd,
       "show ipv6 ospf6 area A.B.C.D spf table",
       SHOW_STR
       IP6_STR
       OSPF6_STR
       OSPF6_AREA_STR
       OSPF6_AREA_ID_STR
       "Shortest Path First caculation\n"
       "Show table contains SPF result\n"
       )
{
  u_int32_t area_id;
  struct ospf6_area *oa;

  if (inet_pton (AF_INET, argv[0], &area_id) != 1)
    {
      vty_out (vty, "Malformed Area-ID: %s%s", argv[0], VNL);
      return CMD_SUCCESS;
    }
  oa = ospf6_area_lookup (area_id, ospf6);
  if (oa == NULL)
    {
      vty_out (vty, "No such Area: %s%s", argv[0], VNL);
      return CMD_SUCCESS;
    }

  argc--;
  argv++;

  ospf6_lsentry_table_show (vty, argc, argv, oa->spf_table);
  return CMD_SUCCESS;
}

ALIAS (show_ipv6_ospf6_area_spf_table,
       show_ipv6_ospf6_area_spf_table_1_cmd,
       "show ipv6 ospf6 area A.B.C.D spf table (A.B.C.D|A.B.C.D/M|detail)",
       SHOW_STR
       IP6_STR
       OSPF6_STR
       OSPF6_AREA_STR
       OSPF6_AREA_ID_STR
       "Shortest Path First caculation\n"
       "Show table contains SPF result\n"
       "Specify Router-ID\n"
       "Display multiple entry by specifying match-prefix of Router-ID\n"
       "Display Detail\n"
       );

ALIAS (show_ipv6_ospf6_area_spf_table,
       show_ipv6_ospf6_area_spf_table_2_cmd,
       "show ipv6 ospf6 area A.B.C.D spf table (A.B.C.D|*) (A.B.C.D|A.B.C.D/M|detail)",
       SHOW_STR
       IP6_STR
       OSPF6_STR
       OSPF6_AREA_STR
       OSPF6_AREA_ID_STR
       "Shortest Path First caculation\n"
       "Show table contains SPF result\n"
       "Specify Router-ID\n"
       "Wildcard Router-ID\n"
       "Specify Link State ID\n"
       "Display multiple entry by specifying match-prefix of Link State ID\n"
       "Display Detail\n"
       );

DEFUN (show_ipv6_ospf6_area_spf_table_3,
       show_ipv6_ospf6_area_spf_table_3_cmd,
       "show ipv6 ospf6 area A.B.C.D spf table (A.B.C.D|*) A.B.C.D/M detail",
       SHOW_STR
       IP6_STR
       OSPF6_STR
       OSPF6_AREA_STR
       OSPF6_AREA_ID_STR
       "Shortest Path First caculation\n"
       "Show table contains SPF result\n"
       "Specify Router-ID\n"
       "Wildcard Router-ID\n"
       "Display multiple entry by specifying match-prefix of Link State ID\n"
       "Display Detail\n"
       )
{
  u_int32_t area_id;
  struct ospf6_area *oa;
  char *sargv[CMD_ARGC_MAX];
  int i, sargc;

  if (inet_pton (AF_INET, argv[0], &area_id) != 1)
    {
      vty_out (vty, "Malformed Area-ID: %s%s", argv[0], VNL);
      return CMD_SUCCESS;
    }
  oa = ospf6_area_lookup (area_id, ospf6);
  if (oa == NULL)
    {
      vty_out (vty, "No such Area: %s%s", argv[0], VNL);
      return CMD_SUCCESS;
    }

  argc--;
  argv++;

  /* copy argv to sargv and then append "detail" */
  for (i = 0; i < argc; i++)
    sargv[i] = argv[i];
  sargc = argc;
  sargv[sargc++] = "detail";
  sargv[sargc] = NULL;

  ospf6_lsentry_table_show (vty, sargc, sargv, oa->spf_table);
  return CMD_SUCCESS;
}

DEFUN (show_ipv6_ospf6_spf_table,
       show_ipv6_ospf6_spf_table_cmd,
       "show ipv6 ospf6 spf table",
       SHOW_STR
       IP6_STR
       OSPF6_STR
       "Shortest Path First caculation\n"
       "Show table contains SPF result\n"
       )
{
  listnode node;
  struct ospf6_area *oa;

  for (node = listhead (ospf6->area_list); node; nextnode (node))
    {
      oa = (struct ospf6_area *) getdata (node);
      ospf6_lsentry_table_show (vty, argc, argv, oa->spf_table);
    }

  return CMD_SUCCESS;
}

ALIAS (show_ipv6_ospf6_spf_table,
       show_ipv6_ospf6_spf_table_1_cmd,
       "show ipv6 ospf6 spf table (A.B.C.D|A.B.C.D/M|detail)",
       SHOW_STR
       IP6_STR
       OSPF6_STR
       "Shortest Path First caculation\n"
       "Show table contains SPF result\n"
       "Specify Router-ID\n"
       "Display multiple entry by specifying match-prefix of Router-ID\n"
       "Display Detail\n"
       );

ALIAS (show_ipv6_ospf6_spf_table,
       show_ipv6_ospf6_spf_table_2_cmd,
       "show ipv6 ospf6 spf table (A.B.C.D|A.B.C.D/M|*) (A.B.C.D|A.B.C.D/M|detail)",
       SHOW_STR
       IP6_STR
       OSPF6_STR
       "Shortest Path First caculation\n"
       "Show table contains SPF result\n"
       "Specify Router-ID\n"
       "Display multiple entry by specifying match-prefix of Router-ID\n"
       "Wildcard Router-ID\n"
       "Specify Link State ID\n"
       "Display multiple entry by specifying match-prefix of Link State ID\n"
       "Display Detail\n"
       );

DEFUN (show_ipv6_ospf6_spf_table_3,
       show_ipv6_ospf6_spf_table_3_cmd,
       "show ipv6 ospf6 spf table (A.B.C.D|*) A.B.C.D/M detail",
       SHOW_STR
       IP6_STR
       OSPF6_STR
       "Shortest Path First caculation\n"
       "Show table contains SPF result\n"
       "Specify Router-ID\n"
       "Wildcard Router-ID\n"
       "Display multiple entry by specifying match-prefix of Link State ID\n"
       "Display Detail\n"
       )
{
  listnode node;
  struct ospf6_area *oa;
  char *sargv[CMD_ARGC_MAX];
  int i, sargc;

  /* copy argv to sargv and then append "detail" */
  for (i = 0; i < argc; i++)
    sargv[i] = argv[i];
  sargc = argc;
  sargv[sargc++] = "detail";
  sargv[sargc] = NULL;

  for (node = listhead (ospf6->area_list); node; nextnode (node))
    {
      oa = (struct ospf6_area *) getdata (node);
      ospf6_lsentry_table_show (vty, sargc, sargv, oa->spf_table);
    }

  return CMD_SUCCESS;
}

DEFUN (show_ipv6_ospf6_simulate_spf_tree_root,
       show_ipv6_ospf6_simulate_spf_tree_root_cmd,
       "show ipv6 ospf6 simulate spf-tree A.B.C.D area A.B.C.D",
       SHOW_STR
       IP6_STR
       OSPF6_STR
       "Shortest Path First caculation\n"
       "Show SPF tree\n"
       "Specify root's router-id to calculate another router's SPF tree\n")
{
  u_int32_t area_id;
  struct ospf6_area *oa;
  struct ospf6_vertex *root;
  struct ospf6_route *route;
  struct prefix prefix;
  u_int32_t router_id;
  struct ospf6_route_table *spf_table;
  unsigned char tmp_debug_ospf6_spf = 0;

  inet_pton (AF_INET, argv[0], &router_id);
  ospf6_linkstate_prefix (router_id, htonl (0), &prefix);

  if (inet_pton (AF_INET, argv[1], &area_id) != 1)
    {
      vty_out (vty, "Malformed Area-ID: %s%s", argv[1], VNL);
      return CMD_SUCCESS;
    }
  oa = ospf6_area_lookup (area_id, ospf6);
  if (oa == NULL)
    {
      vty_out (vty, "No such Area: %s%s", argv[1], VNL);
      return CMD_SUCCESS;
    }

  tmp_debug_ospf6_spf = conf_debug_ospf6_spf;
  conf_debug_ospf6_spf = 0;

  spf_table = ospf6_route_table_create ();
  ospf6_spf_calculation (router_id, spf_table, oa);

  conf_debug_ospf6_spf = tmp_debug_ospf6_spf;

  route = ospf6_route_lookup (&prefix, spf_table);
  if (route == NULL)
    {
      ospf6_spf_table_finish (spf_table);
      ospf6_route_table_delete (spf_table);
      return CMD_SUCCESS;
    }
  root = (struct ospf6_vertex *) route->route_option;
  ospf6_spf_display_subtree (vty, "", 0, root);

  ospf6_spf_table_finish (spf_table);
  ospf6_route_table_delete (spf_table);

  return CMD_SUCCESS;
}

void
ospf6_area_init ()
{
  install_element (VIEW_NODE, &show_ipv6_ospf6_spf_tree_cmd);
  install_element (VIEW_NODE, &show_ipv6_ospf6_spf_table_cmd);
  install_element (VIEW_NODE, &show_ipv6_ospf6_spf_table_1_cmd);
  install_element (VIEW_NODE, &show_ipv6_ospf6_spf_table_2_cmd);
  install_element (VIEW_NODE, &show_ipv6_ospf6_spf_table_3_cmd);
  install_element (VIEW_NODE, &show_ipv6_ospf6_area_spf_tree_cmd);
  install_element (VIEW_NODE, &show_ipv6_ospf6_area_spf_table_cmd);
  install_element (VIEW_NODE, &show_ipv6_ospf6_area_spf_table_1_cmd);
  install_element (VIEW_NODE, &show_ipv6_ospf6_area_spf_table_2_cmd);
  install_element (VIEW_NODE, &show_ipv6_ospf6_area_spf_table_3_cmd);

  install_element (VIEW_NODE, &show_ipv6_ospf6_route_intra_cmd);
  install_element (VIEW_NODE, &show_ipv6_ospf6_route_intra_detail_cmd);
  install_element (VIEW_NODE, &show_ipv6_ospf6_route_intra_match_cmd);
  install_element (VIEW_NODE, &show_ipv6_ospf6_route_intra_match_detail_cmd);
  install_element (VIEW_NODE, &show_ipv6_ospf6_area_route_intra_cmd);
  install_element (VIEW_NODE, &show_ipv6_ospf6_area_route_intra_detail_cmd);
  install_element (VIEW_NODE, &show_ipv6_ospf6_area_route_intra_match_cmd);
  install_element (VIEW_NODE, &show_ipv6_ospf6_area_route_intra_match_detail_cmd);

  install_element (VIEW_NODE, &show_ipv6_ospf6_simulate_spf_tree_root_cmd);

  install_element (ENABLE_NODE, &show_ipv6_ospf6_spf_tree_cmd);
  install_element (ENABLE_NODE, &show_ipv6_ospf6_spf_table_cmd);
  install_element (ENABLE_NODE, &show_ipv6_ospf6_spf_table_1_cmd);
  install_element (ENABLE_NODE, &show_ipv6_ospf6_spf_table_2_cmd);
  install_element (ENABLE_NODE, &show_ipv6_ospf6_spf_table_3_cmd);
  install_element (ENABLE_NODE, &show_ipv6_ospf6_area_spf_tree_cmd);
  install_element (ENABLE_NODE, &show_ipv6_ospf6_area_spf_table_cmd);
  install_element (ENABLE_NODE, &show_ipv6_ospf6_area_spf_table_1_cmd);
  install_element (ENABLE_NODE, &show_ipv6_ospf6_area_spf_table_2_cmd);
  install_element (ENABLE_NODE, &show_ipv6_ospf6_area_spf_table_3_cmd);

  install_element (ENABLE_NODE, &show_ipv6_ospf6_route_intra_cmd);
  install_element (ENABLE_NODE, &show_ipv6_ospf6_route_intra_detail_cmd);
  install_element (ENABLE_NODE, &show_ipv6_ospf6_route_intra_match_cmd);
  install_element (ENABLE_NODE, &show_ipv6_ospf6_route_intra_match_detail_cmd);
  install_element (ENABLE_NODE, &show_ipv6_ospf6_area_route_intra_cmd);
  install_element (ENABLE_NODE, &show_ipv6_ospf6_area_route_intra_detail_cmd);
  install_element (ENABLE_NODE, &show_ipv6_ospf6_area_route_intra_match_cmd);
  install_element (ENABLE_NODE, &show_ipv6_ospf6_area_route_intra_match_detail_cmd);

  install_element (ENABLE_NODE, &show_ipv6_ospf6_simulate_spf_tree_root_cmd);
}

