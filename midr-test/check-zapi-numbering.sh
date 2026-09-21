#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# check-zapi-numbering.sh -- ZAPI message-number stability regression gate.
#
# ZEBRA_* message numbers are an inter-daemon wire ABI: every number that has
# ever been shipped to a peer must keep its ordinal forever.  Only brand-new
# messages may be *appended* after the last member of enum
# zebra_message_types (lib/zclient.h), and the matching entries in
# lib/log.c command_types[] must stay ordered consistently with the enum.
#
# This gate fails if:
#   * a pre-existing member of the enum changed ordinal, or
#   * the working tree appended anything other than ZEBRA_GRE_ADD and
#     ZEBRA_GRE_DELETE (which must land at the very end of the enum), or
#   * lib/log.c command_types[] is no longer ordered consistently with the
#     enum, or gained/lost/reordered non-GRE entries.
#
# Why the baseline is normalised: this branch's baseline (default
# 1adb4c92d0) already carries ZEBRA_GRE_ADD/ZEBRA_GRE_DELETE *inside* the
# enum -- that insertion is the defect being guarded against.  The two GRE
# names are therefore removed from the baseline before the ordinal comparison,
# so that the remaining members are the genuine "pre-existing" set.  A correct
# fix keeps every one of them at its original ordinal and simply appends the
# two GRE names, and this gate must not false-positive on that.
#
# On the command_types[] positional check: the table is built with designated
# initialisers `[(T)] = {...}`, so array[T] is correct regardless of source
# order.  What we can and do assert is that the *source order* still follows
# the enum's numeric order -- i.e. the Nth DESC_ENTRY describes the enum value
# at the Nth slot of the enum sequence restricted to the members the table
# lists.  The table is intentionally partial (ZEBRA_INTERFACE_SET_ARP has no
# DESC_ENTRY), so a literal "ordinal == index" does not hold even on the
# clean baseline; we therefore require the set of positional mismatches to be
# *unchanged* versus the baseline and fail on any newly introduced mismatch.
#
# Usage:
#   midr-test/check-zapi-numbering.sh [BASELINE_REV]
#       Verify the working tree; BASELINE_REV defaults to 1adb4c92d0.
#       Pass WORKTREE to compare the working tree against itself (smoke).
#   midr-test/check-zapi-numbering.sh --dump [REV]
#       Print "ordinal<TAB>name" for REV (default HEAD) and exit.
#
# Set ZAPI_NUMBERING_VERBOSE=1 to also print the full enum dumps and the
# baseline-vs-working-tree diff (diagnostics for a failing run).
#
set -eu

ROOT=$(git rev-parse --show-toplevel)
cd "$ROOT"

HEADER=lib/zclient.h
LOGFILE=lib/log.c
DEFAULT_BASELINE=1adb4c92d0
GRE_ADD=ZEBRA_GRE_ADD
GRE_DEL=ZEBRA_GRE_DELETE

usage() {
	sed -n '2,8p;/^# Usage:/,/^#$/p' "$0" | sed 's/^# \{0,1\}//'
}

# --- enum parsing -----------------------------------------------------------
# Emit "ordinal<TAB>name" for the "typedef enum { ... } zebra_message_types_t"
# block read from stdin.  State is reset on every "typedef enum {", so an
# earlier unrelated enum in the same header cannot contaminate the output.
enum_dump() {
	awk '
		function emit(   i) {
			for (i = 0; i < cnt; i++)
				printf "%d\t%s\n", i, names[i];
		}
		/typedef enum \{/ { cnt = 0; inblk = 1; next }
		inblk && /^} / {
			if ($0 ~ /} zebra_message_types_t;/) {
				emit();
				inblk = 0;
				exit;
			}
			inblk = 0;
			next;
		}
		inblk {
			line = $0;
			sub(/\/\/.*/, "", line);
			gsub(/,/, "", line);
			gsub(/^[ \t]+/, "", line);
			gsub(/[ \t]+$/, "", line);
			if (line ~ /^[A-Za-z_][A-Za-z0-9_]*$/)
				names[cnt++] = line;
		}
	'
}

# Emit "index<TAB>name" for command_types[] in lib/log.c read from stdin.
desc_dump() {
	sed -n '/static const struct zebra_desc_table command_types\[\] = {/,/^};/p' \
		| grep -oE 'DESC_ENTRY\([A-Z0-9_]+\)' \
		| sed -E 's/DESC_ENTRY\(([A-Z0-9_]+)\)/\1/' \
		| awk '{ printf "%d\t%s\n", NR - 1, $0 }'
}

# Read a file at a revision ("WORKTREE" means the on-disk file).
rev_file() { # $1=rev $2=path
	case "$1" in
	WORKTREE|worktree) cat "$2" ;;
	*) git show "$1:$2" ;;
	esac
}

# --- argument handling ------------------------------------------------------
case "${1:-}" in
--dump)
	rev=${2:-HEAD}
	rev_file "$rev" "$HEADER" | enum_dump
	exit 0
	;;
-h|--help)
	usage
	exit 0
	;;
esac

BASELINE=${1:-$DEFAULT_BASELINE}
if [ "$BASELINE" != WORKTREE ] && [ "$BASELINE" != worktree ]; then
	git rev-parse --verify --quiet "${BASELINE}^{commit}" >/dev/null \
		|| { echo "check-zapi-numbering: unknown baseline revision: $BASELINE" >&2; exit 2; }
fi

TMP=$(mktemp -d "${TMPDIR:-/tmp}/zapi-numbering.XXXXXX")
trap 'rm -rf "$TMP"' EXIT

fail=0
err() { echo "check-zapi-numbering: FAIL: $*" >&2; fail=1; }

rev_file "$BASELINE" "$HEADER" | enum_dump > "$TMP/base.enum"
enum_dump < "$HEADER" > "$TMP/work.enum"

# Reference = baseline with the two GRE members stripped and re-indexed so
# that it represents the members that existed before the GRE feature.
awk -F'\t' -v a="$GRE_ADD" -v d="$GRE_DEL" '
	$2 == a || $2 == d { next }
	{ printf "%d\t%s\n", n, $2; n++ }
' "$TMP/base.enum" > "$TMP/ref.enum"

# Full dumps are diagnostics for a failing run; keep the gate quiet by default.
if [ -n "${ZAPI_NUMBERING_VERBOSE:-}" ]; then
	echo "== baseline enum ($BASELINE) =="
	cat "$TMP/base.enum"
	echo "== working-tree enum =="
	cat "$TMP/work.enum"
	echo "== diff (baseline vs working tree; informational) =="
	diff -u "$TMP/base.enum" "$TMP/work.enum" || true
fi

# --- assertion 1: no pre-existing member changed ordinal --------------------
while IFS="$(printf '\t')" read -r ord name; do
	[ -n "$name" ] || continue
	work_ord=$(awk -F'\t' -v n="$name" '$2 == n { print $1; found = 1 } END { exit !found }' "$TMP/work.enum") || {
		err "pre-existing member $name is missing from the working-tree enum"
		continue
	}
	[ "$ord" = "$work_ord" ] \
		|| err "pre-existing member $name changed ordinal: baseline=$ord working=$work_ord"
done < "$TMP/ref.enum"

# --- assertion 2: only the two GRE members were appended, at the tail -------
ref_n=$(wc -l < "$TMP/ref.enum" | tr -d ' ')
work_n=$(wc -l < "$TMP/work.enum" | tr -d ' ')
awk -F'\t' 'NR == FNR { ref[$2] = 1; next } !($2 in ref) { print $2 }' \
	"$TMP/ref.enum" "$TMP/work.enum" | sort > "$TMP/extra"
printf '%s\n%s\n' "$GRE_ADD" "$GRE_DEL" | sort > "$TMP/extra.ok"
if ! diff -u "$TMP/extra.ok" "$TMP/extra" > "$TMP/extra.diff"; then
	err "working tree appended members other than $GRE_ADD/$GRE_DEL:"
	sed 's/^/    /' "$TMP/extra.diff" >&2
fi
[ "$work_n" -eq $((ref_n + 2)) ] \
	|| err "working-tree enum has $work_n members, expected $((ref_n + 2))"
gre_add_ord=$(awk -F'\t' -v n="$GRE_ADD" '$2 == n { print $1 }' "$TMP/work.enum")
gre_del_ord=$(awk -F'\t' -v n="$GRE_DEL" '$2 == n { print $1 }' "$TMP/work.enum")
[ "$gre_add_ord" = "$ref_n" ] \
	|| err "$GRE_ADD ordinal is $gre_add_ord, expected $ref_n (must be appended)"
[ "$gre_del_ord" = "$((ref_n + 1))" ] \
	|| err "$GRE_DEL ordinal is $gre_del_ord, expected $((ref_n + 1)) (must be appended)"

# --- assertion 3: command_types[] still mirrors the enum --------------------
rev_file "$BASELINE" "$LOGFILE" | desc_dump > "$TMP/base.desc"
desc_dump < "$LOGFILE" > "$TMP/work.desc"

# Attach the working-tree enum ordinal to every DESC_ENTRY.
awk -F'\t' 'NR == FNR { ord[$2] = $1 + 0; next }
	{ print $1 "\t" $2 "\t" (($2 in ord) ? ord[$2] : "?") }' \
	"$TMP/work.enum" "$TMP/work.desc" > "$TMP/work.desc.ord"

unknown=$(awk -F'\t' '$3 == "?" { print $2 }' "$TMP/work.desc.ord" | tr '\n' ' ')
[ -z "$unknown" ] || err "command_types[] names not present in the enum: $unknown"

if ! awk -F'\t' 'BEGIN { bad = 0; p = -1 }
	$3 != "?" { o = $3 + 0; if (o <= p) bad = 1; p = o }
	END { exit bad }' "$TMP/work.desc.ord"; then
	err "command_types[] source order does not follow the enum ordinal order"
fi

if ! tail -n 2 "$TMP/work.desc" | awk -F'\t' -v a="$GRE_ADD" -v d="$GRE_DEL" \
	'{ n[NR] = $2 } END { exit !(NR == 2 && n[1] == a && n[2] == d) }'; then
	err "$GRE_ADD/$GRE_DEL DESC_ENTRY lines are not the appended tail of command_types[]"
fi

# Positional mismatches ("the Nth DESC_ENTRY must describe the enum value at
# the Nth slot") are compared as a set against the baseline to tolerate the
# pre-existing partial-table gap and to catch any *new* positional breakage.
awk -F'\t' '$3 != "?" && ($3 + 0) != $1 { print $2 }' "$TMP/work.desc.ord" | sort > "$TMP/work.posmm"
awk -F'\t' 'NR == FNR { ord[$2] = $1 + 0; next }
	($2 in ord) && (ord[$2] + 0) != $1 { print $2 }' \
	"$TMP/base.enum" "$TMP/base.desc" | sort > "$TMP/base.posmm"
if ! diff -q "$TMP/base.posmm" "$TMP/work.posmm" > /dev/null; then
	err "command_types[] positional mismatches changed versus baseline (new positional breakage):"
	diff -u "$TMP/base.posmm" "$TMP/work.posmm" | sed 's/^/    /' >&2
else
	posmm_n=$(wc -l < "$TMP/work.posmm" | tr -d ' ')
	echo "note: $ref_n reference enum members; $posmm_n tolerated pre-existing positional gap(s) (unchanged vs baseline, e.g. ZEBRA_INTERFACE_SET_ARP has no DESC_ENTRY)"
fi

# Non-GRE DESC_ENTRY entries must not be reordered.
awk -F'\t' -v a="$GRE_ADD" -v d="$GRE_DEL" '$2 != a && $2 != d { print $2 }' "$TMP/work.desc" > "$TMP/work.desc.nongre"
awk -F'\t' -v a="$GRE_ADD" -v d="$GRE_DEL" '$2 != a && $2 != d { print $2 }' "$TMP/base.desc" > "$TMP/base.desc.nongre"
if ! diff -q "$TMP/base.desc.nongre" "$TMP/work.desc.nongre" > /dev/null; then
	err "pre-existing command_types[] entries were reordered versus baseline:"
	diff -u "$TMP/base.desc.nongre" "$TMP/work.desc.nongre" | sed 's/^/    /' >&2
fi

# --- assertion 4: the dispatch/encoding tables stay index-safe --------------
# Review 2.1 asks for the *indexed* tables to be checked as well, not only the
# enum and lib/log.c.  zebra/zapi_msg.c's zserv_handlers[] must stay built with
# designated initialisers keyed by the enum member, so appending an enum member
# can never move an existing handler (a positional table would silently shift
# every later handler when the enum grows).  The two GRE handlers must be
# dispatched, and lib/zclient.c must never encode a message type as a numeric
# literal.
MSG_TABLE=zebra/zapi_msg.c
ZCLIENT_C=lib/zclient.c

awk '/zserv_handlers\[\]\)\(ZAPI_HANDLER_ARGS\) = \{/,/^\};/' "$MSG_TABLE" \
	| tail -n +2 > "$TMP/handlers.txt"
if [ ! -s "$TMP/handlers.txt" ]; then
	err "could not locate zserv_handlers[] in $MSG_TABLE"
else
	# Every non-empty, non-comment line of the table body must be a
	# designated initialiser "[ENUM_MEMBER] = handler,".  Anything else --
	# a bare handler name (positional table), a numeric key, or a bracketed
	# numeric key -- means the table would shift when the enum grows.
	grep -vE '^[[:space:]]*($|/\*|\*|//|#)' "$TMP/handlers.txt" \
		| grep -vE '^\};[[:space:]]*$' \
		| grep -vE '^[[:space:]]*\[[A-Z][A-Z0-9_]*\][[:space:]]*=' \
		> "$TMP/handlers.pos" || true
	if [ -s "$TMP/handlers.pos" ]; then
		err "zserv_handlers[] has positional or numerically keyed entries:"
		sed 's/^/    /' "$TMP/handlers.pos" >&2
	fi
	grep -qE "\[$GRE_ADD\][[:space:]]*=" "$TMP/handlers.txt" \
		|| err "zserv_handlers[] has no dispatch entry for $GRE_ADD"
	grep -qE "\[$GRE_DEL\][[:space:]]*=" "$TMP/handlers.txt" \
		|| err "zserv_handlers[] has no dispatch entry for $GRE_DEL"
fi

if grep -nE 'zclient_create_header\([^,]+, *[0-9]' "$ZCLIENT_C" > "$TMP/zc.num"; then
	err "$ZCLIENT_C encodes a ZAPI message type as a numeric literal:"
	sed 's/^/    /' "$TMP/zc.num" >&2
fi

# --- assertion 5: the shared (group 2) common branch is synced --------------
# Review 4 allows the working branch to carry the shared commits as
# cherry-picks, so equivalence is checked by commit subject rather than by
# ancestry: every commit of the shared branch that is not an ancestor of HEAD
# must have a counterpart with the same subject in HEAD.  Override the ref with
# MIDR_COMMON_REF; the check is skipped when the ref is not fetched.
SYNC_REF=${MIDR_COMMON_REF:-origin/fix/midrd-integration-hardening}
if git rev-parse --verify --quiet "${SYNC_REF}^{commit}" >/dev/null 2>&1; then
	: > "$TMP/sync.missing"
	git log --format=%H "${SYNC_REF}" --not HEAD > "$TMP/sync.missing" \
		2>/dev/null || true
	git log --format=%s HEAD > "$TMP/sync.subjects"
	sync_missing=0
	while read -r sha; do
		[ -n "$sha" ] || continue
		subject=$(git log -1 --format=%s "$sha")
		if grep -Fxq -- "$subject" "$TMP/sync.subjects"; then
			echo "note: $SYNC_REF $sha is present in HEAD: $subject"
		else
			err "$SYNC_REF commit $sha is missing from HEAD: $subject"
			sync_missing=$((sync_missing + 1))
		fi
	done < "$TMP/sync.missing"
	[ "$sync_missing" -eq 0 ] \
		|| echo "note: $sync_missing shared-branch commit(s) need syncing" >&2
else
	echo "note: $SYNC_REF not fetched; shared-baseline sync check skipped"
fi

if [ "$fail" -ne 0 ]; then
	echo "check-zapi-numbering: FAIL" >&2
	exit 1
fi

echo "check-zapi-numbering: PASS (baseline $BASELINE vs working tree)"
