#!/usr/bin/env python3
"""Mock of Gemini's OpenAI-compatible endpoint: the first answer is a tool call
carrying a thought signature; later requests must send it back (as Gemini 3
demands; calls made by another model may carry Google's placeholder), else
400 like the real API. Paths under /plain act as a strict OpenAI-style server
that rejects Gemini's extra_content."""
import json, sys
from http.server import BaseHTTPRequestHandler, HTTPServer

SIG = "c2lnbmF0dXJlLWZyb20tbW9jaw=="
PLACEHOLDER = "skip_thought_signature_validator"

class H(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_POST(self):
        raw = self.rfile.read(int(self.headers["Content-Length"]))
        body = json.loads(raw)
        if self.path.startswith("/plain"):
            if b"extra_content" in raw:
                return self.reply(400, {"error": {"message": "Unrecognized field extra_content"}})
            return self.reply(200, {"choices": [{"message": {"role": "assistant", "content": "plain ok"}}]})
        calls = [c for m in body["messages"] if m.get("role") == "assistant" for c in m.get("tool_calls") or []]
        if not calls:
            out = {"choices": [{"message": {"role": "assistant", "content": None, "tool_calls": [
                {"id": "call_1", "type": "function", "function": {"name": "get_inventory", "arguments": "{}"},
                 "extra_content": {"google": {"thought_signature": SIG}}}]}}]}
        elif all(c.get("extra_content", {}).get("google", {}).get("thought_signature") in (SIG, PLACEHOLDER)
                 for m in body["messages"] if m.get("role") == "assistant" and m.get("tool_calls")
                 for c in m["tool_calls"][:1]):
            out = {"choices": [{"message": {"role": "assistant", "content": "The inventory is empty."}}]}
        else:
            return self.reply(400, {"error": {"code": 400, "message": "Function call is missing a thought_signature"}})
        self.reply(200, out)

    def reply(self, status, obj):
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        self.wfile.write(json.dumps(obj).encode())

HTTPServer(("127.0.0.1", int(sys.argv[1])), H).serve_forever()
