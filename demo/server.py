#!/usr/bin/env python3
"""HTTP shim for demo/index.html.

Prefers the real C++ process (build/lob_demo --json). If that binary is
missing, falls back to an in-process toy book — still labeled demo, not
the engine hot path.
"""
from __future__ import annotations

import json
import os
import subprocess
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parent
REPO = ROOT.parent
HTML = (ROOT / "index.html").read_bytes()

CANDIDATES = [
    REPO / "build" / "lob_demo",
    REPO / "build" / "Release" / "lob_demo",
]


class ToyBook:
    def __init__(self) -> None:
        self.next_id = 1
        self.bids: dict[int, list[dict]] = {}
        self.asks: dict[int, list[dict]] = {}

    def _levels(self, book: dict[int, list[dict]], reverse: bool) -> list[dict]:
        out = []
        for px in sorted(book.keys(), reverse=reverse):
            os = book[px]
            qty = sum(o["qty"] for o in os)
            if qty:
                out.append({"price": px, "qty": qty, "n": len(os)})
        return out

    def snapshot(self, extra: dict) -> dict:
        extra.update(
            {
                "ok": extra.get("ok", True),
                "bids": self._levels(self.bids, True),
                "asks": self._levels(self.asks, False),
                "mock": True,
            }
        )
        return extra

    def handle(self, cmd: dict) -> dict:
        op = cmd.get("op")
        if op == "reset":
            self.__init__()
            return self.snapshot({"id": 0, "fills": [], "latency_ns": 0})
        if op == "seed":
            self.__init__()
            for i in range(1, 9):
                self.handle({"op": "limit", "side": "BUY", "price": 10000 - i, "qty": 5 + i})
                self.handle({"op": "limit", "side": "SELL", "price": 10000 + i, "qty": 5 + i})
            return self.snapshot({"id": self.next_id - 1, "fills": [], "latency_ns": 0})
        if op == "limit":
            side = cmd.get("side", "BUY").upper()
            px, qty = int(cmd["price"]), int(cmd["qty"])
            oid = self.next_id
            self.next_id += 1
            book = self.bids if side == "BUY" else self.asks
            book.setdefault(px, []).append({"id": oid, "qty": qty})
            return self.snapshot({"id": oid, "fills": [], "latency_ns": 0})
        if op == "cancel":
            cid = int(cmd.get("id") or 0)
            ok = False
            for book in (self.bids, self.asks):
                for px, os in list(book.items()):
                    book[px] = [o for o in os if o["id"] != cid]
                    if len(book[px]) != len(os):
                        ok = True
                    if not book[px]:
                        del book[px]
            return self.snapshot({"id": cid, "ok": ok, "fills": [], "latency_ns": 0})
        if op == "market":
            side = cmd.get("side", "BUY").upper()
            left = int(cmd["qty"])
            opp = self.asks if side == "BUY" else self.bids
            fills = []
            while left > 0 and opp:
                px = min(opp) if side == "BUY" else max(opp)
                o = opp[px][0]
                take = min(left, o["qty"])
                o["qty"] -= take
                left -= take
                fills.append({"id": o["id"], "price": px, "qty": take})
                if o["qty"] == 0:
                    opp[px].pop(0)
                if not opp[px]:
                    del opp[px]
            return self.snapshot({"id": 0, "fills": fills, "latency_ns": 0})
        return self.snapshot({"ok": False, "fills": [], "id": 0})


class CppBook:
    def __init__(self, path: Path) -> None:
        self.path = path
        self.proc = subprocess.Popen(
            [str(path), "--json"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            text=True,
            bufsize=1,
        )

    def _line(self, cmd: str) -> dict:
        assert self.proc.stdin and self.proc.stdout
        self.proc.stdin.write(cmd + "\n")
        self.proc.stdin.flush()
        raw = self.proc.stdout.readline()
        return json.loads(raw)

    def handle(self, cmd: dict) -> dict:
        op = cmd.get("op")
        if op == "reset":
            self.proc.kill()
            self.__init__(self.path)
            return self._line("SNAP")
        if op == "seed":
            self.handle({"op": "reset"})
            last = {}
            for i in range(1, 9):
                last = self._line(f"LIMIT BUY {10000 - i} {5 + i}")
                last = self._line(f"LIMIT SELL {10000 + i} {5 + i}")
            return last
        if op == "limit":
            side = cmd.get("side", "BUY").upper()
            return self._line(f"LIMIT {side} {int(cmd['price'])} {int(cmd['qty'])}")
        if op == "market":
            side = cmd.get("side", "BUY").upper()
            return self._line(f"MARKET {side} {int(cmd['qty'])}")
        if op == "cancel":
            return self._line(f"CANCEL {int(cmd.get('id') or 0)}")
        return self._line("SNAP")


def pick_backend():
    for p in CANDIDATES:
        if p.is_file() and os.access(p, os.X_OK):
            print(f"using C++ {p}", file=sys.stderr)
            return CppBook(p)
    print("lob_demo not found — JS-equivalent Python toy book", file=sys.stderr)
    return ToyBook()


BACKEND = pick_backend()


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt: str, *args) -> None:
        sys.stderr.write("%s - %s\n" % (self.address_string(), fmt % args))

    def do_GET(self) -> None:
        if self.path in ("/", "/index.html"):
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(HTML)))
            self.end_headers()
            self.wfile.write(HTML)
            return
        self.send_error(404)

    def do_POST(self) -> None:
        if self.path != "/api":
            self.send_error(404)
            return
        n = int(self.headers.get("Content-Length", "0"))
        cmd = json.loads(self.rfile.read(n) or b"{}")
        body = json.dumps(BACKEND.handle(cmd)).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8765
    httpd = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    print(f"Demo UI http://127.0.0.1:{port}  (not production)")
    httpd.serve_forever()
