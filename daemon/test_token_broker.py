"""
Behavior-pinning tests for token_broker.py.

Pure-function + in-process HTTP tests, no network egress. Run with stdlib python:
    python3 test_token_broker.py -v
"""

import base64
import json
import os
import sys
import threading
import time
import unittest
import urllib.error
import urllib.request
from http.server import ThreadingHTTPServer
from unittest.mock import patch

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import token_broker as b


def mkjwt(exp) -> str:
    """A 3-segment JWT whose payload carries the given exp (or no exp if None)."""
    payload = {} if exp is None else {"exp": exp}
    seg = base64.urlsafe_b64encode(json.dumps(payload).encode()).rstrip(b"=").decode()
    return f"hdr.{seg}.sig"


class JwtExpTests(unittest.TestCase):
    def test_valid(self):
        self.assertEqual(b.jwt_exp(mkjwt(1781368856)), 1781368856)

    def test_missing_exp_claim(self):
        self.assertIsNone(b.jwt_exp(mkjwt(None)))

    def test_not_a_jwt(self):
        self.assertIsNone(b.jwt_exp("not-a-jwt"))

    def test_non_string_returns_none(self):
        # regression: must not raise AttributeError (was uncaught -> HTTP 500)
        self.assertIsNone(b.jwt_exp(12345))
        self.assertIsNone(b.jwt_exp(None))


class ClaudeTokenTests(unittest.TestCase):
    def test_env_wins_and_strips(self):
        with patch.dict(os.environ, {b.CLAUDE_TOKEN_ENV: "  sk-ant-oat01-X  "}):
            self.assertEqual(b.claude_token(), "sk-ant-oat01-X")

    def test_empty_env_falls_through_to_missing_file(self):
        with (
            patch.dict(os.environ, {b.CLAUDE_TOKEN_ENV: "   "}),
            patch.object(b, "CLAUDE_TOKEN_FILE", b.Path("/nonexistent/clawd_tok")),
        ):
            self.assertIsNone(b.claude_token())

    def test_file_used_when_env_absent(self):
        import tempfile

        with tempfile.NamedTemporaryFile("w", suffix=".tok", delete=False) as f:
            f.write("file-token\n")
            path = f.name
        try:
            env = {k: v for k, v in os.environ.items() if k != b.CLAUDE_TOKEN_ENV}
            with (
                patch.dict(os.environ, env, clear=True),
                patch.object(b, "CLAUDE_TOKEN_FILE", b.Path(path)),
            ):
                self.assertEqual(b.claude_token(), "file-token")
        finally:
            os.unlink(path)


class CodexCredsTests(unittest.TestCase):
    def _write_auth(self, obj):
        import tempfile

        with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as f:
            json.dump(obj, f)
        return f.name

    def test_valid(self):
        path = self._write_auth({"tokens": {"access_token": "at", "account_id": "acct"}})
        try:
            with patch.object(b, "CODEX_AUTH_PATH", b.Path(path)):
                self.assertEqual(b.codex_creds(), ("at", "acct"))
        finally:
            os.unlink(path)

    def test_missing_key_returns_none(self):
        path = self._write_auth({"tokens": {"access_token": "at"}})
        try:
            with patch.object(b, "CODEX_AUTH_PATH", b.Path(path)):
                self.assertIsNone(b.codex_creds())
        finally:
            os.unlink(path)

    def test_absent_file_returns_none(self):
        with patch.object(b, "CODEX_AUTH_PATH", b.Path("/nonexistent/auth.json")):
            self.assertIsNone(b.codex_creds())


class BuildTokensTests(unittest.TestCase):
    def test_both_usable_is_200(self):
        with (
            patch.object(b, "claude_token", lambda: "sk-ant-oat01-X"),
            patch.object(b, "codex_creds", lambda: (mkjwt(int(time.time()) + 99999), "acct")),
        ):
            code, body = b.build_tokens()
        self.assertEqual(code, 200)
        self.assertEqual(body["claude"]["token"], "sk-ant-oat01-X")
        self.assertEqual(body["codex"]["account_id"], "acct")

    def test_missing_claude_is_409_but_codex_token_present(self):
        with (
            patch.object(b, "claude_token", lambda: None),
            patch.object(b, "codex_creds", lambda: (mkjwt(int(time.time()) + 99999), "acct")),
        ):
            code, body = b.build_tokens()
        self.assertEqual(code, 409)
        self.assertIn("needs_action", body["claude"])
        self.assertIn("token", body["codex"])

    def test_expired_codex_is_409(self):
        with (
            patch.object(b, "claude_token", lambda: "X"),
            patch.object(b, "codex_creds", lambda: (mkjwt(int(time.time()) - 10), "acct")),
        ):
            code, body = b.build_tokens()
        self.assertEqual(code, 409)
        self.assertIn("expired", body["codex"]["needs_action"])

    def test_near_expiry_codex_is_409(self):
        # Still valid by the clock, but within CODEX_EXP_MARGIN of expiry — must
        # be treated as unusable so the device re-auths before the token dies.
        with (
            patch.object(b, "claude_token", lambda: "X"),
            patch.object(
                b,
                "codex_creds",
                lambda: (mkjwt(int(time.time()) + b.CODEX_EXP_MARGIN - 30), "acct"),
            ),
        ):
            code, body = b.build_tokens()
        self.assertEqual(code, 409)
        self.assertIn("expired", body["codex"]["needs_action"])

    def test_unparseable_codex_is_409(self):
        with (
            patch.object(b, "claude_token", lambda: "X"),
            patch.object(b, "codex_creds", lambda: ("garbage", "acct")),
        ):
            code, body = b.build_tokens()
        self.assertEqual(code, 409)
        self.assertIn("unparseable", body["codex"]["needs_action"])

    def test_both_absent_is_409_with_needs_action_and_no_tokens(self):
        with (
            patch.object(b, "claude_token", lambda: None),
            patch.object(b, "codex_creds", lambda: None),
        ):
            code, body = b.build_tokens()
        self.assertEqual(code, 409)
        self.assertIn("needs_action", body["claude"])
        self.assertNotIn("token", body["claude"])
        self.assertIn("needs_action", body["codex"])
        self.assertNotIn("token", body["codex"])


class HttpEndpointTests(unittest.TestCase):
    """Exercises routing + X-Broker-Key gating against a live in-process server."""

    @classmethod
    def setUpClass(cls):
        cls._key_patch = patch.object(b, "BROKER_KEY", "testkey123")
        cls._claude_patch = patch.object(b, "claude_token", lambda: "sk-ant-oat01-X")
        cls._codex_patch = patch.object(
            b, "codex_creds", lambda: (mkjwt(int(time.time()) + 99999), "acct")
        )
        cls._key_patch.start()
        cls._claude_patch.start()
        cls._codex_patch.start()
        cls.server = ThreadingHTTPServer(("127.0.0.1", 0), b.BrokerHandler)
        cls.port = cls.server.server_address[1]
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls._codex_patch.stop()
        cls._claude_patch.stop()
        cls._key_patch.stop()

    def _get(self, path, key=None):
        req = urllib.request.Request(f"http://127.0.0.1:{self.port}{path}")
        if key is not None:
            req.add_header("X-Broker-Key", key)
        try:
            r = urllib.request.urlopen(req, timeout=5)
            return r.status, json.loads(r.read())
        except urllib.error.HTTPError as e:
            return e.code, json.loads(e.read())

    def test_healthz_open(self):
        self.assertEqual(self._get("/healthz")[0], 200)

    def test_tokens_no_key_forbidden(self):
        self.assertEqual(self._get("/tokens")[0], 403)

    def test_tokens_wrong_key_forbidden(self):
        self.assertEqual(self._get("/tokens", "nope")[0], 403)

    def test_tokens_empty_key_forbidden(self):
        self.assertEqual(self._get("/tokens", "")[0], 403)

    def test_tokens_correct_key_ok(self):
        code, body = self._get("/tokens", "testkey123")
        self.assertEqual(code, 200)
        self.assertIn("token", body["claude"])

    def test_unknown_path_404(self):
        self.assertEqual(self._get("/nope", "testkey123")[0], 404)


if __name__ == "__main__":
    unittest.main(verbosity=2)
