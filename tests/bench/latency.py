#!/usr/bin/env python3
"""Замер задержки: от отправки move до смены _m в дереве Sway.

Подключение к Sway IPC постоянное, без запуска swaymsg на каждый опрос.
Печатает min/p50/p95/max в миллисекундах для двух направлений.
"""
import json
import os
import socket
import statistics
import struct
import sys
import time

IPC_MAGIC = b"i3-ipc"


class IPC:
    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.s.connect(path)

    def _read(self, n):
        buf = b""
        while len(buf) < n:
            chunk = self.s.recv(n - len(buf))
            if not chunk:
                raise EOFError("sway closed the socket")
            buf += chunk
        return buf

    def request(self, typ, payload=b""):
        hdr = IPC_MAGIC + struct.pack("=II", len(payload), typ)
        self.s.sendall(hdr + payload)
        hdr = self._read(14)
        if hdr[:6] != IPC_MAGIC:
            raise RuntimeError("bad IPC frame")
        ln, rtyp = struct.unpack("=II", hdr[6:])
        return rtyp, self._read(ln)

    def command(self, cmd):
        _, payload = self.request(0, cmd.encode())
        return json.loads(payload)

    def get_tree(self):
        _, payload = self.request(4)
        return json.loads(payload)


def walk(node):
    yield node
    for key in ("nodes", "floating_nodes"):
        for child in node.get(key) or []:
            yield from walk(child)


def find_ws(tree):
    spaces = [n for n in walk(tree) if n.get("type") == "workspace"]
    # Рабочий стол с меткой _m<id>: не путаем с __i3_scratch.
    for ws in spaces:
        mark = "_m%d" % ws["id"]
        if any(mark in (n.get("marks") or []) for n in walk(ws)):
            return ws
    for ws in spaces:
        if ws.get("focused") and not ws.get("name", "").startswith("__i3"):
            return ws
    if not spaces:
        raise RuntimeError("no workspace")
    return spaces[0]


def leaves(ws):
    out = []
    for node in walk(ws):
        if node.get("type") != "con":
            continue
        if node.get("nodes") or node.get("floating_nodes"):
            continue
        if (node.get("floating") or "auto_off").endswith("_on"):
            continue
        if (node.get("scratchpad_state") or "none") != "none":
            continue
        out.append(node)
    return out


def master_app(ws):
    wid = ws["id"]
    mark = "_m%d" % wid
    for node in walk(ws):
        if node.get("type") == "con" and mark in (node.get("marks") or []):
            return node.get("app_id")
    return None


def stack_apps(ws):
    wid = ws["id"]
    mark = "_m%d" % wid
    return [
        n.get("app_id")
        for n in leaves(ws)
        if mark not in (n.get("marks") or [])
    ]


def wait_master(ipc, old, timeout=2.0):
    t0 = time.perf_counter()
    while True:
        tree = ipc.get_tree()
        ws = find_ws(tree)
        cur = master_app(ws)
        if cur != old:
            return (time.perf_counter() - t0) * 1000.0, cur
        if time.perf_counter() - t0 > timeout:
            raise TimeoutError("master did not change from %r" % old)


def pct(vals, q):
    vals = sorted(vals)
    k = max(0, min(len(vals) - 1, int(round((len(vals) - 1) * q))))
    return vals[k]


def report(label, vals):
    print(
        "%s: n=%d min=%.2f p50=%.2f p95=%.2f max=%.2f ms"
        % (
            label,
            len(vals),
            min(vals),
            statistics.median(vals),
            pct(vals, 0.95),
            max(vals),
        )
    )


def main():
    sock = os.environ.get("SWAYSOCK") or sys.argv[1]
    cycles = int(sys.argv[2]) if len(sys.argv) > 2 else 20
    ipc = IPC(sock)

    tree = ipc.get_tree()
    ws = find_ws(tree)
    apps = [n.get("app_id") for n in leaves(ws)]
    m = master_app(ws)
    stack = stack_apps(ws)
    print("ws=%s master=%s stack=%s" % (ws["id"], m, " ".join(stack)))
    if len(stack) < 2:
        print("need at least 3 windows", file=sys.stderr)
        return 2
    target = stack[1]  # второй сверху: ветка точного зума через _s

    left, right = [], []
    for i in range(cycles):
        ipc.command('[app_id="%s"] focus' % target)
        old = master_app(find_ws(ipc.get_tree()))
        ipc.command("move left")
        dt, new = wait_master(ipc, old)
        left.append(dt)
        if new != target:
            raise RuntimeError("zoom left: master=%r, want %r" % (new, target))

        ipc.command('[app_id="%s"] focus' % target)
        old = master_app(find_ws(ipc.get_tree()))
        ipc.command("move right")
        dt, new = wait_master(ipc, old)
        right.append(dt)

    report("zoom left ", left)
    report("zoom right", right)
    return 0


if __name__ == "__main__":
    sys.exit(main())
