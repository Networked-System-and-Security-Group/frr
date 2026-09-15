#!/usr/bin/env sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

# Includes and types from the standalone public surface must remain protocol
# neutral.  Comments may mention the excluded adapter; source dependencies may
# not.
if grep -RInE '#[[:space:]]*include[[:space:]]+[<"](bgpd|bgp_|zebra/)' \
    "$root"/*.c "$root"/*.h; then
	printf '%s\n' 'standalone boundary scan: forbidden include' >&2
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
#include "midr-prefix-provider.h"
#include "midr-transport.h"
int main(void) { return MIDR_CORE_WIRE_VERSION == 1U ? 0 : 1; }
EOF

$cc $flags -c "$tmp/contract.c" -o "$tmp/contract.o"
printf '%s\n' 'standalone boundary scan: PASS'
