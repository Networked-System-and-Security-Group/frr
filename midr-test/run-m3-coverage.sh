#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later

set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD_DIR=${MIDR_COVERAGE_BUILD_DIR:-"$ROOT/midr-test/run/m3-coverage"}
CC=${CC:-gcc}
GCOVR=${GCOVR:-gcovr}

if ! command -v "$CC" >/dev/null 2>&1; then
	echo "missing compiler: $CC" >&2
	exit 1
fi
if ! command -v "$GCOVR" >/dev/null 2>&1; then
	echo "missing gcovr; install it with: python3 -m pip install --user gcovr" >&2
	exit 1
fi
if [ ! -f "$ROOT/config.h" ] || [ ! -f "$ROOT/lib/.libs/libfrr.so" ]; then
	echo "build FRR before running M3 coverage" >&2
	exit 1
fi

rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"

compile_object()
{
	source=$1
	output=$2

	"$CC" -DHAVE_CONFIG_H -I"$ROOT" -I"$ROOT/include" -I"$ROOT/lib" \
		-I"$ROOT/lib/assert" -fms-extensions -fno-omit-frame-pointer \
		-fprofile-abs-path --coverage -g -O0 -c "$source" -o "$output"
}

link_test()
{
	output=$1
	shift

	"$CC" --coverage "$@" \
		-L"$ROOT/lib/.libs" -Wl,-rpath,"$ROOT/lib/.libs" \
		-lfrr -lcap -lyang -ljson-c -latomic -lrt -lm -o "$output"
}

compile_object "$ROOT/bgpd/bgp_midr_ls.c" "$BUILD_DIR/bgp_midr_ls.o"
compile_object "$ROOT/bgpd/bgp_midr_sequence.c" "$BUILD_DIR/bgp_midr_sequence.o"
compile_object "$ROOT/bgpd/bgp_midr_cost.c" "$BUILD_DIR/bgp_midr_cost.o"
compile_object "$ROOT/bgpd/bgp_midr_codec.c" "$BUILD_DIR/bgp_midr_codec.o"
compile_object "$ROOT/bgpd/bgp_memory.c" "$BUILD_DIR/bgp_memory.o"
compile_object "$ROOT/tests/bgpd/test_midr_ls_object.c" "$BUILD_DIR/test_midr_ls_object.o"
compile_object "$ROOT/tests/bgpd/test_midr_sequence.c" "$BUILD_DIR/test_midr_sequence.o"
compile_object "$ROOT/tests/bgpd/test_midr_cost.c" "$BUILD_DIR/test_midr_cost.o"
compile_object "$ROOT/tests/bgpd/test_midr_codec.c" "$BUILD_DIR/test_midr_codec.o"

link_test "$BUILD_DIR/test_midr_ls_object" \
	"$BUILD_DIR/test_midr_ls_object.o" "$BUILD_DIR/bgp_midr_ls.o"
link_test "$BUILD_DIR/test_midr_sequence" \
	"$BUILD_DIR/test_midr_sequence.o" "$BUILD_DIR/bgp_midr_sequence.o"
link_test "$BUILD_DIR/test_midr_cost" \
	"$BUILD_DIR/test_midr_cost.o" "$BUILD_DIR/bgp_midr_cost.o"
link_test "$BUILD_DIR/test_midr_codec" \
	"$BUILD_DIR/test_midr_codec.o" "$BUILD_DIR/bgp_midr_codec.o" \
	"$BUILD_DIR/bgp_midr_ls.o" "$BUILD_DIR/bgp_memory.o"

"$BUILD_DIR/test_midr_ls_object"
"$BUILD_DIR/test_midr_sequence"
"$BUILD_DIR/test_midr_cost"
"$BUILD_DIR/test_midr_codec"

"$GCOVR" \
	--root "$ROOT" \
	--object-directory "$BUILD_DIR" \
	--filter 'bgpd/bgp_midr_(ls|sequence|cost|codec)\.c$' \
	--print-summary \
	--fail-under-line 90 \
	--fail-under-branch 80
