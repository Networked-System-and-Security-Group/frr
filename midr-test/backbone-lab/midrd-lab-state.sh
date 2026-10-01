#!/usr/bin/env bash

midr_lab_self_field()
{
	printf '%s\n' "$1" | awk -F: -v key="$2" \
		'$1 ~ key {sub(/^[^:]*: */, ""); print; exit}'
}

midr_lab_z2_group()
{
	local self=$1 log=$2 group configured recommendation representative

	group=$(midr_lab_self_field "$self" '^Group-ID')
	configured=$(midr_lab_self_field "$self" '^Configured gid')
	case "$group" in
		1) representative=10.0.0.111 ;;
		2) representative=10.0.0.121 ;;
		*) return 1 ;;
	esac
	[ "$configured" = '-' ] || return 1
	recommendation=$(printf '%s\n' "$log" |
		grep 'REP_PROBE_DONE → RECOMMEND' | tail -n 1)
	printf '%s\n' "$recommendation" |
		grep -Fq "RECOMMEND 群 $group 代表 $representative（" || return 1
	printf '%s\n' "$group"
}

midr_lab_tier1_blocks()
{
	local admission=$1 target=$2

	printf '%s\n' "$admission" | awk -v target="$target:" '
		/^MIDR avoid-tier1: enabled/ { enabled = 1 }
		$3 == target && $4 == "BLOCKED_TIER1" { blocked = 1 }
		END { exit !(enabled && blocked) }'
}

midr_lab_route_is_gre()
{
	local route=$1 fib=$2

	[ -n "$fib" ] && printf '%s\n' "$route" |
		grep -Eq ' dev mgre-[0-9a-f]{8}([[:space:]]|$)'
}
