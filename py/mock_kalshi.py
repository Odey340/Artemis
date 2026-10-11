#!/usr/bin/env python3
"""A tiny local stand-in for the Kalshi Trade API v2, for dry-running Artemis.

It serves one mutually-exclusive event whose books contain a NO-basket
arbitrage, verifies every authenticated request's RSA-PSS / Ed25519
signature (if a public key is given), fills fill-or-kill orders against its
own books, and removes the liquidity it fills - so the opportunity disappears
once the bot has taken it.

    python3 py/mock_kalshi.py --port 8765 --pubkey /path/to/public.pem
    # then point artemis at it:  "kalshi": {"base_url": "http://127.0.0.1:8765/trade-api/v2"}

Standard library only (signature checks use `cryptography` when installed).
"""

import argparse
import base64
import json
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

PREFIX = "/trade-api/v2"
LOCK = threading.Lock()

# YES bids / NO bids per market: [price_dollars, contracts]. Sum of best YES
# bids = 1.10 -> buying NO on all three costs 1.90 for a 2.00 payout.
BOOKS = {
    "KXMOCK-26-A": {"yes": [[0.40, 10.0], [0.30, 50.0]], "no": [[0.55, 50.0]]},
    "KXMOCK-26-B": {"yes": [[0.35, 10.0], [0.30, 50.0]], "no": [[0.60, 50.0]]},
    "KXMOCK-26-C": {"yes": [[0.35, 10.0], [0.30, 50.0]], "no": [[0.60, 50.0]]},
    "KXOTHER-26-X": {"yes": [[0.50, 10.0]], "no": [[0.45, 10.0]]},
}
ORDERS = []
RACE = {"ticker": None, "mode": "thin", "done": False}  # simulate another trader racing us
STATS = {"auth_ok": 0, "auth_fail": 0, "requests": 0}
PUBKEY = None


def book_json(ticker):
    b = BOOKS[ticker]
    fmt = lambda side: [[f"{p:.4f}", f"{q:.2f}"] for p, q in side if q > 0]
    return {"yes_dollars": fmt(b["yes"]), "no_dollars": fmt(b["no"])}


def market(ticker, event):
    b = BOOKS[ticker]
    yb = b["yes"][0][0] if b["yes"] else 0
    nb = b["no"][0][0] if b["no"] else 0
    return {
        "ticker": ticker, "event_ticker": event, "status": "active",
        "yes_sub_title": ticker[-1], "close_time": "2030-01-01T00:00:00Z",
        "yes_bid_dollars": f"{yb:.4f}", "no_bid_dollars": f"{nb:.4f}",
        "yes_ask_dollars": f"{1 - nb:.4f}" if nb else "0.0000",
        "no_ask_dollars": f"{1 - yb:.4f}" if yb else "0.0000",
    }


def verify(method, path, headers):
    if PUBKEY is None:
        return True
    from cryptography.hazmat.primitives import hashes
    from cryptography.hazmat.primitives.asymmetric import padding, rsa
    try:
        msg = (headers["KALSHI-ACCESS-TIMESTAMP"] + method + path).encode()
        sig = base64.b64decode(headers["KALSHI-ACCESS-SIGNATURE"])
        if isinstance(PUBKEY, rsa.RSAPublicKey):
            PUBKEY.verify(sig, msg, padding.PSS(mgf=padding.MGF1(hashes.SHA256()),
                                               salt_length=padding.PSS.DIGEST_LENGTH), hashes.SHA256())
        else:
            PUBKEY.verify(sig, msg)
        return abs(int(headers["KALSHI-ACCESS-TIMESTAMP"]) - time.time() * 1000) < 30_000
    except Exception:
        return False


def fill(order):
    """Fill a V2 order (quoted on the YES book) fill-or-kill style."""
    t, side = order["ticker"], order["side"]
    price, count = float(order["price"]), float(order["count"])
    b = BOOKS.get(t)
    if b is None:
        return 0.0, 0.0
    # bid YES @p hits NO bids >= 1-p ; ask YES @p hits YES bids >= p
    ladder = b["no"] if side == "bid" else b["yes"]
    ok = lambda lvl: (1 - lvl[0] <= price + 1e-9) if side == "bid" else (lvl[0] >= price - 1e-9)
    avail = sum(q for p, q in ladder if ok([p, q]))
    tif = order.get("time_in_force")
    want = count if tif == "fill_or_kill" and avail >= count else (min(avail, count) if tif != "fill_or_kill" else 0)
    got, cost, left = 0.0, 0.0, want
    for lvl in ladder:
        if left <= 0 or not ok(lvl):
            break
        take = min(lvl[1], left)
        yes_px = 1 - lvl[0] if side == "bid" else lvl[0]
        cost += take * yes_px
        lvl[1] -= take
        left -= take
        got += take
    b["yes"] = [l for l in b["yes"] if l[1] > 0]
    b["no"] = [l for l in b["no"] if l[1] > 0]
    return got, (cost / got if got else 0.0)


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def reply(self, code, obj):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def authed(self, method, path):
        ok = verify(method, path, self.headers)
        STATS["auth_ok" if ok else "auth_fail"] += 1
        if not ok:
            self.reply(401, {"error": {"code": "unauthorized", "message": "bad signature"}})
        return ok

    def do_GET(self):
        STATS["requests"] += 1
        u = urlparse(self.path)
        p, q = u.path, parse_qs(u.query)
        if not p.startswith(PREFIX):
            return self.reply(404, {})
        r = p[len(PREFIX):]
        with LOCK:
            if r == "/exchange/status":
                return self.reply(200, {"exchange_active": True, "trading_active": True})
            if r == "/events":
                evs = [
                    {"event_ticker": "KXMOCK-26", "series_ticker": "KXMOCK", "title": "Mock event",
                     "mutually_exclusive": True,
                     "markets": [market(t, "KXMOCK-26") for t in BOOKS if t.startswith("KXMOCK")]},
                    {"event_ticker": "KXOTHER-26", "series_ticker": "KXOTHER", "title": "Not ME",
                     "mutually_exclusive": False, "markets": [market("KXOTHER-26-X", "KXOTHER-26")]},
                ]
                return self.reply(200, {"events": evs, "cursor": ""})
            if r.startswith("/series/"):
                return self.reply(200, {"series": {"ticker": r.split("/")[-1], "fee_type": "quadratic",
                                                   "fee_multiplier": 1}})
            if r == "/markets/orderbooks":
                tick = q.get("tickers", [])
                return self.reply(200, {"orderbooks": [{"ticker": t, "orderbook_fp": book_json(t)}
                                                       for t in tick if t in BOOKS]})
            if r.startswith("/markets/") and r.endswith("/orderbook"):
                t = r.split("/")[2]
                return self.reply(200, {"orderbook_fp": book_json(t)} if t in BOOKS else {})
            if r == "/portfolio/balance":
                if not self.authed("GET", p):
                    return
                return self.reply(200, {"balance": 100000, "balance_dollars": "1000.00",
                                        "portfolio_value": 0, "updated_ts": int(time.time())})
            if r == "/portfolio/positions":
                if not self.authed("GET", p):
                    return
                return self.reply(200, {"market_positions": [], "event_positions": [], "cursor": ""})
            if r == "/portfolio/orders":
                if not self.authed("GET", p):
                    return
                return self.reply(200, {"orders": [], "cursor": ""})
        return self.reply(404, {"error": {"code": "not_found", "message": r}})

    def do_POST(self):
        STATS["requests"] += 1
        p = urlparse(self.path).path
        n = int(self.headers.get("Content-Length", 0))
        body = json.loads(self.rfile.read(n) or b"{}")
        if p != PREFIX + "/portfolio/events/orders":
            return self.reply(404, {})
        if not self.authed("POST", p):
            return
        with LOCK:
            if RACE["ticker"] == body.get("ticker") and not RACE["done"]:
                RACE["done"] = True
                b = BOOKS[body["ticker"]]
                b["yes"] = b["yes"][1:] if RACE["mode"] == "thin" else []
                print(f"[mock] RACE: liquidity on {body['ticker']} taken by someone else", flush=True)
            got, avg = fill(body)
            ORDERS.append({**body, "filled": got})
            print(f"[mock] order {body['side']} {body['ticker']} x{body['count']} @ {body['price']} "
                  f"{body['time_in_force']} -> filled {got:.2f}", flush=True)
            return self.reply(201, {
                "order_id": f"mock-{len(ORDERS)}", "client_order_id": body.get("client_order_id"),
                "fill_count": f"{got:.2f}", "remaining_count": f"{float(body['count']) - got:.2f}",
                "average_fill_price": f"{avg:.4f}" if got else None,
                "average_fee_paid": "0.0160" if got else "0.0000", "ts_ms": int(time.time() * 1000)})


def main():
    global PUBKEY
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--pubkey", help="PEM public key used to verify request signatures")
    ap.add_argument("--race", help="ticker whose best YES bid vanishes just before our first order")
    ap.add_argument("--race-mode", choices=["thin", "empty"], default="thin")
    a = ap.parse_args()
    RACE["ticker"], RACE["mode"] = a.race, a.race_mode
    if a.pubkey:
        from cryptography.hazmat.primitives import serialization
        PUBKEY = serialization.load_pem_public_key(open(a.pubkey, "rb").read())
    srv = ThreadingHTTPServer(("127.0.0.1", a.port), Handler)
    print(f"[mock] Kalshi mock on http://127.0.0.1:{a.port}{PREFIX}", flush=True)
    import signal
    signal.signal(signal.SIGTERM, lambda *_: threading.Thread(target=srv.shutdown).start())
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        print(f"[mock] stats: {STATS}, orders: {len(ORDERS)}", flush=True)


if __name__ == "__main__":
    main()
