#!/usr/bin/env bash
# Assert that a midrd binary really contains group 1, and report the exact
# binary the acceptance run used.
#
# midrd.c declares midr_group1_init()/midr_group1_terminate() weak, so a midrd
# linked without midrd/group1 still builds and starts -- it just silently runs
# without group 1.  That makes "the component tests passed" say nothing about
# whether the production binary carries group 1, which is the false pass this
# script exists to prevent.  The two build entries differ on purpose:
#
#   top-level  make midrd/midrd   -> links midrd/group1 (joint acceptance and
#                                    production; the only entry whose result
#                                    may be called a group-1 pass)
#   midrd/Makefile  make test     -> group 2's component tests only; its
#                                    build/midrd has no group 1 by design
#
# usage: verify-group1-linked.sh [binary]   (default: midrd/midrd from the repo root)
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
binary="${1:-$repo_root/midrd/midrd}"

if [ ! -e "$binary" ]; then
	echo "verify-group1-linked: no such binary: $binary" >&2
	echo "verify-group1-linked: build it with 'make midrd/midrd' at the repo root" >&2
	exit 1
fi

# An uninstalled automake/libtool build leaves a wrapper script in place of the
# binary; the ELF object lives next to it in .libs/.
resolved="$binary"
if ! head -c 4 "$resolved" | grep -q $'\x7fELF'; then
	candidate="$(dirname "$resolved")/.libs/$(basename "$resolved")"
	if [ -e "$candidate" ]; then
		resolved="$candidate"
	else
		echo "verify-group1-linked: $binary is not an ELF binary and no .libs/ object was found" >&2
		exit 1
	fi
fi

symbols="$(nm "$resolved")"
missing=0
for symbol in midr_group1_init midr_group1_terminate; do
	# A defined symbol is 'T'/'t'; an unresolved weak reference is 'w'/'U'.
	if ! grep -qE "^[0-9a-fA-F]+ [Tt] $symbol\$" <<<"$symbols"; then
		echo "verify-group1-linked: FAIL $symbol is not defined in this binary" >&2
		missing=1
	fi
done
if [ "$missing" -ne 0 ]; then
	echo "verify-group1-linked: this midrd was linked WITHOUT group 1;" >&2
	echo "verify-group1-linked: build the joint/production entry: make midrd/midrd" >&2
	exit 1
fi

# The acceptance log must be able to name what was actually exercised.
echo "verify-group1-linked: build entry  = top-level make midrd/midrd"
echo "verify-group1-linked: binary       = $binary"
echo "verify-group1-linked: ELF object   = $resolved"
echo "verify-group1-linked: group 1 linked in: PASS"
