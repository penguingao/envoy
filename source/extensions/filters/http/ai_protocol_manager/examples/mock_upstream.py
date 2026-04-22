#!/usr/bin/env python3
"""Tiny mock upstream for ai_protocol_manager testing.

Logs every incoming request (method, path, headers, body) so you can verify
the filter rewrote the path / re-encoded the body correctly. Returns a
canned OpenAI-shaped completion response.

Usage:
    python3 mock_upstream.py            # listens on 127.0.0.1:18080
    python3 mock_upstream.py --port N   # custom port
"""

import argparse
import json
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer


CANNED_OPENAI_REPLY = {
    "id": "chatcmpl-mock-001",
    "object": "chat.completion",
    "created": 1714000000,
    "model": "mock-model",
    "choices": [
        {
            "index": 0,
            "message": {"role": "assistant", "content": "mock reply"},
            "finish_reason": "stop",
        }
    ],
    "usage": {"prompt_tokens": 4, "completion_tokens": 2, "total_tokens": 6},
}


CANNED_GEMINI_REPLY = {
    "responseId": "mock-resp-001",
    "modelVersion": "gemini-2.5-flash",
    "candidates": [
        {
            "content": {"role": "model", "parts": [{"text": "mock reply"}]},
            "finishReason": "STOP",
            "index": 0,
        }
    ],
    "usageMetadata": {
        "promptTokenCount": 4,
        "candidatesTokenCount": 2,
        "totalTokenCount": 6,
    },
}


class Handler(BaseHTTPRequestHandler):
    def _log(self, body):
        print(f"\n=== {self.command} {self.path} ===", flush=True)
        for k, v in self.headers.items():
            print(f"{k}: {v}", flush=True)
        if body:
            try:
                pretty = json.dumps(json.loads(body), indent=2)
                print(f"\n{pretty}", flush=True)
            except (ValueError, TypeError):
                print(f"\n{body!r}", flush=True)

    def _reply(self):
        # Pick the canned response shape that matches the path the filter
        # rewrote to. Vertex paths contain "publishers/google/models".
        if "publishers/google/models" in self.path:
            payload = json.dumps(CANNED_GEMINI_REPLY).encode()
        else:
            payload = json.dumps(CANNED_OPENAI_REPLY).encode()
        self.send_response(200)
        self.send_header("content-type", "application/json")
        self.send_header("content-length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def do_POST(self):
        length = int(self.headers.get("content-length", 0))
        body = self.rfile.read(length).decode("utf-8", errors="replace") if length else ""
        self._log(body)
        self._reply()

    def do_GET(self):
        self._log("")
        self._reply()

    def log_message(self, fmt, *args):
        # Silence default logging; we already print structured output above.
        pass


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--port", type=int, default=18080)
    p.add_argument("--host", default="127.0.0.1")
    args = p.parse_args()
    print(f"mock upstream listening on http://{args.host}:{args.port}", flush=True)
    HTTPServer((args.host, args.port), Handler).serve_forever()


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(0)
