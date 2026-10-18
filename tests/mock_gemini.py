#!/usr/bin/env python3
"""Mock of Gemini's OpenAI-compatible endpoint: the first answer is a tool call
carrying a thought signature; later requests must send it back (as Gemini 3
demands), else 400 like the real API."""
import json, sys
from http.server import BaseHTTPRequestHandler, HTTPServer

SIG = "c2lnbmF0dXJlLWZyb20tbW9jaw=="

class H(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        calls = [c for m in body["messages"] if m.get("role") == "assistant" for c in m.get("tool_calls") or []]
        if not calls:
            out = {"choices": [{"message": {"role": "assistant", "content": None, "tool_calls": [
                {"id": "call_1", "type": "function", "function": {"name": "get_inventory", "arguments": "{}"},
                 "extra_content": {"google": {"thought_signature": SIG}}}]}}]}
        elif all(c.get("extra_content", {}).get("google", {}).get("thought_signature") == SIG for c in calls):
            out = {"choices": [{"message": {"role": "assistant", "content": "The inventory is empty."}}]}
        else:
            self.send_response(400)
            self.end_headers()
            self.wfile.write(json.dumps({"error": {"code": 400, "message": "Function call is missing a thought_signature"}}).encode())
            return
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        self.wfile.write(json.dumps(out).encode())

HTTPServer(("127.0.0.1", int(sys.argv[1])), H).serve_forever()
