#!/usr/bin/env python3
# Copyright (C) 2026 Marisa Giancarla
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for the optional account mode: login, game records, friends, persistence.
Usage: python3 test_accounts.py [path/to/hollyserver]"""
import os, shutil, socket, subprocess, sys, tempfile, time

BIN = sys.argv[1] if len(sys.argv) > 1 else "./hollyserver"
PORT = 5598
DATA = tempfile.mkdtemp(prefix="hollydb")
ok = True

def check(cond, msg):
    global ok
    print(("PASS " if cond else "FAIL ") + msg)
    ok &= bool(cond)

def start(*extra):
    p = subprocess.Popen([BIN, str(PORT), *extra], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    time.sleep(0.4)
    return p

def stop(p):
    p.terminate()
    return p.communicate(timeout=3)[0]

class C:
    def __init__(s):
        s.s = socket.create_connection(("127.0.0.1", PORT)); s.s.settimeout(2); s.buf = b""
    def send(s, t): s.s.sendall((t + "\n").encode())
    def line(s):
        while b"\n" not in s.buf:
            d = s.s.recv(4096)
            if not d: return None
            s.buf += d
        l, s.buf = s.buf.split(b"\n", 1)
        return l.decode()
    def until(s, end):
        out = []
        while True:
            l = s.line()
            out.append(l)
            if l is None or l.startswith(end): return out
    def wait(s, prefix):
        """read lines (skipping notifications) until one starts with prefix"""
        try:
            while True:
                l = s.line()
                if l is None or l.startswith(prefix): return l
        except socket.timeout:
            return None
    def closed(s):
        """drain pending data; True if the server closed the connection"""
        s.s.settimeout(1)
        try:
            while True:
                if s.s.recv(4096) == b"": return True
        except (socket.timeout): return False
        except ConnectionResetError: return True
    def quiet(s):
        s.s.settimeout(0.3)
        try: return s.s.recv(4096) == b""
        except socket.timeout: return True
        except ConnectionResetError: return True
        finally: s.s.settimeout(2)
    def login(s, name, pw):
        s.send("LOGIN %s %s" % (name, pw)); return s.line(), s.line()

def play_match(a, b):
    """a and b are logged in; returns (a_line, b_line) of MATCH"""
    a.send("FIND"); b.send("FIND")
    la, lb = a.line(), b.line()
    # whoever waited first got WAIT before MATCH
    if la == "WAIT": la = a.line()
    if lb == "WAIT": lb = b.line()
    return la, lb

# ---------------------------------------------------------------- accounts off
srv = start()
x = C(); x.send("LOGIN al secret1"); check(x.line().startswith("ERR"), "LOGIN refused when accounts are off")
x.send("HELLO g1"); x.line(); x.send("STATS"); check(x.line().startswith("ERR"), "STATS refused when accounts are off")
stop(srv)

# ---------------------------------------------------------------- accounts on
srv = start("-d", DATA)
try:
    a = C(); w, l = a.login("alice", "pw-alice")
    check(w.startswith("WELCOME") and l == "LOGGEDIN alice 0 0 0 0 0 NEW", "new account created on first LOGIN")
    short = C(); short.send("LOGIN bobby ab"); check(short.line().startswith("ERR"), "too-short password rejected")
    dup = C(); dup.send("LOGIN alice pw-alice"); check(dup.line().startswith("ERR"), "second login of same account refused")
    guest = C(); guest.send("HELLO ALICE"); check(guest.line().startswith("ERR"), "guest cannot take a registered name (case-insensitive)")
    guest.send("HELLO visitor"); check(guest.line().startswith("WELCOME"), "guest with a free name is fine")
    b = C(); b.login("bob", "pw-bob")
    wrong = C(); wrong.send("LOGIN bob nope1234"); check(wrong.line() == "ERR wrong password", "wrong password rejected")
    for _ in range(5): wrong.send("LOGIN bob nope1234")
    check(wrong.closed(), "client dropped after repeated bad passwords")

    # ---- game 1: both report the same result -> recorded for both
    ma, mb = play_match(a, b)
    check(ma.startswith("MATCH") and mb.startswith("MATCH"), "alice and bob matched")
    sa = int(ma.split()[1])           # alice's seat
    a.send("RESULT 1000 650"); b.send("RESULT 650 1000")
    a.send("LEAVE"); b.send("LEAVE")
    time.sleep(0.2)
    a.send("STATS"); check(a.wait("STATS") == "STATS alice 1 1 0 0 0", "winner stats: 1 played, 1 won")
    b.send("STATS alice"); check(b.wait("STATS") == "STATS alice 1 1 0 0 0", "anyone can look up stats by name")
    b.send("STATS"); check(b.wait("STATS") == "STATS bob 1 0 1 0 0", "loser stats: 1 played, 1 lost")
    a.send("HISTORY"); h = a.until("ENDHISTORY")
    gm = [x for x in h if x.startswith("GAME")]
    check(len(gm) == 1 and gm[0].split()[2:] == ["bob", "1000", "650", "W"], "history shows the game with opponent and km")

    # ---- game 2: alice leaves mid-game without a result -> forfeit
    play_match(a, b)
    a.send("LEAVE")
    check(b.wait("OPPLEFT") == "OPPLEFT", "opponent told about the leave")
    b.send("LEAVE")
    time.sleep(0.2)
    a.send("STATS"); check(a.wait("STATS") == "STATS alice 2 1 1 0 1", "leaver gets a loss and a forfeit")
    b.send("STATS"); check(b.wait("STATS") == "STATS bob 2 1 1 0 0", "other player gets the win")

    # ---- game 3: reports disagree -> played, no winner
    play_match(a, b)
    a.send("RESULT 1000 500"); b.send("RESULT 1000 500")
    a.send("LEAVE"); b.send("LEAVE"); time.sleep(0.2)
    a.send("STATS"); check(a.wait("STATS") == "STATS alice 3 1 1 0 1", "disputed game counts as played only")

    # ---- game 4: finished game then drawn, plus one player disappears after reporting
    play_match(a, b)
    a.send("RESULT 400 400"); b.send("RESULT 400 400")
    a.send("LEAVE"); b.send("LEAVE"); time.sleep(0.2)
    a.send("STATS"); check(a.wait("STATS") == "STATS alice 4 1 1 1 1", "draw recorded")

    # ---- game 5: only one side reports, then both leave -> single report is used
    play_match(a, b)
    a.send("RESULT 1000 300"); a.send("LEAVE")
    check(b.wait("OPPLEFT") == "OPPLEFT", "opponent notified")
    b.send("LEAVE"); time.sleep(0.2)
    a.send("STATS"); check(a.wait("STATS") == "STATS alice 5 2 1 1 1", "single report accepted when the other side never reports")

    # ---- leaderboard
    a.send("LEADERS"); lb = a.until("ENDLEADERS")
    lb = [x for x in lb if x.startswith("LEADER")]
    check(lb[0].startswith("LEADER 1 alice 2 5") and lb[1].startswith("LEADER 2 bob"), "leaderboard ordered by wins")

    # ---- friends
    c = C(); c.login("carol", "pw-carol")
    guest.send("FRIENDS"); check(guest.line().startswith("ERR"), "guests cannot use friends")
    a.send("FRIEND ADD alice"); check(a.wait("ERR") is not None, "cannot friend yourself")
    a.send("FRIEND ADD nobody"); check(a.wait("ERR") is not None, "unknown player rejected")
    a.send("FRIEND ADD bob"); check(a.wait("OK") is not None, "request sent")
    check(b.wait("FRIENDREQ") == "FRIENDREQ alice", "online target is notified of the request")
    a.send("FRIEND ADD bob"); check(a.wait("ERR") is not None, "duplicate request rejected")
    b.send("FRIENDS"); r = b.until("ENDFRIENDS")
    check("REQUEST alice" in r, "request appears in the target's list")
    b.send("FRIEND ACCEPT alice"); check(b.wait("OK") is not None, "request accepted")
    time.sleep(0.2)
    a.send("FRIENDS"); r = a.until("ENDFRIENDS")
    check("FRIEND bob online" in r and "FRIENDADDED bob" in r, "both sides are friends; bob shown online")
    # mutual add shortcut: carol adds alice, alice adds carol -> instant friendship
    c.send("FRIEND ADD alice"); c.wait("OK"); a.wait("FRIENDREQ")
    a.send("FRIEND ADD carol"); a.wait("OK")
    a.send("FRIENDS"); r = a.until("ENDFRIENDS")
    check("FRIEND carol online" in r, "adding someone who already asked you makes you friends at once")
    # presence and status
    c.s.close(); time.sleep(0.3)
    a.send("FRIENDS"); r = a.until("ENDFRIENDS")
    check("FRIEND carol offline" in r, "friend shows offline after disconnect")
    a.send("FRIEND REMOVE bob"); check(a.wait("OK") is not None, "friend removed")
    check(b.wait("FRIENDREMOVED") == "FRIENDREMOVED alice", "removed friend is told")
finally:
    out1 = stop(srv)

# ---------------------------------------------------------------- persistence
db = open(os.path.join(DATA, "accounts.db")).read()
check("pw-alice" not in db and "pw-bob" not in db, "database stores no plaintext passwords")
check(os.path.getsize(os.path.join(DATA, "games.log")) > 0, "games.log archive written")
check((os.stat(os.path.join(DATA, "accounts.db")).st_mode & 0o077) == 0, "database file is private (0600)")

srv = start("-d", DATA)
try:
    a2 = C(); w, l = a2.login("alice", "pw-alice")
    check(l.startswith("LOGGEDIN alice 5 2 1 1 1 RETURNING"), "stats survive a server restart: " + str(l))
    a2.send("HISTORY"); h = a2.until("ENDHISTORY")
    check(len([x for x in h if x.startswith("GAME")]) == 5, "history survives restart (5 games)")
    a2.send("FRIENDS"); r = a2.until("ENDFRIENDS")
    check("FRIEND carol offline" in r and not any("bob" in x for x in r), "friend list survives restart")
    bad = C(); bad.send("LOGIN alice wrongpw1"); check(bad.line() == "ERR wrong password", "password still enforced after restart")
finally:
    stop(srv)

# ---------------------------------------------------------------- require-login
srv = start("-d", DATA, "--require-login")
try:
    g = C(); g.send("HELLO anon"); check(g.line().startswith("ERR"), "--require-login refuses anonymous players")
    u = C(); w, l = u.login("alice", "pw-alice"); check(l.startswith("LOGGEDIN"), "login still works with --require-login")
finally:
    stop(srv)

shutil.rmtree(DATA, ignore_errors=True)
print("ALL PASSED" if ok else "SOME FAILED")
sys.exit(0 if ok else 1)
