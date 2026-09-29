# ---------------------------------------------------------------------------
# midr-test/Makefile - entry point for the MIDR tests.
#
# The component tests, the acceptance scripts and the boundary gate live in this
# directory, but their build rules stay in midrd/Makefile: the tests link the
# daemon's object files (e.g. midrd-virtual-link-test links midr-virtual-link.o)
# and share one BUILD_DIR with the daemon build.  These targets forward to that
# makefile so both entry points work, with BUILD_DIR resolved relative to midrd/:
#
#   cd midrd     && make -j8 BUILD_DIR=/tmp/midrd-build test
#   cd midr-test && make -j8 BUILD_DIR=/tmp/midrd-build test
#
# The host-run acceptance targets (fib-smoke, integration-smoke, gre-smoke,
# stagee-smoke) are forwarded too; they must run on the docker HOST as root and
# are documented in ../midrd/Makefile.
# ---------------------------------------------------------------------------
MIDRD_DIR ?= ../midrd
BUILD_DIR ?= build

FORWARD = all test boundary clean install native-smoke hardening-smoke \
	  fib-smoke integration-smoke gre-smoke stagee-smoke

.PHONY: $(FORWARD)

$(FORWARD):
	$(MAKE) -C $(MIDRD_DIR) BUILD_DIR=$(BUILD_DIR) $@
