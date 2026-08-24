#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later

set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD_DIR=${MIDR_FUZZ_BUILD_DIR:-"$ROOT/midr-test/run/m3-fuzz"}
CORPUS_DIR="$BUILD_DIR/corpus"
FUZZ_TIME=${MIDR_FUZZ_TIME:-60}
CLANG=${CLANG:-clang}
CXX=${CXX:-g++}

if ! command -v "$CLANG" >/dev/null 2>&1; then
	echo "missing compiler: $CLANG" >&2
	exit 1
fi
if ! command -v "$CXX" >/dev/null 2>&1; then
	echo "missing C++ runtime locator: $CXX" >&2
	exit 1
fi
if ! command -v xxd >/dev/null 2>&1; then
	echo "missing corpus converter: xxd" >&2
	exit 1
fi
if [ ! -f "$ROOT/config.h" ] || [ ! -f "$ROOT/lib/.libs/libfrr.so" ]; then
	echo "build FRR before running the codec fuzzer" >&2
	exit 1
fi

rm -rf "$BUILD_DIR"
mkdir -p "$CORPUS_DIR"

for seed in "$ROOT"/midr-test/fuzz/corpus/*.hex; do
	name=$(basename "$seed" .hex)
	tr -d '[:space:]' <"$seed" | xxd -r -p >"$CORPUS_DIR/$name"
done

"$CLANG" \
	-DHAVE_CONFIG_H \
	-I"$ROOT" -I"$ROOT/include" -I"$ROOT/lib" -I"$ROOT/lib/assert" \
	-fms-extensions -fno-omit-frame-pointer -g -O1 \
	-fsanitize=fuzzer,address,undefined \
	"$ROOT/tests/bgpd/fuzz_midr_codec.c" \
	"$ROOT/bgpd/bgp_midr_codec.c" \
	"$ROOT/bgpd/bgp_midr_ls.c" \
	"$ROOT/bgpd/bgp_memory.c" \
	-L"$(dirname -- "$("$CXX" -print-file-name=libstdc++.so)")" \
	-L"$ROOT/lib/.libs" -Wl,-rpath,"$ROOT/lib/.libs" \
	-lfrr -lcap -lyang -ljson-c -latomic -lrt -lm \
	-o "$BUILD_DIR/fuzz_midr_codec"

ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:quarantine_size_mb=64 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
	"$BUILD_DIR/fuzz_midr_codec" "$CORPUS_DIR" \
	-max_len=4096 -max_total_time="$FUZZ_TIME" -timeout=5 -rss_limit_mb=1024
