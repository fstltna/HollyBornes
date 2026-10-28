# Copyright (C) 2026 Marisa Giancarla
# SPDX-License-Identifier: GPL-3.0-or-later
import socket, subprocess, time, collections, sys

PORT = 5599
srv = subprocess.Popen([sys.argv[1] if len(sys.argv) > 1 else "./hollyserver", str(PORT)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
time.sleep(0.4)

class C:
    def __init__(s):
        s.s = socket.create_connection(("127.0.0.1", PORT)); s.s.settimeout(2); s.buf = b""
    def send(s, t): s.s.sendall(t.encode())
    def line(s):
        while b"\n" not in s.buf:
            d = s.s.recv(4096)
            if not d: return None
            s.buf += d
        l, s.buf = s.buf.split(b"\n", 1)
        return l.decode()
    def none(s):
        s.s.settimeout(0.4)
        try:
            d = s.s.recv(4096)
            return d == b""
        except socket.timeout:
            return True
        except ConnectionResetError:
            return True
        finally:
            s.s.settimeout(2)

ok = True
def check(cond, msg):
    global ok
    print(("PASS " if cond else "FAIL ") + msg)
    ok &= bool(cond)

try:
    # 1. FIND before HELLO
    x = C(); x.send("FIND\n"); check(x.line().startswith("ERR"), "FIND before HELLO rejected")

    # 2. basic match
    a, b = C(), C()
    a.send("HELLO Alice\r\nFIND\r\n")          # CRLF + two commands in one packet
    check(a.line().startswith("WELCOME"), "A welcomed"); check(a.line() == "WAIT", "A waits")
    b.send("HELLO Bob smith\n"); check(b.line().startswith("WELCOME"), "B welcomed")
    b.send("FI"); time.sleep(0.1); b.send("ND\n")   # split across packets
    ma, mb = a.line(), b.line()
    check(ma.startswith("MATCH ") and mb.startswith("MATCH "), "both matched")
    pa, pb = ma.split(" "), mb.split(" ")
    check({pa[1], pb[1]} == {"0", "1"}, "seats are 0 and 1")
    check(pa[2] == "Bob" and pb[2] == "Alice", "opponent names exchanged (spaces stripped from name)")
    check(pa[3] == pb[3], "identical deck")
    deck = list(map(int, pa[3].split(",")))
    cnt = collections.Counter(deck)
    exp = {0:10,1:10,2:10,3:12,4:4,5:3,6:3,7:3,8:4,9:5,10:6,11:6,12:6,13:6,14:14,15:1,16:1,17:1,18:1}
    check(len(deck) == 106 and dict(cnt) == exp, "deck has exactly 106 cards with correct distribution")

    # 3. relay
    a.send("MOVE P 3\n"); check(b.line() == "OPP P 3", "move relayed A->B")
    b.send("MOVE D 6\n"); check(a.line() == "OPP D 6", "move relayed B->A")
    b.send("MOVE X\n");   check(a.line() == "OPP X", "pass relayed")
    a.send("MOVE P 9\n"); check(a.line().startswith("ERR"), "bad slot rejected")
    a.send("MOVE Z 1\n"); check(a.line().startswith("ERR"), "bad kind rejected")
    a.send("FIND\n");     check(a.line().startswith("ERR"), "FIND during match rejected")

    # 4. leave -> OPPLEFT, then rematch possible
    a.send("LEAVE\n"); check(b.line() == "OPPLEFT", "OPPLEFT delivered on LEAVE")
    a.send("FIND\n"); time.sleep(0.2); b.send("FIND\n")
    check(a.line() == "WAIT", "A waits (queued first)")
    mb = b.line(); ma = a.line()
    check(mb.startswith("MATCH") and ma.startswith("MATCH"), "rematch works after leave")
    check(ma.split(" ")[3] == mb.split(" ")[3] and ma.split(" ")[3] != pa[3], "fresh, different deck for the rematch")

    # 5. disconnect mid-match
    a.s.close(); check(b.line() == "OPPLEFT", "OPPLEFT delivered on disconnect")

    # 6. rooms
    p, q, r = C(), C(), C()
    for c, n in ((p, "P"), (q, "Q"), (r, "R")): c.send("HELLO %s\n" % n); c.line()
    p.send("FIND secret\n"); check(p.line() == "WAIT", "P waits in room")
    q.send("FIND\n");        check(q.line() == "WAIT", "Q (no room) does not match room player")
    r.send("FIND secret\n")
    check(r.line().startswith("MATCH") and p.line().startswith("MATCH"), "same room code matches")
    check(q.none(), "Q still waiting")
    # cancel
    q.send("CANCEL\n"); q.send("FIND\n"); check(q.line() == "WAIT", "cancel then FIND again works")

    # 7. unknown / overlong
    q.send("BOGUS\n"); check(q.line().startswith("ERR"), "unknown command rejected")
    z = C(); z.send("HELLO Z\n"); z.line(); z.send("A" * 5000)
    time.sleep(0.3); check(z.none(), "overlong line drops client")
    q.send("PING\n"); check(q.line() == "PONG", "server still alive after abuse")
finally:
    srv.terminate()
    out = srv.communicate(timeout=3)[0]
    print("--- server log (tail) ---"); print("\n".join(out.splitlines()[-8:]))
print("ALL PASSED" if ok else "SOME FAILED"); sys.exit(0 if ok else 1)
