#!/usr/bin/env sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

# The daemon may use common libfrr facilities, but it must not regain a
# dependency on bgpd, the zebra daemon internals, or BGP protocol types.
if grep -RInE '#[[:space:]]*include[[:space:]]+[<"](bgpd|bgp_|zebra/)' \
    "$root"/*.c "$root"/*.h; then
	printf '%s\n' 'standalone boundary scan: forbidden include' >&2
	exit 1
fi
if grep -RInE '\b(struct[[:space:]]+(bgp|peer|bgp_path_info)|AFI_BGP|SAFI_MIDR_LS|BGP_(OPEN|UPDATE|FSM|CAPABILITY))\b' \
    "$root"/*.c "$root"/*.h; then
	printf '%s\n' 'standalone boundary scan: forbidden BGP dependency' >&2
	exit 1
fi

cc=${CC:-cc}
flags='-std=gnu11 -O2 -Wall -Wextra -Werror -Wno-pedantic -Wno-missing-field-initializers -Wno-unused-parameter -DHAVE_CONFIG_H -I. -I.. -I../lib'
# The third-group headers pull libfrr's vrf.h -> vty.h chain, whose anonymous
# struct members trip GCC's default-on "declaration does not declare anything"
# warning; that warning has no -W option, so -Werror cannot be kept here.
libfrr_flags='-std=gnu11 -O2 -Wall -Wextra -Wno-pedantic -Wno-missing-field-initializers -Wno-unused-parameter -DHAVE_CONFIG_H -I. -I.. -I../lib'
tmp=${TMPDIR:-/tmp}/midrd-boundary.$$
trap 'rm -rf "$tmp"' EXIT INT TERM
mkdir -p "$tmp"
cd "$root"

cat >"$tmp/contract.c" <<'EOF'
#include "midr-context.h"
#include "midr-core.h"
#include "midr-consumer.h"
#include "midr-engine.h"
#include "midr-local-ipc.h"
#include "midr-local-provider.h"
#include "midr-prefix-ipc.h"
#include "midr-prefix-provider.h"
#include "midr-session.h"
#include "midr-spf.h"
#include "midr-spf-install.h"
#include "midr-ted.h"
#include "midr-topology.h"
#include "midr-transport.h"
#include "midr-zebra.h"
int main(void) { return MIDR_CORE_WIRE_VERSION == 1U ? 0 : 1; }
EOF

$cc $flags -c "$tmp/contract.c" -o "$tmp/contract.o"

cat >"$tmp/dp-contract.c" <<'EOF'
#include "midr-dp-backend.h"
#include "midr-gre.h"
#include "midr-spf-install.h"
#include "midr-zebra.h"
int main(void) { return MIDR_SRV6_MAX_SEGS == 8 ? 0 : 1; }
EOF

$cc $libfrr_flags -c "$tmp/dp-contract.c" -o "$tmp/dp-contract.o"
printf '%s\n' 'standalone libfrr boundary scan: PASS'
