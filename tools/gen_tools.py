#!/usr/bin/env python3
"""Regenerates data/tools.json from the MCP server in core/mcp: the tool names,
descriptions and JSON schemas the agent offers the model. Every tool name is
also the core control command it runs. Needs node >= 22.18 and the npm
packages of core/mcp (npm ci there)."""
import json, os, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MCP = os.path.join(ROOT, "core", "mcp")
# Handled by the app itself (connection, credentials) or composed in TypeScript.
SKIP = {"connect", "disconnect", "set_autoreply", "upkeep"}

def strip(schema):
    if isinstance(schema, dict):
        return {k: strip(v) for k, v in schema.items() if k != "$schema"}
    if isinstance(schema, list):
        return [strip(v) for v in schema]
    return schema

def main():
    msgs = [
        {"jsonrpc": "2.0", "id": 1, "method": "initialize",
         "params": {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "gen_tools", "version": "1"}}},
        {"jsonrpc": "2.0", "method": "notifications/initialized"},
        {"jsonrpc": "2.0", "id": 2, "method": "tools/list"},
    ]
    with tempfile.TemporaryDirectory() as tmp:
        env = dict(os.environ, MC_AUTOREPLY="0", MINEMCP_SOCKET=os.path.join(tmp, "unused.sock"))
        proc = subprocess.Popen(["node", "src/server.ts"], cwd=MCP, env=env, stdin=subprocess.PIPE,
                                stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        for m in msgs:
            proc.stdin.write(json.dumps(m) + "\n")
        proc.stdin.flush()
        tools = None
        for line in proc.stdout:
            m = json.loads(line)
            if m.get("id") == 2:
                tools = m["result"]["tools"]
                break
        proc.kill()
    if tools is None:
        sys.exit("no tools/list answer from the MCP server")
    out = [{"name": t["name"], "description": t["description"], "parameters": strip(t["inputSchema"])}
           for t in tools if t["name"] not in SKIP]
    with open(os.path.join(ROOT, "data", "tools.json"), "w") as f:
        json.dump(out, f, indent=1, ensure_ascii=False)
        f.write("\n")
    print(f"{len(out)} tools written to data/tools.json")

if __name__ == "__main__":
    main()
