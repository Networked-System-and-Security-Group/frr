#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec python3 "$SCRIPT_DIR/run-ted-fixtures.py" "$@"
