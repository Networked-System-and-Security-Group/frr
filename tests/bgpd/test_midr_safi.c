// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR internal/wire SAFI registration tests.
 */

#include <zebra.h>

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "privs.h"

#include "bgpd/bgpd.h"

/* Required variables when linking against bgpd/libbgp.a. */
struct zebra_privs_t bgpd_privs = {};
struct event_loop *master;

static void test_midr_mapping(void)
{
	afi_t afi = AFI_UNSPEC;
	safi_t safi = SAFI_UNSPEC;
	iana_afi_t wire_afi = IANA_AFI_RESERVED;
	iana_safi_t wire_safi = IANA_SAFI_RESERVED;

	assert(SAFI_MIDR_LS == 9);
	assert(SAFI_MAX == 10);
	assert(safi_iana2int(IANA_SAFI_MIDR_LS) == SAFI_MIDR_LS);
	assert(safi_int2iana(SAFI_MIDR_LS) == IANA_SAFI_MIDR_LS);
	assert(strcmp(safi2str(SAFI_MIDR_LS), "midr-ls") == 0);

	assert(bgp_map_afi_safi_iana2int(IANA_AFI_BGP_LS, IANA_SAFI_MIDR_LS, &afi, &safi) == 0);
	assert(afi == AFI_BGP_LS);
	assert(safi == SAFI_MIDR_LS);

	assert(bgp_map_afi_safi_int2iana(AFI_BGP_LS, SAFI_MIDR_LS, &wire_afi, &wire_safi) == 0);
	assert(wire_afi == IANA_AFI_BGP_LS);
	assert(wire_safi == IANA_SAFI_MIDR_LS);
}

static void test_invalid_pair(void)
{
	afi_t afi = AFI_UNSPEC;
	safi_t safi = SAFI_UNSPEC;
	iana_afi_t wire_afi = IANA_AFI_RESERVED;
	iana_safi_t wire_safi = IANA_SAFI_RESERVED;

	assert(bgp_map_afi_safi_iana2int(IANA_AFI_IPV4, IANA_SAFI_MIDR_LS, &afi, &safi) == -1);
	assert(bgp_map_afi_safi_iana2int(IANA_AFI_IPV6, IANA_SAFI_MIDR_LS, &afi, &safi) == -1);
	assert(bgp_map_afi_safi_int2iana(AFI_IP, SAFI_MIDR_LS, &wire_afi, &wire_safi) == -1);
	assert(bgp_map_afi_safi_int2iana(AFI_IP6, SAFI_MIDR_LS, &wire_afi, &wire_safi) == -1);
}

static void test_mp_capability_tuple(void)
{
	uint8_t capability[4] = {};
	iana_afi_t wire_afi = IANA_AFI_RESERVED;
	iana_safi_t wire_safi = IANA_SAFI_RESERVED;
	afi_t afi = AFI_UNSPEC;
	safi_t safi = SAFI_UNSPEC;
	uint16_t encoded_afi;

	assert(bgp_map_afi_safi_int2iana(AFI_BGP_LS, SAFI_MIDR_LS, &wire_afi, &wire_safi) == 0);
	encoded_afi = htons(wire_afi);
	memcpy(capability, &encoded_afi, sizeof(encoded_afi));
	capability[2] = 0;
	capability[3] = wire_safi;

	memcpy(&encoded_afi, capability, sizeof(encoded_afi));
	assert(capability[2] == 0);
	assert(bgp_map_afi_safi_iana2int(ntohs(encoded_afi), capability[3], &afi, &safi) == 0);
	assert(afi == AFI_BGP_LS);
	assert(safi == SAFI_MIDR_LS);
}

static void test_existing_mappings(void)
{
	afi_t afi = AFI_UNSPEC;
	safi_t safi = SAFI_UNSPEC;

	assert(bgp_map_afi_safi_iana2int(IANA_AFI_IPV4, IANA_SAFI_UNICAST, &afi, &safi) == 0);
	assert(afi == AFI_IP);
	assert(safi == SAFI_UNICAST);
	assert(bgp_map_afi_safi_iana2int(IANA_AFI_BGP_LS, IANA_SAFI_BGP_LS, &afi, &safi) == 0);
	assert(afi == AFI_BGP_LS);
	assert(safi == SAFI_BGP_LS);
}

int main(void)
{
	test_midr_mapping();
	test_invalid_pair();
	test_mp_capability_tuple();
	test_existing_mappings();
	puts("MIDR SAFI tests passed");
	return 0;
}
