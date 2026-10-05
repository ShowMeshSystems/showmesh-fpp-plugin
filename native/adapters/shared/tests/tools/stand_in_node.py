#!/usr/bin/env python3
"""A stand-in for a node's fallback ingress, for smoke-running the driver.

It checks nothing: it answers "installed" to a program and "authorized" to an
activation, and prints what it received. It is not the node agent.

    stand_in_node.py [port]
"""
import http.server
import json
import sys


class Handler(http.server.BaseHTTPRequestHandler):
    def _answer(self, outcome):
        body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        print(self.command, self.path, len(body), "bytes", flush=True)
        out = json.dumps({"accepted": True, "outcome": outcome, "reason": "stand-in node"}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(out)))
        self.end_headers()
        self.wfile.write(out)

    def do_PUT(self):
        self._answer("installed")

    def do_POST(self):
        self._answer("authorized")

    def log_message(self, *args):
        pass


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
    http.server.HTTPServer(("127.0.0.1", port), Handler).serve_forever()
