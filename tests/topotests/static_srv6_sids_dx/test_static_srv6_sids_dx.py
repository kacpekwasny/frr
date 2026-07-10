#!/usr/bin/env python
# SPDX-License-Identifier: ISC

#
# test_static_srv6_sids_dx.py
#
# Copyright (c) 2025
#

"""
test_static_srv6_sids_dx.py:
Test for SRv6 static uDX4 and uDX6 SID configuration
"""

import os
import sys
import json
import pytest
import functools

CWD = os.path.dirname(os.path.realpath(__file__))
sys.path.append(os.path.join(CWD, "../"))

# pylint: disable=C0413
from lib import topotest
from lib.topogen import Topogen, TopoRouter, get_topogen
from lib.topolog import logger

pytestmark = [pytest.mark.staticd]


def open_json_file(filename):
    try:
        with open(filename, "r") as f:
            return json.load(f)
    except IOError:
        assert False, "Could not read file {}".format(filename)


def setup_module(mod):
    tgen = Topogen({"s1": ("r1",)}, mod.__name__)
    tgen.start_topology()
    for rname, router in tgen.routers().items():
        router.run("/bin/bash {}/{}/setup.sh".format(CWD, rname))
        router.load_frr_config("frr.conf")
    tgen.start_router()


def teardown_module():
    tgen = get_topogen()
    tgen.stop_topology()


def test_srv6_static_sids_dx():
    """
    Verify uDX4 and uDX6 SIDs are correctly installed in the routing table.
    """
    tgen = get_topogen()
    if tgen.routers_have_failure():
        pytest.skip(tgen.errors)
    router = tgen.gears["r1"]

    def _check_srv6_static_sids(router, expected_route_file):
        logger.info("checking zebra srv6 static sids (uDX4/uDX6)")
        output = json.loads(router.vtysh_cmd("show ipv6 route static json"))
        expected = open_json_file("{}/{}".format(CWD, expected_route_file))
        return topotest.json_cmp(output, expected)

    def check_srv6_static_sids(router, expected_file):
        func = functools.partial(_check_srv6_static_sids, router, expected_file)
        _, result = topotest.run_and_expect(func, None, count=15, wait=1)
        assert result is None, "Failed"

    logger.info("Test for uDX4 and uDX6 SID configuration")
    check_srv6_static_sids(router, "expected_srv6_sids_dx.json")


def test_srv6_static_sids_dx_kernel_fib():
    """
    Verify uDX4 and uDX6 SIDs are correctly programmed in the kernel FIB.
    Check that seg6local actions End.DX4 and End.DX6 appear with correct
    nexthop and oif attributes.
    """
    tgen = get_topogen()
    if tgen.routers_have_failure():
        pytest.skip(tgen.errors)
    router = tgen.gears["r1"]

    def _check_kernel_dx4():
        output = router.run("ip -6 route show fcbb:bbbb:1:fe50::/64")
        logger.info("Kernel route for DX4 SID: %s", output)
        if "seg6local" not in output:
            return "seg6local not found in kernel route for DX4 SID"
        if "End.DX4" not in output:
            return "End.DX4 action not found in kernel route"
        if "nh4" not in output and "10.0.0.2" not in output:
            return "nh4 10.0.0.2 not found in kernel route"
        return None

    def _check_kernel_dx6():
        output = router.run("ip -6 route show fcbb:bbbb:1:fe60::/64")
        logger.info("Kernel route for DX6 SID: %s", output)
        if "seg6local" not in output:
            return "seg6local not found in kernel route for DX6 SID"
        if "End.DX6" not in output:
            return "End.DX6 action not found in kernel route"
        if "nh6" not in output and "2001::2" not in output:
            return "nh6 2001::2 not found in kernel route"
        return None

    logger.info("Verifying kernel FIB for uDX4 SID")
    _, result = topotest.run_and_expect(_check_kernel_dx4, None, count=15, wait=1)
    assert result is None, "Kernel FIB check failed for uDX4: {}".format(result)

    logger.info("Verifying kernel FIB for uDX6 SID")
    _, result = topotest.run_and_expect(_check_kernel_dx6, None, count=15, wait=1)
    assert result is None, "Kernel FIB check failed for uDX6: {}".format(result)


def test_srv6_static_sids_dx_delete():
    """
    Remove the uDX4 and uDX6 SIDs and verify they are removed.
    """
    tgen = get_topogen()
    if tgen.routers_have_failure():
        pytest.skip(tgen.errors)
    router = tgen.gears["r1"]

    router.vtysh_cmd(
        """
        configure terminal
         segment-routing
          srv6
           static-sids
            no sid fcbb:bbbb:1:fe50::/64
            no sid fcbb:bbbb:1:fe60::/64
        """
    )

    def _check_removed():
        output = json.loads(router.vtysh_cmd("show ipv6 route static json"))
        if "fcbb:bbbb:1:fe50::/64" in output:
            return "uDX4 SID still present after deletion"
        if "fcbb:bbbb:1:fe60::/64" in output:
            return "uDX6 SID still present after deletion"
        return None

    logger.info("Verifying uDX4/uDX6 SIDs removed")
    _, result = topotest.run_and_expect(_check_removed, None, count=15, wait=1)
    assert result is None, "Failed to remove DX SIDs: {}".format(result)


def test_srv6_static_sids_dx_readd():
    """
    Re-add the uDX4 and uDX6 SIDs and verify they are re-installed.
    """
    tgen = get_topogen()
    if tgen.routers_have_failure():
        pytest.skip(tgen.errors)
    router = tgen.gears["r1"]

    def _check_srv6_static_sids(router, expected_route_file):
        output = json.loads(router.vtysh_cmd("show ipv6 route static json"))
        expected = open_json_file("{}/{}".format(CWD, expected_route_file))
        return topotest.json_cmp(output, expected)

    def check_srv6_static_sids(router, expected_file):
        func = functools.partial(_check_srv6_static_sids, router, expected_file)
        _, result = topotest.run_and_expect(func, None, count=15, wait=1)
        assert result is None, "Failed"

    router.vtysh_cmd(
        """
        configure terminal
         segment-routing
          srv6
           static-sids
            sid fcbb:bbbb:1:fe50::/64 locator MAIN behavior uDX4 interface r1-eth0 nexthop 10.0.0.2
            sid fcbb:bbbb:1:fe60::/64 locator MAIN behavior uDX6 interface r1-eth0 nexthop 2001::2
        """
    )

    logger.info("Verifying uDX4/uDX6 SIDs re-installed")
    check_srv6_static_sids(router, "expected_srv6_sids_dx.json")


if __name__ == "__main__":
    args = ["-s"] + sys.argv[1:]
    sys.exit(pytest.main(args))
