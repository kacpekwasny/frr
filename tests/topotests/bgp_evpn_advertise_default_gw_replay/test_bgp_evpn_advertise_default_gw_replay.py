#!/usr/bin/env python
# SPDX-License-Identifier: ISC

"""Verify advertise-default-gw state across a zebra reconnect."""

import os
import sys

import pytest

pytestmark = [pytest.mark.bgpd, pytest.mark.evpn]

CWD = os.path.dirname(os.path.realpath(__file__))
sys.path.append(os.path.join(CWD, "../"))

from lib import topotest
from lib.common_config import kill_router_daemons, start_router_daemons
from lib.topogen import Topogen, TopoRouter, get_topogen

GATEWAY_MAC = "fa:55:00:00:00:01"
GATEWAY_IPV4 = "10.100.0.1"
GATEWAY_IPV6 = "2001:db8:100::1"


def build_topo(tgen):
    tgen.add_router("r1")
    tgen.add_router("r2")
    tgen.add_link(tgen.gears["r1"], tgen.gears["r2"])


def _configure_leaf_dataplane(router):
    router.cmd_raises("ip link add br100 type bridge vlan_filtering 1")
    router.cmd_raises("ip link set dev br100 up")
    router.cmd_raises(
        "ip link add vxlan100 type vxlan id 100 dstport 4789 "
        "local 10.0.0.1 nolearning"
    )
    router.cmd_raises("ip link set dev vxlan100 master br100")
    router.cmd_raises("bridge vlan add vid 100 dev br100 self")
    router.cmd_raises("bridge vlan del vid 1 dev vxlan100")
    router.cmd_raises("bridge vlan add vid 100 dev vxlan100 pvid untagged")
    router.cmd_raises("ip link set dev vxlan100 up")
    router.cmd_raises("ip link add link br100 name vlan100 type vlan id 100")
    router.cmd_raises("ip link set dev vlan100 address {}".format(GATEWAY_MAC))
    router.cmd_raises("ip addr add {}/24 dev vlan100".format(GATEWAY_IPV4))
    router.cmd_raises("ip -6 addr add {}/64 dev vlan100".format(GATEWAY_IPV6))
    router.cmd_raises("ip link set dev vlan100 up")


def setup_module(mod):
    tgen = Topogen(build_topo, mod.__name__)
    tgen.start_topology()

    r1 = tgen.net["r1"]
    r2 = tgen.net["r2"]

    for rname, router in tgen.routers().items():
        router.load_config(
            TopoRouter.RD_ZEBRA, os.path.join(CWD, rname, "zebra.conf")
        )
        router.load_config(
            TopoRouter.RD_BGP, os.path.join(CWD, rname, "bgpd.conf")
        )

    tgen.start_router()
    r1.cmd_raises("ip addr add 10.0.0.1/24 dev r1-eth0")
    r2.cmd_raises("ip addr add 10.0.0.2/24 dev r2-eth0")
    _configure_leaf_dataplane(r1)


def teardown_module(_mod):
    get_topogen().stop_topology()


def _gateway_routes_present(router):
    macs = router.vtysh_cmd("show evpn mac vni 100")
    if GATEWAY_MAC not in macs:
        return "local gateway MAC is missing:\n{}".format(macs)

    routes = router.vtysh_cmd(
        "show bgp l2vpn evpn neighbors 10.0.0.2 advertised-routes"
    )
    for address in (GATEWAY_IPV4, GATEWAY_IPV6):
        if address not in routes:
            return "gateway address {} is missing:\n{}".format(address, routes)
    return None


def _assert_gateway_routes_present(router):
    _, result = topotest.run_and_expect(
        lambda: _gateway_routes_present(router), None, count=30, wait=1
    )
    assert result is None, result


def test_gateway_routes_are_advertised_before_restart():
    tgen = get_topogen()
    if tgen.routers_have_failure():
        pytest.skip(tgen.errors)

    _assert_gateway_routes_present(tgen.routers()["r1"])


def test_global_gateway_routes_replay_after_zebra_restart():
    tgen = get_topogen()
    if tgen.routers_have_failure():
        pytest.skip(tgen.errors)

    kill_router_daemons(tgen, "r1", ["zebra"])
    start_router_daemons(tgen, "r1", ["zebra"])
    _assert_gateway_routes_present(tgen.routers()["r1"])


def test_per_vni_gateway_routes_replay_after_zebra_restart():
    tgen = get_topogen()
    if tgen.routers_have_failure():
        pytest.skip(tgen.errors)

    tgen.routers()["r1"].vtysh_cmd(
        "configure terminal\n"
        "router bgp 65001\n"
        "address-family l2vpn evpn\n"
        "no advertise-default-gw\n"
        "vni 100\n"
        "advertise-default-gw\n"
        "exit-vni\n"
        "exit-address-family\n"
        "end"
    )
    kill_router_daemons(tgen, "r1", ["zebra"])
    start_router_daemons(tgen, "r1", ["zebra"])
    _assert_gateway_routes_present(tgen.routers()["r1"])


if __name__ == "__main__":
    pytest.main(sys.argv[1:])
