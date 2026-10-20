import copy
import io
import json
from pathlib import Path
import tempfile
import time
import unittest

from tools.tonworld_agents import apply_event, completed_text, event_text, exchange_sse, provider_error


class Reply(io.BytesIO):
    status = 200


class FakeTransport:
    def __init__(self, content):
        self.content, self.writes, self.calls = content, [], []

    def request(self, path, payload, timeout):
        self.calls.append((path, payload, timeout))
        return Reply(self.content)

    def write_json(self, path, value):
        self.writes.append(copy.deepcopy(value))

    def key(self):
        return "private-test-key"


class TransportTests(unittest.TestCase):
    def test_stream_finish_usage_heartbeats_and_no_reasoning_text(self):
        events = [
            {"model": "codex-native/gpt-6-astra", "choices": [{"delta": {"reasoning_content": "private reasoning"}}]},
            {"choices": [{"delta": {"content": "visible answer"}}]},
            {"choices": [{"delta": {}, "finish_reason": "stop"}]},
            {"choices": [], "usage": {"completion_tokens": 500}},
        ]
        payload = b": keep-alive\n\n" + b"".join(("data: " + json.dumps(e) + "\n\n").encode() for e in events) + b"data: [DONE]\n\n"
        transport = FakeTransport(payload)
        state = {"created_at": time.time(), "requested_model": "gpt-6-astra"}
        body = {"task": "brief", "context": "", "effort": "xhigh", "max_tokens": 2048,
                "config": {"endpoint": "https://kube943.prod.tonworld.ru/v1", "event_timeout_seconds": 600}}
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            exchange_sse(transport, directory, state, body)
            self.assertEqual((directory / "answer.txt").read_text(), "visible answer")
        self.assertEqual(len(transport.calls), 1)
        self.assertEqual(state["heartbeat_count"], 1)
        self.assertEqual(state["usage"]["completion_tokens"], 500)
        self.assertFalse(state["truncated"])

    def test_incomplete_stream_is_not_success_or_automatically_retried(self):
        transport = FakeTransport(b'data: {"choices":[{"delta":{"content":"partial"}}]}\n\n')
        body = {"task": "brief", "context": "", "effort": "max", "max_tokens": 2048,
                "config": {"endpoint": "https://kube943.prod.tonworld.ru/v1", "event_timeout_seconds": 600}}
        with tempfile.TemporaryDirectory() as name:
            with self.assertRaisesRegex(RuntimeError, "before \\[DONE\\]"):
                exchange_sse(transport, Path(name), {"created_at": time.time(), "requested_model": "gpt-6.1-sol"}, body)
        self.assertEqual(len(transport.calls), 1)

    def test_key_redaction_and_responses_terminal_event(self):
        transport = FakeTransport(b"")
        error = provider_error(transport, {"code": "quota", "message": "private-test-key", "headers": "discard"})
        self.assertEqual(error, {"code": "quota", "message": "[redacted]"})
        self.assertEqual(event_text({"type": "response.reasoning_summary_text.delta", "delta": "hidden"}), "")
        state = {}
        self.assertTrue(apply_event(state, {"type": "response.incomplete", "response": {"model": "model", "incomplete_details": {"reason": "max_output_tokens"}}}))
        self.assertTrue(state["truncated"])
        self.assertEqual(completed_text({"output": [{"type": "reasoning", "content": [{"type": "output_text", "text": "hidden"}]},
                                                   {"type": "message", "content": [{"type": "output_text", "text": "visible"}]}]}), "visible")


if __name__ == "__main__":
    unittest.main()
