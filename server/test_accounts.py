#!/usr/bin/env python3
# Copyright (C) 2026 Marisa Giancarla
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for the optional account mode: login, game records, friends, SQLite persistence,
and the one-time import of the old text database.
Usage: python3 test_accounts.py [path/to/hollyserver]"""
import hashlib, os, shutil, socket, sqlite3, subprocess, sys, tempfile, time

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
    """a and b are logged in; returns their two MATCH lines (skipping WAIT and notifications)"""
    a.send("FIND"); b.send("FIND")
    return a.wait("MATCH"), b.wait("MATCH")

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
DBFILE = os.path.join(DATA, "hollybornes.db")
raw = open(DBFILE, "rb").read()
check(b"pw-alice" not in raw and b"pw-bob" not in raw and b"pw-carol" not in raw, "database file contains no plaintext passwords")
check((os.stat(DBFILE).st_mode & 0o077) == 0, "database file is private (0600)")
check(not os.path.exists(os.path.join(DATA, "accounts.db")) and not os.path.exists(os.path.join(DATA, "games.log")),
      "no old text files are written any more")
con = sqlite3.connect("file:%s?mode=ro" % DBFILE, uri=True)
check(con.execute("PRAGMA integrity_check").fetchone()[0] == "ok", "SQLite integrity check passes")
check(con.execute("SELECT COUNT(*) FROM players").fetchone()[0] == 3, "three accounts stored (alice, bob, carol)")
check(con.execute("SELECT COUNT(*) FROM games").fetchone()[0] == 5, "all five games stored in the games table")
row = con.execute("SELECT played, won, lost, drawn, forfeits FROM players WHERE name = 'alice'").fetchone()
check(row == (5, 2, 1, 1, 1), "stats row for alice matches what the server reported: %s" % (row,))
check(len(con.execute("SELECT hash FROM players").fetchone()[0]) == 64 and
      all(len(r[0]) == 32 for r in con.execute("SELECT salt FROM players")), "hashes and salts stored, not passwords")
fr = con.execute("SELECT COUNT(*) FROM friends").fetchone()[0]
check(fr == 2, "alice-carol friendship stored in both directions (%d rows)" % fr)
con.close()

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

# ---------------------------------------------------------------- import of the old text database
def legacy_hash(salt, pw):                      # the hash the text-file versions of the server used
    x = hashlib.sha256((salt + pw).encode()).digest()
    for _ in range(5000):
        x = hashlib.sha256(x + (salt + pw).encode()).digest()
    return x.hex()

LEG = tempfile.mkdtemp(prefix="hollyleg")
s1, s2, s3 = "00112233445566778899aabbccddeeff", "ffeeddccbbaa99887766554433221100", "0f1e2d3c4b5a69788796a5b4c3d2e1f0"
open(LEG + "/accounts.db", "w").write(
    "HOLLYBORNES-DB 1\n"
    "A dora %s %s 1700000000 7 4 2 1 0\n" % (s1, legacy_hash(s1, "dora-pass")) +
    "A eli %s %s 1700000100 3 1 2 0 1\n" % (s2, legacy_hash(s2, "eli-pass")) +
    "A fay %s %s 1700000200 0 0 0 0 0\n" % (s3, legacy_hash(s3, "fay-pass")) +
    "F dora eli\nF eli dora\nR fay dora\n"
    "G dora 1700000200 eli 1000 400 W\n")
open(LEG + "/games.log", "w").write(
    "1700000200 dora eli 1000 400 W L\n"
    "1700000300 eli dora 900 1000 L W\n"
    "1700000400 dora guestx 500 500 D D\n")

srv = start("-d", LEG)
try:
    d = C(); w, l = d.login("dora", "dora-pass")
    check(l == "LOGGEDIN dora 7 4 2 1 0 RETURNING", "old account imported and old password still works: " + str(l))
    d.send("FRIENDS"); r = d.until("ENDFRIENDS")
    check("FRIEND eli offline" in r and "REQUEST fay" in r, "old friends and open request imported")
    d.send("HISTORY"); h = [x.split()[2:] for x in d.until("ENDHISTORY") if x.startswith("GAME")]
    check(h == [["guestx", "500", "500", "D"], ["eli", "1000", "900", "W"], ["eli", "1000", "400", "W"]],
          "old games imported, newest first, seen from dora's side: " + str(h))
    bad = C(); bad.send("LOGIN dora wrongpass1"); check(bad.line() == "ERR wrong password", "wrong password still refused after import")
finally:
    out = stop(srv)
check("import done: 3 accounts, 2 friendships, 1 requests, 3 games" in out, "server logged the import summary")
check(os.path.exists(LEG + "/accounts.db.migrated") and os.path.exists(LEG + "/games.log.migrated") and
      not os.path.exists(LEG + "/accounts.db"), "old files renamed to *.migrated (kept as a backup)")

srv = start("-d", LEG)                                      # second start must not import again
try:
    d = C(); w, l = d.login("dora", "dora-pass")
    check(l.startswith("LOGGEDIN dora 7 4 2 1 0"), "stats unchanged after a second start")
finally:
    out = stop(srv)
check("importing" not in out, "old files are not imported twice")
con = sqlite3.connect("file:%s/hollybornes.db?mode=ro" % LEG, uri=True)
check(con.execute("SELECT COUNT(*) FROM players").fetchone()[0] == 3 and
      con.execute("SELECT COUNT(*) FROM games").fetchone()[0] == 3, "no duplicate rows after restart")
con.close()
shutil.rmtree(LEG, ignore_errors=True)

# a data directory with a corrupt/foreign accounts.db must not be imported or damaged
BADD = tempfile.mkdtemp(prefix="hollybad")
open(BADD + "/accounts.db", "w").write("this is not a HollyBornes database\n")
srv = start("-d", BADD)
try:
    z = C(); w, l = z.login("zed", "zed-pass")
    check(l.startswith("LOGGEDIN zed 0 0 0 0 0 NEW"), "server starts normally next to an unrelated accounts.db")
finally:
    stop(srv)
check(open(BADD + "/accounts.db").read().startswith("this is not"), "the unrelated file was left untouched")
shutil.rmtree(BADD, ignore_errors=True)

# ---------------------------------------------------------------- crash safety
CR = tempfile.mkdtemp(prefix="hollycrash")
srv = start("-d", CR)
a, b = C(), C()
a.login("anna", "crash-pw1"); b.login("ben", "crash-pw2")
a.send("FRIEND ADD ben"); a.wait("OK"); b.wait("FRIENDREQ"); b.send("FRIEND ACCEPT anna"); b.wait("OK")
for i in range(3):
    play_match(a, b)
    a.send("RESULT 1000 %d" % (100 * i)); b.send("RESULT %d 1000" % (100 * i))
    a.send("LEAVE"); b.send("LEAVE"); time.sleep(0.15)
a.send("STATS"); before = a.wait("STATS")
srv.kill(); srv.wait()                                  # SIGKILL: no clean shutdown, like a crash or power cut
srv = start("-d", CR)
try:
    a2 = C(); w, l = a2.login("anna", "crash-pw1")
    a2.send("FRIENDS"); fr = a2.until("ENDFRIENDS")
    check(before == "STATS anna 3 3 0 0 0" and l.startswith("LOGGEDIN anna 3 3 0 0 0"),
          "recorded games survive kill -9 (%s)" % before)
    check("FRIEND ben offline" in fr, "friendships survive kill -9")
finally:
    stop(srv)
shutil.rmtree(CR, ignore_errors=True)

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
