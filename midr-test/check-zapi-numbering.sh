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
#   * the working tree appended anything outside the whitelist
#     (ZEBRA_GRE_ADD, ZEBRA_GRE_DELETE, ZEBRA_INTERFACE_ADDRESS_SET,
#     ZEBRA_INTERFACE_ADDRESS_UNSET, ZEBRA_INTERFACE_SET_ADMIN_UP), which
#     must all land at the very end of the enum, or
#   * lib/log.c command_types[] is no longer ordered consistently with the
#     enum, or gained/lost/reordered non-new entries.
#
# Why the baseline is normalised: this branch's baseline (default
# 1adb4c92d0) already carries ZEBRA_GRE_ADD/ZEBRA_GRE_DELETE *inside* the
# enum -- that insertion is the defect being guarded against.  The two GRE
# names are therefore removed from the baseline before the ordinal comparison,
# so that the remaining members are the genuine "pre-existing" set.  A correct
# fix keeps every one of them at its original ordinal and simply appends the
# whitelisted names, and this gate must not false-positive on that.
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
#   midr-test/check-zapi-numbering.sh --self-test
#       Prove the gate still has teeth without touching the working tree: check
#       out HEAD into a scratch worktree, confirm the pristine commit passes,
#       swap ZEBRA_GRE_ADD/ZEBRA_GRE_DELETE in that copy and confirm the gate
#       then fails, then verify the real working tree is unchanged.
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
IFACE_ADDR_SET=ZEBRA_INTERFACE_ADDRESS_SET
IFACE_ADDR_UNSET=ZEBRA_INTERFACE_ADDRESS_UNSET
IFACE_ADMIN_UP=ZEBRA_INTERFACE_SET_ADMIN_UP
# Messages that may legitimately be appended after the baseline enum tail.
NEW_MESSAGES="$GRE_ADD $GRE_DEL $IFACE_ADDR_SET"
NEW_MESSAGES="$NEW_MESSAGES $IFACE_ADDR_UNSET $IFACE_ADMIN_UP"
NEW_COUNT=5

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

# --- self-test ---------------------------------------------------------------
# The gate is only worth trusting if it still rejects a mutation.  Verify that
# in a throwaway worktree so the caller's tree is never touched.
self_test() {
	command -v git >/dev/null 2>&1 || { echo "self-test: git is required" >&2; exit 2; }

	before=$(git status --porcelain)
	scratch=$(mktemp -d "${TMPDIR:-/tmp}/zapi-self-test.XXXXXX")
	wt="$scratch/wt"
	cleanup() {
		git worktree remove --force "$wt" >/dev/null 2>&1 || true
		git worktree prune >/dev/null 2>&1 || true
		rm -rf "$scratch"
	}
	trap cleanup EXIT INT TERM

	if ! git worktree add -f --detach "$wt" HEAD >/dev/null 2>&1; then
		echo "check-zapi-numbering: self-test FAIL: cannot create a scratch worktree" >&2
		exit 1
	fi

	# 1. control: the pristine commit must pass.
	mut_rc=0
	( cd "$wt" && ./midr-test/check-zapi-numbering.sh ) >/dev/null 2>&1 || mut_rc=$?
	if [ "$mut_rc" -ne 0 ]; then
		echo "check-zapi-numbering: self-test FAIL: gate rejects a clean HEAD (rc=$mut_rc)" >&2
		exit 1
	fi

	# 2. swap the two GRE messages in the enum tail; the gate must now fail.
	awk '
		/^[ \t]*ZEBRA_GRE_ADD,$/    { swap = $0; next }
		/^[ \t]*ZEBRA_GRE_DELETE,$/ { print; if (swap != "") { print swap; swap = "" }; next }
		{ print }
	' "$wt/$HEADER" > "$scratch/mutated.h" \
		|| { echo "check-zapi-numbering: self-test FAIL: cannot build the mutation" >&2; exit 1; }
	if cmp -s "$wt/$HEADER" "$scratch/mutated.h"; then
		echo "check-zapi-numbering: self-test FAIL: the mutation did not apply" >&2
		exit 1
	fi
	cp "$scratch/mutated.h" "$wt/$HEADER"

	mut_rc=0
	( cd "$wt" && ./midr-test/check-zapi-numbering.sh ) >/dev/null 2>&1 || mut_rc=$?
	if [ "$mut_rc" -eq 0 ]; then
		echo "check-zapi-numbering: self-test FAIL: gate accepted a swapped enum tail" >&2
		exit 1
	fi

	# 3. the caller's working tree must be byte-for-byte unchanged.
	after=$(git status --porcelain)
	if [ "$before" != "$after" ]; then
		echo "check-zapi-numbering: self-test FAIL: the working tree changed" >&2
		exit 1
	fi

	echo "check-zapi-numbering: self-test PASS (clean HEAD passes, swapped $GRE_ADD/$GRE_DEL fails)"
	exit 0
}

# --- argument handling ------------------------------------------------------
case "${1:-}" in
--dump)
	rev=${2:-HEAD}
	rev_file "$rev" "$HEADER" | enum_dump
	exit 0
	;;
--self-test)
	self_test
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

# Names that are legitimately appended by this branch.  They are excluded
# from the baseline reference and from the ordering/positional comparisons
# below, because their source position in the partial command_types[] table
# cannot be assumed to line up with their newly appended enum ordinal.
printf '%s\n' $NEW_MESSAGES | sort > "$TMP/new.messages"

# Reference = baseline with every appended member stripped and re-indexed so
# that it represents the members that existed before this feature.  On the
# real baseline only the two GRE names are present, so this is equivalent to
# removing exactly those; under the WORKTREE smoke mode all five are removed.
awk -F'\t' 'NR == FNR { skip[$1] = 1; next }
	!($2 in skip) { printf "%d\t%s\n", n, $2; n++ }
' "$TMP/new.messages" "$TMP/base.enum" > "$TMP/ref.enum"

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

# --- assertion 2: only the whitelisted members were appended, at the tail ---
ref_n=$(wc -l < "$TMP/ref.enum" | tr -d ' ')
work_n=$(wc -l < "$TMP/work.enum" | tr -d ' ')
awk -F'\t' 'NR == FNR { ref[$2] = 1; next } !($2 in ref) { print $2 }' \
	"$TMP/ref.enum" "$TMP/work.enum" | sort > "$TMP/extra"
printf '%s\n' $NEW_MESSAGES | sort > "$TMP/extra.ok"
if ! diff -u "$TMP/extra.ok" "$TMP/extra" > "$TMP/extra.diff"; then
	err "working tree appended members outside the whitelist ($NEW_MESSAGES):"
	sed 's/^/    /' "$TMP/extra.diff" >&2
fi
[ "$work_n" -eq $((ref_n + NEW_COUNT)) ] \
	|| err "working-tree enum has $work_n members, expected $((ref_n + NEW_COUNT))"
# Every whitelisted name must land, in order, at the very end of the enum.
expected=$ref_n
for name in $NEW_MESSAGES; do
	ord=$(awk -F'\t' -v n="$name" '$2 == n { print $1 }' "$TMP/work.enum")
	[ "$ord" = "$expected" ] \
		|| err "$name ordinal is $ord, expected $expected (must be appended)"
	expected=$((expected + 1))
done

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

if ! tail -n "$NEW_COUNT" "$TMP/work.desc" \
	| awk -F'\t' '{ print $2 }' > "$TMP/work.desc.tail"; then
	: > "$TMP/work.desc.tail"
fi
printf '%s\n' $NEW_MESSAGES > "$TMP/extra.ok.ordered"
if ! diff -q "$TMP/extra.ok.ordered" "$TMP/work.desc.tail" > /dev/null; then
	err "appended DESC_ENTRY lines are not the tail of command_types[]:"
	diff -u "$TMP/extra.ok.ordered" "$TMP/work.desc.tail" | sed 's/^/    /' >&2
fi

# Positional mismatches ("the Nth DESC_ENTRY must describe the enum value at
# the Nth slot") are compared as a set against the baseline to tolerate the
# pre-existing partial-table gap and to catch any *new* positional breakage.
# Newly appended names are excluded: their appended enum ordinal need not
# equal their source index in the partial table.
awk -F'\t' '$3 != "?" && ($3 + 0) != $1 { print $2 }' "$TMP/work.desc.ord" \
	| sort > "$TMP/work.posmm.raw"
grep -vFxf "$TMP/new.messages" "$TMP/work.posmm.raw" > "$TMP/work.posmm" || true
awk -F'\t' 'NR == FNR { ord[$2] = $1 + 0; next }
	($2 in ord) && (ord[$2] + 0) != $1 { print $2 }' \
	"$TMP/base.enum" "$TMP/base.desc" | sort > "$TMP/base.posmm.raw"
grep -vFxf "$TMP/new.messages" "$TMP/base.posmm.raw" > "$TMP/base.posmm" || true
if ! diff -q "$TMP/base.posmm" "$TMP/work.posmm" > /dev/null; then
	err "command_types[] positional mismatches changed versus baseline (new positional breakage):"
	diff -u "$TMP/base.posmm" "$TMP/work.posmm" | sed 's/^/    /' >&2
else
	posmm_n=$(wc -l < "$TMP/work.posmm" | tr -d ' ')
	echo "note: $ref_n reference enum members; $posmm_n tolerated pre-existing positional gap(s) (unchanged vs baseline, e.g. ZEBRA_INTERFACE_SET_ARP has no DESC_ENTRY)"
fi

# Newly appended DESC_ENTRY entries must not disturb pre-existing ones.
awk -F'\t' 'NR == FNR { skip[$1] = 1; next } !($2 in skip) { print $2 }' \
	"$TMP/new.messages" "$TMP/work.desc" > "$TMP/work.desc.nongre"
awk -F'\t' 'NR == FNR { skip[$1] = 1; next } !($2 in skip) { print $2 }' \
	"$TMP/new.messages" "$TMP/base.desc" > "$TMP/base.desc.nongre"
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
