# SPDX-License-Identifier: GPL-2.0-or-later
import frrtest


class TestMidrTier1(frrtest.TestMultiOut):
    program = "./test_midr_tier1"


TestMidrTier1.okfail("ordered tier1 hit")
TestMidrTier1.okfail("private asns ignored")
TestMidrTier1.okfail("unordered set hit is degraded")
TestMidrTier1.okfail("empty aspath")
TestMidrTier1.okfail("observed path from traceroute")
TestMidrTier1.okfail("ip2asn snapshot lookup")
TestMidrTier1.okfail("trace observer requires ip2asn")
TestMidrTier1.okfail("invalid arguments")
