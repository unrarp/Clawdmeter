"""
Behavior-pinning tests for claude_usage_daemon.py.

Pure-function / no-network tests only. Run with the daemon venv:
    /home/rarp/workspace/clawdmeter/daemon/.venv/bin/python test_daemon.py -v
"""

import os
import sys
import unittest
from pathlib import Path
from unittest.mock import patch, MagicMock, AsyncMock

import httpx

# Self-bootstrap: ensure the daemon module is importable regardless of cwd.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import claude_usage_daemon as d


WIRE_KEYS = frozenset(
    {"s", "sr", "w", "wr", "st", "ok", "sp", "cp", "cs", "csr", "cw", "cwr", "cst", "cok"}
)

CLAUDE_FRESH = {"s": 42, "sr": 100, "w": 10, "wr": 5000, "st": "allowed"}
CODEX_FRESH  = {"cs": 55, "csr": 60, "cw": 20, "cwr": 10080, "cst": "allowed"}


def _reset():
    """Reset all mutable module-level state between tests."""
    d._claude_last = None
    d._codex_last  = None
    d._claude_seen = False


class TestBuildPayloadKeys(unittest.TestCase):
    """1. build_payload always returns exactly the 14 wire keys."""

    def setUp(self):
        _reset()

    def test_both_present_fresh(self):
        p = d.build_payload(sp=True, cp=True,
                            claude_fresh=CLAUDE_FRESH, codex_fresh=CODEX_FRESH)
        self.assertEqual(set(p.keys()), WIRE_KEYS)

    def test_both_absent(self):
        p = d.build_payload(sp=False, cp=False, claude_fresh=None, codex_fresh=None)
        self.assertEqual(set(p.keys()), WIRE_KEYS)

    def test_only_claude_present(self):
        p = d.build_payload(sp=True, cp=False,
                            claude_fresh=CLAUDE_FRESH, codex_fresh=None)
        self.assertEqual(set(p.keys()), WIRE_KEYS)

    def test_only_codex_present(self):
        p = d.build_payload(sp=False, cp=True,
                            claude_fresh=None, codex_fresh=CODEX_FRESH)
        self.assertEqual(set(p.keys()), WIRE_KEYS)

    def test_both_present_no_fresh(self):
        p = d.build_payload(sp=True, cp=True, claude_fresh=None, codex_fresh=None)
        self.assertEqual(set(p.keys()), WIRE_KEYS)


class TestAbsentProviderSentinels(unittest.TestCase):
    """2. Absent provider → -1 sentinels, correct st/cst, ok/cok False, cache cleared."""

    def setUp(self):
        _reset()

    # --- Claude absent ---

    def test_claude_absent_sentinels(self):
        p = d.build_payload(sp=False, cp=False, claude_fresh=None, codex_fresh=None)
        self.assertEqual(p["s"],  -1)
        self.assertEqual(p["sr"], -1)
        self.assertEqual(p["w"],  -1)
        self.assertEqual(p["wr"], -1)
        self.assertEqual(p["st"], "allowed")
        self.assertFalse(p["ok"])

    def test_claude_absent_clears_cache(self):
        # Pre-populate the last-good cache, then call with sp=False.
        d._claude_last = CLAUDE_FRESH.copy()
        d.build_payload(sp=False, cp=False, claude_fresh=None, codex_fresh=None)
        self.assertIsNone(d._claude_last,
                          "cache must be cleared when Claude is absent (re-add rule)")

    # --- Codex absent ---

    def test_codex_absent_sentinels(self):
        p = d.build_payload(sp=False, cp=False, claude_fresh=None, codex_fresh=None)
        self.assertEqual(p["cs"],  -1)
        self.assertEqual(p["csr"], -1)
        self.assertEqual(p["cw"],  -1)
        self.assertEqual(p["cwr"], -1)
        self.assertEqual(p["cst"], "unknown")
        self.assertFalse(p["cok"])

    def test_codex_absent_clears_cache(self):
        d._codex_last = CODEX_FRESH.copy()
        d.build_payload(sp=False, cp=False, claude_fresh=None, codex_fresh=None)
        self.assertIsNone(d._codex_last,
                          "cache must be cleared when Codex is absent (re-add rule)")

    def test_codex_absent_via_cp_false_clears_cache(self):
        # More explicit: sp=True but cp=False
        d._codex_last = CODEX_FRESH.copy()
        d.build_payload(sp=True, cp=False, claude_fresh=CLAUDE_FRESH, codex_fresh=None)
        self.assertIsNone(d._codex_last)

    def test_claude_absent_via_sp_false_clears_cache(self):
        d._claude_last = CLAUDE_FRESH.copy()
        d.build_payload(sp=False, cp=True, claude_fresh=None, codex_fresh=CODEX_FRESH)
        self.assertIsNone(d._claude_last)

    # --- Presence flags pass through ---

    def test_sp_cp_flags_in_payload(self):
        p = d.build_payload(sp=True, cp=False, claude_fresh=CLAUDE_FRESH, codex_fresh=None)
        self.assertTrue(p["sp"])
        self.assertFalse(p["cp"])

        _reset()
        p2 = d.build_payload(sp=False, cp=True, claude_fresh=None, codex_fresh=CODEX_FRESH)
        self.assertFalse(p2["sp"])
        self.assertTrue(p2["cp"])


class TestPresentFresh(unittest.TestCase):
    """3. Present + fresh → correct numbers, ok/cok True, cache updated."""

    def setUp(self):
        _reset()

    def test_claude_fresh_values(self):
        p = d.build_payload(sp=True, cp=False,
                            claude_fresh=CLAUDE_FRESH, codex_fresh=None)
        self.assertEqual(p["s"],  CLAUDE_FRESH["s"])
        self.assertEqual(p["sr"], CLAUDE_FRESH["sr"])
        self.assertEqual(p["w"],  CLAUDE_FRESH["w"])
        self.assertEqual(p["wr"], CLAUDE_FRESH["wr"])
        self.assertEqual(p["st"], CLAUDE_FRESH["st"])
        self.assertTrue(p["ok"])

    def test_claude_fresh_updates_cache(self):
        d.build_payload(sp=True, cp=False, claude_fresh=CLAUDE_FRESH, codex_fresh=None)
        self.assertEqual(d._claude_last, CLAUDE_FRESH)

    def test_codex_fresh_values(self):
        p = d.build_payload(sp=False, cp=True,
                            claude_fresh=None, codex_fresh=CODEX_FRESH)
        self.assertEqual(p["cs"],  CODEX_FRESH["cs"])
        self.assertEqual(p["csr"], CODEX_FRESH["csr"])
        self.assertEqual(p["cw"],  CODEX_FRESH["cw"])
        self.assertEqual(p["cwr"], CODEX_FRESH["cwr"])
        self.assertEqual(p["cst"], CODEX_FRESH["cst"])
        self.assertTrue(p["cok"])

    def test_codex_fresh_updates_cache(self):
        d.build_payload(sp=False, cp=True, claude_fresh=None, codex_fresh=CODEX_FRESH)
        self.assertEqual(d._codex_last, CODEX_FRESH)

    def test_st_limited_when_s_100(self):
        fresh_limited = {"s": 100, "sr": 0, "w": 50, "wr": 1000, "st": "limited"}
        p = d.build_payload(sp=True, cp=False,
                            claude_fresh=fresh_limited, codex_fresh=None)
        self.assertEqual(p["st"], "limited")
        self.assertTrue(p["ok"])

    def test_cst_limited_when_cok(self):
        fresh_lim = {"cs": 100, "csr": 0, "cw": 50, "cwr": 1000, "cst": "limited"}
        p = d.build_payload(sp=False, cp=True,
                            claude_fresh=None, codex_fresh=fresh_lim)
        self.assertEqual(p["cst"], "limited")
        self.assertTrue(p["cok"])


class TestLastGoodFallback(unittest.TestCase):
    """4. Present but failing (fresh=None) with a prior last-good → last-good values, ok False."""

    def setUp(self):
        _reset()

    def test_claude_last_good_on_poll_failure(self):
        d._claude_last = CLAUDE_FRESH.copy()
        p = d.build_payload(sp=True, cp=False, claude_fresh=None, codex_fresh=None)
        self.assertEqual(p["s"],  CLAUDE_FRESH["s"])
        self.assertEqual(p["sr"], CLAUDE_FRESH["sr"])
        self.assertEqual(p["w"],  CLAUDE_FRESH["w"])
        self.assertEqual(p["wr"], CLAUDE_FRESH["wr"])
        self.assertFalse(p["ok"], "ok must be False when serving last-good (stale)")

    def test_codex_last_good_on_poll_failure(self):
        d._codex_last = CODEX_FRESH.copy()
        p = d.build_payload(sp=False, cp=True, claude_fresh=None, codex_fresh=None)
        self.assertEqual(p["cs"],  CODEX_FRESH["cs"])
        self.assertEqual(p["csr"], CODEX_FRESH["csr"])
        self.assertEqual(p["cw"],  CODEX_FRESH["cw"])
        self.assertEqual(p["cwr"], CODEX_FRESH["cwr"])
        self.assertFalse(p["cok"])

    def test_last_good_cache_not_overwritten_on_failure(self):
        """Cache must stay intact when fresh is None (so next-cycle last-good still works)."""
        saved = CLAUDE_FRESH.copy()
        d._claude_last = saved
        d.build_payload(sp=True, cp=False, claude_fresh=None, codex_fresh=None)
        self.assertEqual(d._claude_last, saved)

    def test_codex_last_good_cache_not_overwritten_on_failure(self):
        saved = CODEX_FRESH.copy()
        d._codex_last = saved
        d.build_payload(sp=False, cp=True, claude_fresh=None, codex_fresh=None)
        self.assertEqual(d._codex_last, saved)


class TestNeverSucceededSentinels(unittest.TestCase):
    """5. Present but never succeeded (fresh=None, last-good None) → -1 sentinels, ok False."""

    def setUp(self):
        _reset()

    def test_claude_never_succeeded(self):
        # d._claude_last is already None from _reset()
        p = d.build_payload(sp=True, cp=False, claude_fresh=None, codex_fresh=None)
        self.assertEqual(p["s"],  -1)
        self.assertEqual(p["sr"], -1)
        self.assertEqual(p["w"],  -1)
        self.assertEqual(p["wr"], -1)
        # st is "allowed" in the never-succeeded sentinel path
        self.assertEqual(p["st"], "allowed")
        self.assertFalse(p["ok"])

    def test_codex_never_succeeded(self):
        p = d.build_payload(sp=False, cp=True, claude_fresh=None, codex_fresh=None)
        self.assertEqual(p["cs"],  -1)
        self.assertEqual(p["csr"], -1)
        self.assertEqual(p["cw"],  -1)
        self.assertEqual(p["cwr"], -1)
        self.assertEqual(p["cst"], "unknown")
        self.assertFalse(p["cok"])


class TestClaudePresent(unittest.TestCase):
    """6. claude_present() — file-based path (Linux), latch behaviour."""

    def setUp(self):
        _reset()

    def test_file_exists_returns_true_and_latches(self):
        # Use an always-present repo file instead of a writable tempfile.
        existing = Path(d.__file__)
        with patch.object(d, "CREDENTIALS_PATH", existing):
            result = d.claude_present()
        self.assertTrue(result)
        self.assertTrue(d._claude_seen, "latch must be set when file exists")

    def test_file_absent_returns_false_on_linux(self):
        absent = Path("/nonexistent/no_such_file_xyz_abc.json")
        with patch.object(d, "CREDENTIALS_PATH", absent):
            with patch.object(sys, "platform", "linux"):
                result = d.claude_present()
        self.assertFalse(result)
        self.assertFalse(d._claude_seen)

    def test_latch_returns_true_after_prior_seen(self):
        """Once _claude_seen=True, file-absent on Linux still returns True
        because the file-exists branch latches _claude_seen before the
        darwin branch is reached.  On Linux with _claude_seen=True the
        function returns False (only darwin uses the latch fallback).
        Pin the actual Linux behaviour: latch does NOT short-circuit on Linux;
        a missing file → False even if _claude_seen was True."""
        absent = Path("/nonexistent/no_such_file_xyz_abc.json")
        d._claude_seen = True  # simulate prior success
        with patch.object(d, "CREDENTIALS_PATH", absent):
            with patch.object(sys, "platform", "linux"):
                result = d.claude_present()
        # On Linux the last line is `return False`, regardless of _claude_seen.
        self.assertFalse(result)

    def test_file_exists_even_if_seen_was_false(self):
        """File present → always True, always latches, regardless of prior state."""
        d._claude_seen = False
        existing = Path(d.__file__)
        with patch.object(d, "CREDENTIALS_PATH", existing):
            result = d.claude_present()
        self.assertTrue(result)
        self.assertTrue(d._claude_seen)

    # darwin-only path: we can't portably call Keychain on Linux, so we pin
    # the documented latch fallback behaviour using mocking.
    def test_darwin_latch_fallback_when_keychain_fails(self):
        """On darwin, if read_token() returns None but _claude_seen is True,
        claude_present() must return True (transient denial absorbed by latch)."""
        absent = Path("/nonexistent/no_such_file_xyz_abc.json")
        d._claude_seen = True
        with patch.object(d, "CREDENTIALS_PATH", absent):
            with patch.object(sys, "platform", "darwin"):
                with patch.object(d, "read_token", return_value=None):
                    result = d.claude_present()
        self.assertTrue(result)

    def test_darwin_no_latch_no_token_returns_false(self):
        """On darwin, if read_token() returns None and _claude_seen is False,
        claude_present() returns False."""
        absent = Path("/nonexistent/no_such_file_xyz_abc.json")
        d._claude_seen = False
        with patch.object(d, "CREDENTIALS_PATH", absent):
            with patch.object(sys, "platform", "darwin"):
                with patch.object(d, "read_token", return_value=None):
                    result = d.claude_present()
        self.assertFalse(result)

    def test_darwin_sets_latch_on_successful_token_read(self):
        """On darwin, a successful read_token() sets _claude_seen."""
        absent = Path("/nonexistent/no_such_file_xyz_abc.json")
        d._claude_seen = False
        with patch.object(d, "CREDENTIALS_PATH", absent):
            with patch.object(sys, "platform", "darwin"):
                with patch.object(d, "read_token", return_value="tok_abc"):
                    result = d.claude_present()
        self.assertTrue(result)
        self.assertTrue(d._claude_seen)


class TestCodexPresent(unittest.TestCase):
    """7. codex_present() — returns True iff CODEX_AUTH_PATH exists."""

    def setUp(self):
        _reset()

    def test_file_exists_returns_true(self):
        # Use an always-present repo file instead of a writable tempfile.
        existing = Path(d.__file__)
        with patch.object(d, "CODEX_AUTH_PATH", existing):
            self.assertTrue(d.codex_present())

    def test_file_absent_returns_false(self):
        absent = Path("/nonexistent/clawdmeter/does-not-exist")
        with patch.object(d, "CODEX_AUTH_PATH", absent):
            self.assertFalse(d.codex_present())


class TestExtractAccessToken(unittest.TestCase):
    """Unit-level coverage for _extract_access_token — pure function, no I/O."""

    def test_direct_json(self):
        blob = '{"accessToken": "tok_direct"}'
        self.assertEqual(d._extract_access_token(blob), "tok_direct")

    def test_nested_json(self):
        blob = '{"claudeAiOauth": {"accessToken": "tok_nested"}}'
        self.assertEqual(d._extract_access_token(blob), "tok_nested")

    def test_regex_fallback(self):
        blob = 'some prefix {"accessToken" : "tok_regex"} trailing'
        self.assertEqual(d._extract_access_token(blob), "tok_regex")

    def test_raw_token_fallback(self):
        raw = "sk-ant-abcdefghijklmnopqrst"   # ≥20 chars, safe charset
        self.assertEqual(d._extract_access_token(raw), raw)

    def test_empty_returns_none(self):
        self.assertIsNone(d._extract_access_token(""))
        self.assertIsNone(d._extract_access_token("   "))

    def test_garbage_returns_none(self):
        self.assertIsNone(d._extract_access_token("not json and not a token!!!"))

    def test_json_without_access_token_returns_none(self):
        self.assertIsNone(d._extract_access_token('{"foo": "bar"}'))


class TestBuildPayloadBothProviders(unittest.TestCase):
    """End-to-end: both providers present and fresh → all 14 keys correct."""

    def setUp(self):
        _reset()

    def test_both_present_fresh_all_values(self):
        p = d.build_payload(sp=True, cp=True,
                            claude_fresh=CLAUDE_FRESH, codex_fresh=CODEX_FRESH)
        # Claude fields
        self.assertEqual(p["s"],  42)
        self.assertEqual(p["sr"], 100)
        self.assertEqual(p["w"],  10)
        self.assertEqual(p["wr"], 5000)
        self.assertEqual(p["st"], "allowed")
        self.assertTrue(p["ok"])
        self.assertTrue(p["sp"])
        # Codex fields
        self.assertEqual(p["cs"],  55)
        self.assertEqual(p["csr"], 60)
        self.assertEqual(p["cw"],  20)
        self.assertEqual(p["cwr"], 10080)
        self.assertEqual(p["cst"], "allowed")
        self.assertTrue(p["cok"])
        self.assertTrue(p["cp"])

    def test_cache_updated_for_both(self):
        d.build_payload(sp=True, cp=True,
                        claude_fresh=CLAUDE_FRESH, codex_fresh=CODEX_FRESH)
        self.assertEqual(d._claude_last, CLAUDE_FRESH)
        self.assertEqual(d._codex_last,  CODEX_FRESH)

    def test_second_fresh_overwrites_cache(self):
        d.build_payload(sp=True, cp=True,
                        claude_fresh=CLAUDE_FRESH, codex_fresh=CODEX_FRESH)
        newer_claude = {"s": 99, "sr": 1, "w": 80, "wr": 200, "st": "limited"}
        d.build_payload(sp=True, cp=True,
                        claude_fresh=newer_claude, codex_fresh=CODEX_FRESH)
        self.assertEqual(d._claude_last, newer_claude)


class TestResolveProvider(unittest.TestCase):
    """resolve_provider(present, fresh, last_good, sentinel) -> (eff, new_last_good)."""

    def setUp(self):
        _reset()

    def test_absent_returns_sentinel_copy(self):
        eff, cache = d.resolve_provider(False, None, None, d.CLAUDE_SENTINEL)
        # Same VALUES as the sentinel, but a distinct object (copy-not-reference).
        self.assertEqual(eff, d.CLAUDE_SENTINEL)
        self.assertIsNot(eff, d.CLAUDE_SENTINEL)
        self.assertIsNone(cache)

    def test_fresh_present_returns_fresh_for_both(self):
        fresh = CLAUDE_FRESH.copy()
        eff, cache = d.resolve_provider(True, fresh, None, d.CLAUDE_SENTINEL)
        self.assertIs(eff, fresh)
        self.assertIs(cache, fresh)

    def test_present_failing_with_last_good(self):
        last_good = CLAUDE_FRESH.copy()
        eff, cache = d.resolve_provider(True, None, last_good, d.CLAUDE_SENTINEL)
        self.assertIs(eff, last_good)
        self.assertIs(cache, last_good)

    def test_present_never_succeeded_returns_sentinel_copy(self):
        eff, cache = d.resolve_provider(True, None, None, d.CODEX_SENTINEL)
        self.assertEqual(eff, d.CODEX_SENTINEL)
        self.assertIsNot(eff, d.CODEX_SENTINEL)
        self.assertIsNone(cache)

    def test_mutating_result_does_not_corrupt_sentinel(self):
        eff1, _ = d.resolve_provider(False, None, None, d.CLAUDE_SENTINEL)
        eff2, _ = d.resolve_provider(False, None, None, d.CLAUDE_SENTINEL)
        eff1["s"] = 999
        # The shared module constant must be untouched.
        self.assertEqual(d.CLAUDE_SENTINEL["s"], -1)
        # And the two calls return independent objects.
        self.assertIsNot(eff1, eff2)
        self.assertEqual(eff2["s"], -1)


class TestCycleSucceeded(unittest.TestCase):
    """cycle_succeeded(n_present, n_ok): success if any present provider OK, or none present."""

    def test_none_present(self):
        self.assertTrue(d.cycle_succeeded(0, 0))

    def test_one_present_none_ok(self):
        self.assertFalse(d.cycle_succeeded(1, 0))

    def test_two_present_one_ok(self):
        self.assertTrue(d.cycle_succeeded(2, 1))

    def test_two_present_none_ok(self):
        self.assertFalse(d.cycle_succeeded(2, 0))

    def test_one_present_one_ok(self):
        self.assertTrue(d.cycle_succeeded(1, 1))


def _fake_client(get_result=None, get_exc=None):
    """Return a stand-in for httpx.AsyncClient usable as an async context manager."""
    client = MagicMock()
    inst = MagicMock()
    if get_exc is not None:
        inst.get = AsyncMock(side_effect=get_exc)
    else:
        inst.get = AsyncMock(return_value=get_result)
    client.return_value.__aenter__ = AsyncMock(return_value=inst)
    client.return_value.__aexit__ = AsyncMock(return_value=False)
    return client


class TestFetchJson(unittest.IsolatedAsyncioTestCase):
    """_fetch_json(url, headers, who) -> dict | None, with no real network."""

    async def test_transport_error_returns_none(self):
        with patch.object(d, "httpx") as mh:
            mh.AsyncClient = _fake_client(get_exc=httpx.HTTPError("boom"))
            mh.HTTPError = httpx.HTTPError  # except clause references d.httpx.HTTPError
            self.assertIsNone(await d._fetch_json("http://x", {}, "Test"))

    async def test_non_200_returns_none(self):
        resp = MagicMock()
        resp.status_code = 500
        resp.text = "internal error"
        with patch.object(d, "httpx") as mh:
            mh.AsyncClient = _fake_client(get_result=resp)
            mh.HTTPError = httpx.HTTPError
            self.assertIsNone(await d._fetch_json("http://x", {}, "Test"))

    async def test_non_json_body_returns_none(self):
        resp = MagicMock()
        resp.status_code = 200
        resp.json = MagicMock(side_effect=ValueError("not json"))
        with patch.object(d, "httpx") as mh:
            mh.AsyncClient = _fake_client(get_result=resp)
            mh.HTTPError = httpx.HTTPError
            self.assertIsNone(await d._fetch_json("http://x", {}, "Test"))

    async def test_success_returns_parsed_dict(self):
        resp = MagicMock()
        resp.status_code = 200
        resp.json = MagicMock(return_value={"x": 1})
        with patch.object(d, "httpx") as mh:
            mh.AsyncClient = _fake_client(get_result=resp)
            mh.HTTPError = httpx.HTTPError
            self.assertEqual(await d._fetch_json("http://x", {}, "Test"), {"x": 1})


if __name__ == "__main__":
    unittest.main()
