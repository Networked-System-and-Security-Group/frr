#!/usr/bin/env python3
import importlib.util
import ipaddress
from pathlib import Path
import sys
import unittest

spec = importlib.util.spec_from_file_location(
    "bgp_prefix_provider",
    Path(__file__).resolve().parent.parent / "node" / "midr-bgp-prefix-provider.py")
provider = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = provider
spec.loader.exec_module(provider)


class PrefixPolicyTest(unittest.TestCase):
    def route(self, *paths, prefix="2001:db8::/48", limit=1):
        return provider.parse_bgp_json({"routes": {prefix: list(paths)}}, set(), limit)

    def path(self, value, **attributes):
        return {"path": value, "valid": True, "bestpath": True, **attributes}

    def test_default_limit(self):
        for value, exported in [("", True), ("Local", True),
                                ("4200000006", True),
                                ("4200000006 4200000003", False),
                                ("4200000006 4200000006", False)]:
            with self.subTest(value=value):
                self.assertEqual(bool(self.route(self.path(value))), exported)

    def test_structured_detail_and_nested_bestpath(self):
        for length, exported in [(0, True), (1, True), (2, False)]:
            with self.subTest(length=length):
                path = {"aspath": {"length": length}, "valid": True,
                        "bestpath": {"overall": True}}
                result = provider.parse_bgp_json(
                    {"2001:db8::/48": {"paths": [path]}}, set())
                self.assertEqual(bool(result), exported)

    def test_set_confederation_and_asdot(self):
        for value, length in [("{64501,64502}", 1),
                              ("64500 {64501,64502}", 2),
                              ("(64501 64502) 64503", 1),
                              ("[64501,64502] 64503", 1),
                              ("64086.59910", 1)]:
            with self.subTest(value=value):
                self.assertEqual(provider.as_path_length({"path": value}), length)
        self.assertEqual(provider.as_path_length({"aspath": {"segments": [
            {"type": "as-confed-sequence", "list": [64501, 64502]},
            {"type": "as-set", "list": [64503, 64504]}]}}), 1)

    def test_missing_or_malformed_path_is_rejected(self):
        for path in [{"valid": True, "bestpath": True},
                     self.path("unknown"), self.path("{}"),
                     {"aspath": {"length": -1}, "valid": True},
                     {"aspath": {"length": True}, "valid": True},
                     {"aspath": {"segments": None}, "valid": True},
                     {"aspath": {"segments": [{"type": "unknown", "list": []}]},
                      "valid": True}]:
            with self.subTest(path=path):
                self.assertFalse(self.route(path))

    def test_filter_selected_paths_only(self):
        self.assertFalse(self.route(self.path("64501 64502"),
                                    self.path("64503", bestpath=False)))
        result = self.route(self.path("64501 64502", metric=1),
                            self.path("64503", bestpath=False, multipath=True, metric=20))
        self.assertEqual(result["2001:db8::/48"].metric, 20)
        result = self.route(self.path("64501 64502", bestpath={"overall": False}),
                            self.path("64503", bestpath={"overall": True}))
        self.assertTrue(result)

    def test_limit_and_existing_filters(self):
        self.assertTrue(self.route(self.path("64501 64502"), limit=2))
        self.assertFalse(self.route(self.path("64501"), limit=0))
        self.assertTrue(self.route(self.path(""), limit=0))
        for flag in ("stale", "withdrawn", "removed", "inactive", "invalid"):
            self.assertFalse(self.route(self.path("64501", **{flag: True})))
        self.assertFalse(self.route(self.path("64501", valid=False)))
        self.assertFalse(provider.parse_bgp_json(
            {"2001:db8::/48": {"paths": [self.path("64501")]}},
            {ipaddress.ip_network("2001:db8::/48")}))

    def test_policy_transition_withdraws_disallowed_prefix(self):
        args = type("Args", (), {})()
        instance = provider.Provider(args, set())
        instance.current = self.route(self.path("64501 64502"), limit=2)
        events = []
        instance.send = lambda kind, prefix=None: events.append((kind, str(prefix.network)))
        instance.publish_changes(self.route(self.path("64501 64502")))
        self.assertEqual(events, [(provider.WITHDRAW, "2001:db8::/48")])


if __name__ == "__main__":
    unittest.main()
