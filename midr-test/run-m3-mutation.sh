#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later

set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD_DIR=${MIDR_MUTATION_BUILD_DIR:-"$ROOT/midr-test/run/m3-mutation"}
CC=${CC:-gcc}
KILLED=0

if ! command -v "$CC" >/dev/null 2>&1; then
	echo "missing compiler: $CC" >&2
	exit 1
fi
if [ ! -f "$ROOT/config.h" ] || [ ! -f "$ROOT/lib/.libs/libfrr.so" ]; then
	echo "build FRR before running mutation tests" >&2
	exit 1
fi

rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"

mutate_source()
{
	source_file=$1
	mutant_file=$2
	search=$3
	replacement=$4

	cp "$source_file" "$mutant_file"
	python3 - "$mutant_file" "$search" "$replacement" <<'PY'
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
search = sys.argv[2]
replacement = sys.argv[3]
text = path.read_text()
count = text.count(search)
if count != 1:
    raise SystemExit(f"expected one mutation site, found {count}: {search}")
path.write_text(text.replace(search, replacement, 1))
PY
}

compile_mutant()
{
	component=$1
	mutant=$2
	binary=$3

	case "$component" in
	cost)
		"$CC" -DHAVE_CONFIG_H -I"$ROOT" -I"$ROOT/include" -I"$ROOT/lib" \
			-I"$ROOT/lib/assert" -fms-extensions -g -O0 \
			"$ROOT/tests/bgpd/test_midr_cost.c" "$mutant" \
			-L"$ROOT/lib/.libs" -Wl,-rpath,"$ROOT/lib/.libs" \
			-lfrr -lcap -lyang -ljson-c -latomic -lrt -lm -o "$binary"
		;;
	ls)
		"$CC" -DHAVE_CONFIG_H -I"$ROOT" -I"$ROOT/include" -I"$ROOT/lib" \
			-I"$ROOT/lib/assert" -fms-extensions -g -O0 \
			"$ROOT/tests/bgpd/test_midr_ls_object.c" "$mutant" \
			-L"$ROOT/lib/.libs" -Wl,-rpath,"$ROOT/lib/.libs" \
			-lfrr -lcap -lyang -ljson-c -latomic -lrt -lm -o "$binary"
		;;
	codec)
		"$CC" -DHAVE_CONFIG_H -I"$ROOT" -I"$ROOT/include" -I"$ROOT/lib" \
			-I"$ROOT/lib/assert" -fms-extensions -g -O0 \
			"$ROOT/tests/bgpd/test_midr_codec.c" "$mutant" \
			"$ROOT/bgpd/bgp_midr_ls.c" \
			"$ROOT/bgpd/bgp_memory.c" \
			-L"$ROOT/lib/.libs" -Wl,-rpath,"$ROOT/lib/.libs" \
			-lfrr -lcap -lyang -ljson-c -latomic -lrt -lm -o "$binary"
		;;
	sequence)
		"$CC" -DHAVE_CONFIG_H -I"$ROOT" -I"$ROOT/include" -I"$ROOT/lib" \
			-I"$ROOT/lib/assert" -fms-extensions -g -O0 \
			"$ROOT/tests/bgpd/test_midr_sequence.c" "$mutant" \
			-L"$ROOT/lib/.libs" -Wl,-rpath,"$ROOT/lib/.libs" \
			-lfrr -lcap -lyang -ljson-c -latomic -lrt -lm -o "$binary"
		;;
	*)
		echo "unknown mutation component: $component" >&2
		return 1
		;;
	esac
}

run_mutant()
{
	name=$1
	component=$2
	source_rel=$3
	search=$4
	replacement=$5
	mutant="$BUILD_DIR/$name.c"
	binary="$BUILD_DIR/$name"
	compile_log="$BUILD_DIR/$name.compile.log"
	run_log="$BUILD_DIR/$name.run.log"

	mutate_source "$ROOT/$source_rel" "$mutant" "$search" "$replacement"
	if ! compile_mutant "$component" "$mutant" "$binary" >"$compile_log" 2>&1; then
		echo "invalid mutant (compile failed): $name" >&2
		cat "$compile_log" >&2
		exit 1
	fi
	if "$binary" >"$run_log" 2>&1; then
		echo "survived mutant: $name" >&2
		exit 1
	fi
	KILLED=$((KILLED + 1))
	printf 'killed %-28s (%s)\n' "$name" "$component"
}

run_mutant ceil-floor cost bgpd/bgp_midr_cost.c \
	'dividend % divisor != 0' 'dividend % divisor == 0'
run_mutant loss-denominator cost bgpd/bgp_midr_cost.c \
	'1000000 - metrics->loss_ppm' '1000000 + metrics->loss_ppm'
run_mutant update-threshold cost bgpd/bgp_midr_cost.c \
	'return delta >= threshold;' 'return delta > threshold;'
run_mutant node-prefix-canonical ls bgpd/bgp_midr_ls.c \
	'return midr_ls_prefix_key_is_canonical(&key->u.node_prefix) ? 0 : -EINVAL;' \
	'return midr_ls_prefix_key_is_canonical(&key->u.node_prefix) ? -EINVAL : 0;'
run_mutant full-loss-active ls bgpd/bgp_midr_ls.c \
	'link->metrics.loss_ppm >= 1000000U' 'link->metrics.loss_ppm > 1000000U'
run_mutant tlv-canonical-order codec bgpd/bgp_midr_codec.c \
	'if (offset && type <= previous_type)' \
	'if (false && offset && type <= previous_type)'
run_mutant duplicate-path codec bgpd/bgp_midr_codec.c \
	'if (path->nodes[left] == path->nodes[right])' \
	'if (path->nodes[left] != path->nodes[right])'
run_mutant path-length codec bgpd/bgp_midr_codec.c \
	'if (length != expected_length)' 'if (length == expected_length)'
run_mutant boot-epoch-increment sequence bgpd/bgp_midr_sequence.c \
	'persisted_epoch + 1, 0' 'persisted_epoch, 0'
run_mutant sequence-counter-increment sequence bgpd/bgp_midr_sequence.c \
	'allocator->origin_counter++;' 'allocator->origin_counter += 2;'

echo "MIDR M3 mutation tests passed: $KILLED/10 mutants killed"
