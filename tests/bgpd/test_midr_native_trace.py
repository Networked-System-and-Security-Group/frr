# SPDX-License-Identifier: GPL-2.0-or-later
import frrtest


class TestMidrTier1List(frrtest.TestMultiOut):
    program = "./test_midr_tier1_list"


class TestMidrTraceEngine(frrtest.TestMultiOut):
    program = "./test_midr_trace_engine"


class TestMidrTraceScheduler(frrtest.TestMultiOut):
    program = "./test_midr_trace_scheduler"


class TestMidrTraceUdp(frrtest.TestMultiOut):
    program = "./test_midr_trace_udp"


class TestMidrIp2asnUpdate(frrtest.TestMultiOut):
    program = "./test_midr_ip2asn_update"


class TestMidrAdmission(frrtest.TestMultiOut):
    program = "./test_midr_admission"


TestMidrTier1List.onesimple("MIDR Tier-1 list tests passed")
TestMidrTraceEngine.onesimple("MIDR engine tests passed")
TestMidrTraceScheduler.onesimple("MIDR scheduler tests passed")
TestMidrTraceUdp.exit_cleanly()
TestMidrAdmission.onesimple("MIDR admission tests passed")
TestMidrIp2asnUpdate.okfail("transactional ADD/REPLACE/DELETE")
TestMidrIp2asnUpdate.okfail("validate-only and strict rejection paths")
TestMidrIp2asnUpdate.okfail("dirty load/clear guards and explicit discard")
