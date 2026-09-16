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
flags='-std=c11 -O2 -Wall -Wextra -Werror -pedantic -I.'
tmp=${TMPDIR:-/tmp}/midrd-boundary.$$
trap 'rm -rf "$tmp"' EXIT INT TERM
mkdir -p "$tmp"
cd "$root"

cat >"$tmp/contract.c" <<'EOF'
#include "midr-core.h"
#include "midr-consumer.h"
#include "midr-engine.h"
#include "midr-local-ipc.h"
#include "midr-local-provider.h"
#include "midr-prefix-ipc.h"
#include "midr-prefix-provider.h"
#include "midr-spf.h"
#include "midr-ted.h"
#include "midr-transport.h"
int main(void) { return MIDR_CORE_WIRE_VERSION == 1U ? 0 : 1; }
EOF

$cc $flags -c "$tmp/contract.c" -o "$tmp/contract.o"
printf '%s\n' 'standalone libfrr boundary scan: PASS'
