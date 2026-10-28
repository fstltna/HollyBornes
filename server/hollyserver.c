/*
 * hollyserver.c  -  matchmaking, relay and (optionally) accounts for HollyBornes
 *
 * What it does
 *   - accepts TCP clients speaking a tiny line-based text protocol
 *     (see PROTOCOL.md)
 *   - pairs two players who ask for a match (same room code, or both
 *     without one) and shuffles the 106-card deck for them, so neither
 *     client can rig the draw order
 *   - relays each player's moves to the other player
 *
 * Optional account mode (-d <dir>)
 *   - registered players log in with a name + password
 *   - finished games are recorded: totals, a short history per player and an
 *     append-only archive (games.log)
 *   - players can send friend requests, see which friends are online, and
 *     look at stats and a leaderboard
 *
 * The server does NOT know the rules of the game. Both clients run the same
 * rules engine on the same deck and apply the same moves in the same order.
 * Results are reported by both clients and cross-checked (see PROTOCOL.md).
 *
 * Build:  cc -O2 -Wall -Wextra -o hollyserver hollyserver.c
 * Run:    ./hollyserver [port] [-d datadir] [--require-login]
 *         ./hollyserver --selftest
 *
 * Portable POSIX C (Linux, macOS, BSD). Single thread, poll() based.
 * On Windows use WSL, or port the socket calls to Winsock.
 *
 * Copyright (C) 2026 Marisa Giancarla
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version. It is distributed WITHOUT ANY WARRANTY; without even the
 * implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details:
 * https://www.gnu.org/licenses/
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0 /* SIGPIPE is ignored explicitly below */
#endif

#define DEFAULT_PORT   5555
#define MAX_CLIENTS    256
#define IN_SIZE        1024
#define OUT_SIZE       16384
#define NAME_LEN       16
#define PASS_MIN       4
#define PASS_MAX       32
#define HELLO_TIMEOUT  30   /* seconds a new connection may stay silent        */
#define REPORT_TIMEOUT 120  /* seconds to wait for the second result report    */
#define MAX_LOGIN_FAILS 5
#define DECK_SIZE      106
#define HAND_SLOTS     7

#define MAX_ACCOUNTS   4096
#define MAX_FRIENDS    50
#define MAX_REQS       20
#define HIST_LEN       10
#define HASH_ROUNDS    5000

/* =========================================================================
 *  SHA-256 (used for password hashing)
 * ========================================================================= */

typedef struct {
    uint32_t h[8];
    uint64_t len;
    uint8_t  buf[64];
    size_t   n;
} Sha;

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

#define ROR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha_block(Sha *s, const uint8_t *p)
{
    uint32_t w[64], a, b, c, d, e, f, g, h, t1, t2;
    int i;

    for (i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) |
               ((uint32_t)p[4 * i + 2] << 8) | (uint32_t)p[4 * i + 3];
    for (i = 16; i < 64; i++) {
        uint32_t s0 = ROR32(w[i - 15], 7) ^ ROR32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR32(w[i - 2], 17) ^ ROR32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3];
    e = s->h[4]; f = s->h[5]; g = s->h[6]; h = s->h[7];
    for (i = 0; i < 64; i++) {
        t1 = h + (ROR32(e, 6) ^ ROR32(e, 11) ^ ROR32(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        t2 = (ROR32(a, 2) ^ ROR32(a, 13) ^ ROR32(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
    s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

static void sha_init(Sha *s)
{
    static const uint32_t iv[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };
    memcpy(s->h, iv, sizeof iv);
    s->len = 0;
    s->n = 0;
}

static void sha_update(Sha *s, const void *data, size_t len)
{
    const uint8_t *p = data;

    s->len += len;
    while (len > 0) {
        size_t take = 64 - s->n;
        if (take > len)
            take = len;
        memcpy(s->buf + s->n, p, take);
        s->n += take;
        p += take;
        len -= take;
        if (s->n == 64) {
            sha_block(s, s->buf);
            s->n = 0;
        }
    }
}

static void sha_final(Sha *s, uint8_t out[32])
{
    uint64_t bits = s->len * 8;
    uint8_t pad = 0x80, zero = 0, lenb[8];
    int i;

    sha_update(s, &pad, 1);
    while (s->n != 56)
        sha_update(s, &zero, 1);
    for (i = 0; i < 8; i++)
        lenb[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha_update(s, lenb, 8);
    for (i = 0; i < 8; i++) {
        out[4 * i]     = (uint8_t)(s->h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(s->h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(s->h[i] >> 8);
        out[4 * i + 3] = (uint8_t)(s->h[i]);
    }
}

static void to_hex(const uint8_t *in, size_t n, char *out)
{
    static const char hx[] = "0123456789abcdef";
    size_t i;
    for (i = 0; i < n; i++) {
        out[2 * i]     = hx[in[i] >> 4];
        out[2 * i + 1] = hx[in[i] & 15];
    }
    out[2 * n] = '\0';
}

/* salted + iterated SHA-256; out must hold 65 bytes */
static void hash_password(const char *salt_hex, const char *password, char *out)
{
    uint8_t x[32];
    Sha s;
    int i;

    sha_init(&s);
    sha_update(&s, salt_hex, strlen(salt_hex));
    sha_update(&s, password, strlen(password));
    sha_final(&s, x);
    for (i = 0; i < HASH_ROUNDS; i++) {
        sha_init(&s);
        sha_update(&s, x, 32);
        sha_update(&s, salt_hex, strlen(salt_hex));
        sha_update(&s, password, strlen(password));
        sha_final(&s, x);
    }
    to_hex(x, 32, out);
}

static int selftest(void)
{
    uint8_t d[32];
    char hex[65];
    Sha s;

    sha_init(&s);
    sha_update(&s, "abc", 3);
    sha_final(&s, d);
    to_hex(d, 32, hex);
    if (strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") != 0) {
        printf("selftest FAILED: sha256(abc) = %s\n", hex);
        return 1;
    }
    sha_init(&s);
    sha_update(&s, "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56);
    sha_final(&s, d);
    to_hex(d, 32, hex);
    if (strcmp(hex, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1") != 0) {
        printf("selftest FAILED: sha256(two-block) = %s\n", hex);
        return 1;
    }
    printf("selftest ok\n");
    return 0;
}

/* =========================================================================
 *  Data structures
 * ========================================================================= */

enum state {
    ST_FREE = 0,
    ST_NEW,     /* connected, no HELLO/LOGIN yet      */
    ST_LOBBY,   /* introduced itself, idle            */
    ST_QUEUED,  /* waiting for an opponent            */
    ST_PLAYING  /* in a match                         */
};

typedef struct {
    int    fd;
    int    state;
    int    opp;                /* index of opponent while PLAYING, else -1 */
    int    dead;               /* drop at the end of this loop iteration   */
    int    acct;               /* account index, -1 for guests             */
    int    match;              /* index into matches[], -1 if none         */
    int    side;               /* 0/1 side in that match (= seat)          */
    int    fails;              /* failed logins                            */
    char   name[NAME_LEN + 1];
    char   room[NAME_LEN + 1]; /* room code while QUEUED ("" = random)      */
    unsigned long qseq;        /* queue ticket: lowest waits longest        */
    time_t connected;
    char   in[IN_SIZE];
    int    inlen;
    char   out[OUT_SIZE];
    int    outlen;
    char   addr[48];
} Client;

typedef struct {
    time_t t;
    int    km, okm;
    char   res;                /* W L D F(orfeit) X(disputed) */
    char   opp[NAME_LEN + 1];
} GameRec;

typedef struct {
    char    name[NAME_LEN + 1];
    char    salt[33];
    char    hash[65];
    time_t  created;
    int     played, won, lost, drawn, forfeits;
    int     friends[MAX_FRIENDS];
    int     nfriends;
    int     reqs[MAX_REQS];    /* incoming friend requests (account indices) */
    int     nreqs;
    GameRec hist[HIST_LEN];    /* newest first */
    int     nhist;
    int     online;            /* client index or -1 */
} Account;

typedef struct {
    int    used, done, refs;
    int    acct[2];
    char   name[2][NAME_LEN + 1];
    int    left[2], reported[2];
    int    km[2], okm[2];      /* what each side reported: my km, opponent km */
    time_t first_report;
} Match;

static Client  clients[MAX_CLIENTS];
static Match   matches[MAX_CLIENTS];
static Account accts[MAX_ACCOUNTS];
static int     nacc = 0;

static volatile sig_atomic_t g_stop = 0;
static unsigned long g_qseq = 0;
static unsigned long g_matches = 0;
static char g_dir[256] = "";          /* data directory; empty = accounts off */
static int  g_accounts = 0;
static int  g_require_login = 0;
static int  g_dirty = 0;

/* ------------------------------------------------------------------ log */

static void logf_(const char *fmt, ...)
{
    char ts[32];
    time_t now = time(NULL);
    struct tm tm;
    va_list ap;

    localtime_r(&now, &tm);
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tm);
    printf("[%s] ", ts);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}

/* --------------------------------------------------------------- random */

static void seed_random(void)
{
    unsigned int seed = 0;
    FILE *f = fopen("/dev/urandom", "rb");

    if (f) {
        if (fread(&seed, sizeof seed, 1, f) != 1)
            seed = 0;
        fclose(f);
    }
    if (seed == 0)
        seed = (unsigned int)time(NULL) ^ ((unsigned int)getpid() << 16);
    srandom(seed);
}

/* 16 random bytes as 32 hex characters */
static void random_salt(char *out)
{
    uint8_t b[16];
    FILE *f = fopen("/dev/urandom", "rb");
    int i, ok = 0;

    if (f) {
        ok = fread(b, 1, sizeof b, f) == sizeof b;
        fclose(f);
    }
    if (!ok)
        for (i = 0; i < 16; i++)
            b[i] = (uint8_t)(random() & 0xff);
    to_hex(b, sizeof b, out);
}

static int card_count(int c)
{
    if (c <= 2)  return 10;               /* 25, 50, 75      */
    if (c == 3)  return 12;               /* 100             */
    if (c == 4)  return 4;                /* 200             */
    if (c <= 7)  return 3;                /* accident, gas, tire */
    if (c == 8)  return 4;                /* speed limit     */
    if (c == 9)  return 5;                /* stop            */
    if (c <= 13) return 6;                /* repairs, gasoline, spare tire, end of limit */
    if (c == 14) return 14;               /* go              */
    return 1;                             /* four safeties   */
}

/* Builds the shuffled deck as "n,n,n,..." (106 numbers, 0..18). */
static void make_deck(char *out, size_t outsz)
{
    int deck[DECK_SIZE];
    int n = 0, c, k, i, j, t;
    size_t pos = 0;

    for (c = 0; c <= 18; c++)
        for (k = 0; k < card_count(c); k++)
            deck[n++] = c;

    for (i = n - 1; i > 0; i--) {          /* Fisher-Yates */
        j = (int)(random() % (i + 1));
        t = deck[i]; deck[i] = deck[j]; deck[j] = t;
    }

    out[0] = '\0';
    for (i = 0; i < n; i++) {
        int w = snprintf(out + pos, outsz - pos, i ? ",%d" : "%d", deck[i]);
        if (w < 0 || (size_t)w >= outsz - pos)
            break;
        pos += (size_t)w;
    }
}

/* -------------------------------------------------------------- sending */

static void vqueue_line(Client *c, const char *fmt, va_list ap)
{
    char tmp[1024];
    int n;

    if (c->fd < 0 || c->dead)
        return;
    n = vsnprintf(tmp, sizeof tmp - 1, fmt, ap);
    if (n < 0)
        return;
    if (n > (int)sizeof tmp - 2)
        n = (int)sizeof tmp - 2;
    tmp[n++] = '\n';
    if (c->outlen + n > OUT_SIZE) {      /* client is not reading: drop it */
        c->dead = 1;
        return;
    }
    memcpy(c->out + c->outlen, tmp, (size_t)n);
    c->outlen += n;
}

static void queue_line(Client *c, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vqueue_line(c, fmt, ap);
    va_end(ap);
}

static void flush_client(Client *c)
{
    while (c->outlen > 0 && !c->dead) {
        ssize_t n = send(c->fd, c->out, (size_t)c->outlen, MSG_NOSIGNAL);
        if (n > 0) {
            memmove(c->out, c->out + n, (size_t)(c->outlen - n));
            c->outlen -= (int)n;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            c->dead = 1;
        }
    }
}

/* message to whoever is logged in on account ai (if anyone) */
static void notify_acct(int ai, const char *fmt, ...)
{
    va_list ap;

    if (ai < 0 || accts[ai].online < 0)
        return;
    va_start(ap, fmt);
    vqueue_line(&clients[accts[ai].online], fmt, ap);
    va_end(ap);
}

/* =========================================================================
 *  Account database
 * ========================================================================= */

static int find_acct(const char *name)
{
    int i;
    if (!name)
        return -1;
    for (i = 0; i < nacc; i++)
        if (strcasecmp(accts[i].name, name) == 0)
            return i;
    return -1;
}

static int list_has(const int *a, int n, int v)
{
    int i;
    for (i = 0; i < n; i++)
        if (a[i] == v)
            return 1;
    return 0;
}

static void list_del(int *a, int *n, int v)
{
    int i;
    for (i = 0; i < *n; i++)
        if (a[i] == v) {
            memmove(&a[i], &a[i + 1], (size_t)(*n - i - 1) * sizeof a[0]);
            (*n)--;
            return;
        }
}

static void db_path(char *out, size_t sz, const char *file)
{
    snprintf(out, sz, "%s/%s", g_dir, file);
}

static void db_save(void)
{
    char path[320], tmp[330];
    FILE *f;
    int i, j;

    if (!g_accounts)
        return;
    db_path(path, sizeof path, "accounts.db");
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    f = fopen(tmp, "w");
    if (!f) {
        logf_("cannot write %s: %s", tmp, strerror(errno));
        return;
    }
    fprintf(f, "HOLLYBORNES-DB 1\n");
    for (i = 0; i < nacc; i++) {
        Account *a = &accts[i];
        fprintf(f, "A %s %s %s %ld %d %d %d %d %d\n", a->name, a->salt, a->hash,
                (long)a->created, a->played, a->won, a->lost, a->drawn, a->forfeits);
    }
    for (i = 0; i < nacc; i++) {
        Account *a = &accts[i];
        for (j = 0; j < a->nfriends; j++)
            fprintf(f, "F %s %s\n", a->name, accts[a->friends[j]].name);
        for (j = 0; j < a->nreqs; j++)
            fprintf(f, "R %s %s\n", accts[a->reqs[j]].name, a->name);
        for (j = 0; j < a->nhist; j++)
            fprintf(f, "G %s %ld %s %d %d %c\n", a->name, (long)a->hist[j].t,
                    a->hist[j].opp, a->hist[j].km, a->hist[j].okm, a->hist[j].res);
    }
    fflush(f);
    fsync(fileno(f));
    if (fclose(f) != 0 || rename(tmp, path) != 0)
        logf_("saving %s failed: %s", path, strerror(errno));
    g_dirty = 0;
}

static void db_load(void)
{
    char path[320], line[512];
    FILE *f;
    int i;

    for (i = 0; i < MAX_ACCOUNTS; i++)
        accts[i].online = -1;
    db_path(path, sizeof path, "accounts.db");
    f = fopen(path, "r");
    if (!f) {
        logf_("no existing database in %s, starting empty", g_dir);
        return;
    }
    if (!fgets(line, sizeof line, f) || strncmp(line, "HOLLYBORNES-DB", 14) != 0) {
        logf_("%s is not a HollyBornes database - ignoring it", path);
        fclose(f);
        return;
    }
    while (fgets(line, sizeof line, f)) {
        char n1[64], n2[64], n3[96], n4[96], rc = 0;
        long t;
        int a1, a2, a3, a4, a5, km, okm;

        if (line[0] == 'A' && nacc < MAX_ACCOUNTS &&
            sscanf(line, "A %16s %32s %64s %ld %d %d %d %d %d", n1, n3, n4, &t,
                   &a1, &a2, &a3, &a4, &a5) == 9) {
            Account *a = &accts[nacc++];
            snprintf(a->name, sizeof a->name, "%.16s", n1);
            snprintf(a->salt, sizeof a->salt, "%.32s", n3);
            snprintf(a->hash, sizeof a->hash, "%.64s", n4);
            a->created = (time_t)t;
            a->played = a1; a->won = a2; a->lost = a3; a->drawn = a4; a->forfeits = a5;
        } else if (line[0] == 'F' && sscanf(line, "F %16s %16s", n1, n2) == 2) {
            int x = find_acct(n1), y = find_acct(n2);
            if (x >= 0 && y >= 0 && accts[x].nfriends < MAX_FRIENDS &&
                !list_has(accts[x].friends, accts[x].nfriends, y))
                accts[x].friends[accts[x].nfriends++] = y;
        } else if (line[0] == 'R' && sscanf(line, "R %16s %16s", n1, n2) == 2) {
            int x = find_acct(n1), y = find_acct(n2);
            if (x >= 0 && y >= 0 && accts[y].nreqs < MAX_REQS &&
                !list_has(accts[y].reqs, accts[y].nreqs, x))
                accts[y].reqs[accts[y].nreqs++] = x;
        } else if (line[0] == 'G' &&
                   sscanf(line, "G %16s %ld %16s %d %d %c", n1, &t, n2, &km, &okm, &rc) == 6) {
            int x = find_acct(n1);
            if (x >= 0 && accts[x].nhist < HIST_LEN) {
                GameRec *g = &accts[x].hist[accts[x].nhist++];
                g->t = (time_t)t; g->km = km; g->okm = okm; g->res = rc;
                snprintf(g->opp, sizeof g->opp, "%.16s", n2);
            }
        }
    }
    fclose(f);
    logf_("loaded %d accounts from %s", nacc, path);
}

static void record_game(int ai, const char *opp, int km, int okm, char res)
{
    Account *a = &accts[ai];
    GameRec *g;

    a->played++;
    switch (res) {
    case 'W': a->won++; break;
    case 'L': a->lost++; break;
    case 'D': a->drawn++; break;
    case 'F': a->lost++; a->forfeits++; break;
    default:  break;
    }
    memmove(&a->hist[1], &a->hist[0], sizeof(GameRec) * (HIST_LEN - 1));
    g = &a->hist[0];
    g->t = time(NULL);
    g->km = km;
    g->okm = okm;
    g->res = res;
    snprintf(g->opp, sizeof g->opp, "%s", opp);
    if (a->nhist < HIST_LEN)
        a->nhist++;
    g_dirty = 1;
}

/* =========================================================================
 *  Matches and result reporting
 * ========================================================================= */

/* mode 0: both reports agree, 1: single report from 'who', 2: 'who' forfeited,
 * 3: reports disagree (recorded as played, no winner) */
static void finish_match(Match *m, int mode, int who)
{
    char res[2];
    int km[2], okm[2], s;

    if (m->done)
        return;
    m->done = 1;

    if (mode == 2) {
        res[who] = 'F';
        res[1 - who] = 'W';
        km[0] = km[1] = okm[0] = okm[1] = 0;
    } else {
        if (mode == 1) {
            km[who] = m->km[who];
            okm[who] = m->okm[who];
            km[1 - who] = okm[who];
            okm[1 - who] = km[who];
        } else {
            for (s = 0; s < 2; s++) {
                km[s] = m->km[s];
                okm[s] = m->okm[s];
            }
        }
        for (s = 0; s < 2; s++)
            res[s] = km[s] > okm[s] ? 'W' : km[s] < okm[s] ? 'L' : 'D';
        if (mode == 3)
            res[0] = res[1] = 'X';
    }

    if (g_accounts) {
        for (s = 0; s < 2; s++)
            if (m->acct[s] >= 0)
                record_game(m->acct[s], m->name[1 - s], km[s], okm[s], res[s]);
        {
            char path[320];
            FILE *f;
            db_path(path, sizeof path, "games.log");
            f = fopen(path, "a");
            if (f) {
                fprintf(f, "%ld %s %s %d %d %c %c\n", (long)time(NULL), m->name[0], m->name[1],
                        km[0], km[1], res[0], res[1]);
                fclose(f);
            }
        }
    }
    logf_("game finished: %s %c / %s %c (mode %d)", m->name[0], res[0], m->name[1], res[1], mode);
}

/* a client reports "I have km, opponent has okm" */
static void match_report(Client *c, int km, int okm)
{
    Match *m;
    int s = c->side, o = 1 - c->side;

    if (c->match < 0)
        return;
    m = &matches[c->match];
    if (!m->used || m->done || m->reported[s])
        return;
    m->reported[s] = 1;
    m->km[s] = km;
    m->okm[s] = okm;
    if (!m->first_report)
        m->first_report = time(NULL);
    if (m->reported[o]) {
        if (m->km[0] == m->okm[1] && m->km[1] == m->okm[0])
            finish_match(m, 0, 0);
        else
            finish_match(m, 3, 0);
    }
}

/* the client stops being part of its match (LEAVE, new FIND, disconnect) */
static void match_detach(Client *c)
{
    Match *m;
    int s, o;

    if (c->match < 0)
        return;
    m = &matches[c->match];
    s = c->side;
    o = 1 - s;
    c->match = -1;
    if (!m->used)
        return;
    m->left[s] = 1;
    if (!m->done && !m->reported[s]) {
        if (m->reported[o])
            finish_match(m, 1, o);      /* trust the one report we have      */
        else
            finish_match(m, 2, s);      /* left mid-game without a result    */
    }
    if (--m->refs <= 0) {
        m->done = 1;
        m->used = 0;
    }
}

static void match_sweep(void)
{
    time_t now = time(NULL);
    int i;

    for (i = 0; i < MAX_CLIENTS; i++) {
        Match *m = &matches[i];
        if (m->used && !m->done && m->first_report && now - m->first_report > REPORT_TIMEOUT)
            finish_match(m, 1, m->reported[0] ? 0 : 1);
    }
}

/* ---------------------------------------------------------- bookkeeping */

static int count_online(void)
{
    int i, n = 0;
    for (i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].fd >= 0 && clients[i].state >= ST_LOBBY)
            n++;
    return n;
}

static int name_in_use(const char *name, int except)
{
    int i;
    for (i = 0; i < MAX_CLIENTS; i++)
        if (i != except && clients[i].fd >= 0 && clients[i].state >= ST_LOBBY &&
            strcasecmp(clients[i].name, name) == 0)
            return 1;
    return 0;
}

static void sanitize(char *dst, const char *src)
{
    size_t i = 0;

    if (!src)
        src = "";
    for (; *src && i < NAME_LEN; src++) {
        unsigned char ch = (unsigned char)*src;
        dst[i++] = (isalnum(ch) || ch == '_' || ch == '-' || ch == '.') ? (char)ch : '_';
    }
    dst[i] = '\0';
}

/* Takes client idx out of whatever it was doing (queue or match). */
static void leave_current(int idx)
{
    Client *c = &clients[idx];

    if (c->state == ST_QUEUED) {
        c->state = ST_LOBBY;
        c->room[0] = '\0';
        logf_("%s (%s) left the queue", c->name, c->addr);
    } else if (c->state == ST_PLAYING) {
        int oi = c->opp;
        c->state = ST_LOBBY;
        c->opp = -1;
        if (oi >= 0 && clients[oi].fd >= 0 && clients[oi].opp == idx) {
            Client *o = &clients[oi];
            o->state = ST_LOBBY;
            o->opp = -1;
            queue_line(o, "OPPLEFT");
        }
        logf_("%s left the match", c->name);
    }
    match_detach(c);
}

static void announce_presence(int ai, const char *what)
{
    int j;
    for (j = 0; j < accts[ai].nfriends; j++)
        notify_acct(accts[ai].friends[j], "%s %s", what, accts[ai].name);
}

static void drop_client(int idx)
{
    Client *c = &clients[idx];
    char name[NAME_LEN + 1], addr[sizeof c->addr];

    if (c->fd < 0)
        return;
    snprintf(name, sizeof name, "%s", c->name[0] ? c->name : "?");
    snprintf(addr, sizeof addr, "%s", c->addr);
    leave_current(idx);
    if (c->acct >= 0) {
        accts[c->acct].online = -1;
        announce_presence(c->acct, "FRIENDOFF");
    }
    flush_client(c);
    close(c->fd);
    memset(c, 0, sizeof *c);
    c->fd = -1;
    c->opp = -1;
    c->acct = -1;
    c->match = -1;
    logf_("disconnect %s (%s), %d online", name, addr, count_online());
}

static void start_match(int a, int b)
{
    char deck[512];
    Client *ca, *cb;
    Match *m = NULL;
    int mi;

    if (random() & 1) { int t = a; a = b; b = t; }  /* random seat order */
    ca = &clients[a];
    cb = &clients[b];

    for (mi = 0; mi < MAX_CLIENTS; mi++)
        if (!matches[mi].used) {
            m = &matches[mi];
            break;
        }
    if (m) {
        memset(m, 0, sizeof *m);
        m->used = 1;
        m->refs = 2;
        m->acct[0] = ca->acct;
        m->acct[1] = cb->acct;
        snprintf(m->name[0], sizeof m->name[0], "%s", ca->name);
        snprintf(m->name[1], sizeof m->name[1], "%s", cb->name);
        ca->match = cb->match = mi;
        ca->side = 0;
        cb->side = 1;
    }

    make_deck(deck, sizeof deck);

    ca->state = cb->state = ST_PLAYING;
    ca->opp = b;
    cb->opp = a;
    ca->room[0] = cb->room[0] = '\0';

    queue_line(ca, "MATCH 0 %s %s", cb->name, deck);
    queue_line(cb, "MATCH 1 %s %s", ca->name, deck);
    g_matches++;
    logf_("match #%lu: %s (seat 0) vs %s (seat 1)", g_matches, ca->name, cb->name);
}

static void try_match(int idx)
{
    Client *c = &clients[idx];
    int i, best = -1;

    for (i = 0; i < MAX_CLIENTS; i++) {
        Client *o = &clients[i];
        if (i == idx || o->fd < 0 || o->dead || o->state != ST_QUEUED)
            continue;
        if (strcmp(o->room, c->room) != 0)
            continue;
        if (best < 0 || o->qseq < clients[best].qseq)
            best = i;
    }
    if (best >= 0) {
        start_match(best, idx);
    } else {
        c->state = ST_QUEUED;
        c->qseq = ++g_qseq;
        queue_line(c, "WAIT");
        logf_("%s (%s) is waiting for an opponent%s%s", c->name, c->addr,
              c->room[0] ? " in room " : "", c->room);
    }
}

/* =========================================================================
 *  Accounts, stats and friends (commands)
 * ========================================================================= */

static void do_login(int idx, const char *name_in, const char *pw)
{
    Client *c = &clients[idx];
    char name[NAME_LEN + 1], hash[65];
    int ai, is_new = 0, j, diff = 0;
    size_t i;

    if (!g_accounts) {
        queue_line(c, "ERR accounts are not enabled on this server");
        return;
    }
    if (c->state != ST_NEW) {
        queue_line(c, "ERR already introduced");
        return;
    }
    sanitize(name, name_in);
    if (name[0] == '\0' || !pw || strlen(pw) < PASS_MIN || strlen(pw) > PASS_MAX) {
        queue_line(c, "ERR need LOGIN <name> <password> (password %d-%d characters)", PASS_MIN, PASS_MAX);
        return;
    }
    ai = find_acct(name);
    if (ai >= 0) {
        hash_password(accts[ai].salt, pw, hash);
        for (i = 0; i < 64; i++)               /* constant-time compare */
            diff |= hash[i] ^ accts[ai].hash[i];
        if (diff != 0) {
            logf_("bad password for %s from %s", accts[ai].name, c->addr);
            if (++c->fails >= MAX_LOGIN_FAILS)
                c->dead = 1;
            queue_line(c, "ERR wrong password");
            return;
        }
        if (accts[ai].online >= 0) {           /* only reveal this to the real owner */
            queue_line(c, "ERR that account is already logged in");
            return;
        }
    } else {
        Account *a;
        if (nacc >= MAX_ACCOUNTS) {
            queue_line(c, "ERR the server cannot take more accounts");
            return;
        }
        if (name_in_use(name, idx)) {
            queue_line(c, "ERR name in use");
            return;
        }
        ai = nacc++;
        a = &accts[ai];
        memset(a, 0, sizeof *a);
        a->online = -1;
        snprintf(a->name, sizeof a->name, "%s", name);
        random_salt(a->salt);
        hash_password(a->salt, pw, a->hash);
        a->created = time(NULL);
        is_new = 1;
        g_dirty = 1;
        logf_("new account %s", a->name);
    }

    c->acct = ai;
    accts[ai].online = idx;
    snprintf(c->name, sizeof c->name, "%s", accts[ai].name);
    c->state = ST_LOBBY;
    queue_line(c, "WELCOME %d", count_online());
    queue_line(c, "LOGGEDIN %s %d %d %d %d %d %s", accts[ai].name, accts[ai].played,
               accts[ai].won, accts[ai].lost, accts[ai].drawn, accts[ai].forfeits,
               is_new ? "NEW" : "RETURNING");
    for (j = 0; j < accts[ai].nreqs; j++)
        queue_line(c, "FRIENDREQ %s", accts[accts[ai].reqs[j]].name);
    announce_presence(ai, "FRIENDON");
    logf_("%s logged in from %s, %d online", accts[ai].name, c->addr, count_online());
}

static const char *status_of(int ai)
{
    int ci = accts[ai].online;
    if (ci < 0)
        return "offline";
    if (clients[ci].state == ST_PLAYING)
        return "playing";
    if (clients[ci].state == ST_QUEUED)
        return "queued";
    return "online";
}

static void make_friends(int a, int b)
{
    list_del(accts[a].reqs, &accts[a].nreqs, b);
    list_del(accts[b].reqs, &accts[b].nreqs, a);
    if (!list_has(accts[a].friends, accts[a].nfriends, b))
        accts[a].friends[accts[a].nfriends++] = b;
    if (!list_has(accts[b].friends, accts[b].nfriends, a))
        accts[b].friends[accts[b].nfriends++] = a;
    g_dirty = 1;
    notify_acct(a, "FRIENDADDED %s", accts[b].name);
    notify_acct(b, "FRIENDADDED %s", accts[a].name);
}

static void cmd_friend(Client *c, char *sub, char *arg)
{
    int me = c->acct, t;

    if (!sub) {
        queue_line(c, "ERR usage: FRIEND ADD|ACCEPT|DECLINE|REMOVE <name>");
        return;
    }
    t = find_acct(arg);
    if (strcasecmp(sub, "ADD") == 0) {
        if (t < 0) { queue_line(c, "ERR no such player"); return; }
        if (t == me) { queue_line(c, "ERR you cannot friend yourself"); return; }
        if (list_has(accts[me].friends, accts[me].nfriends, t)) {
            queue_line(c, "ERR already friends");
        } else if (list_has(accts[me].reqs, accts[me].nreqs, t)) {
            make_friends(me, t);                      /* they asked first */
            queue_line(c, "OK you are now friends with %s", accts[t].name);
        } else if (list_has(accts[t].reqs, accts[t].nreqs, me)) {
            queue_line(c, "ERR request already sent");
        } else if (accts[me].nfriends >= MAX_FRIENDS) {
            queue_line(c, "ERR your friend list is full");
        } else if (accts[t].nreqs >= MAX_REQS || accts[t].nfriends >= MAX_FRIENDS) {
            queue_line(c, "ERR that player cannot take requests right now");
        } else {
            accts[t].reqs[accts[t].nreqs++] = me;
            g_dirty = 1;
            notify_acct(t, "FRIENDREQ %s", accts[me].name);
            queue_line(c, "OK request sent to %s", accts[t].name);
        }
    } else if (strcasecmp(sub, "ACCEPT") == 0) {
        if (t < 0 || !list_has(accts[me].reqs, accts[me].nreqs, t)) {
            queue_line(c, "ERR no request from that player");
        } else if (accts[me].nfriends >= MAX_FRIENDS || accts[t].nfriends >= MAX_FRIENDS) {
            queue_line(c, "ERR friend list is full");
        } else {
            make_friends(me, t);
            queue_line(c, "OK you are now friends with %s", accts[t].name);
        }
    } else if (strcasecmp(sub, "DECLINE") == 0) {
        if (t < 0 || !list_has(accts[me].reqs, accts[me].nreqs, t)) {
            queue_line(c, "ERR no request from that player");
        } else {
            list_del(accts[me].reqs, &accts[me].nreqs, t);
            g_dirty = 1;
            queue_line(c, "OK request declined");
        }
    } else if (strcasecmp(sub, "REMOVE") == 0) {
        if (t < 0 || !list_has(accts[me].friends, accts[me].nfriends, t)) {
            queue_line(c, "ERR not on your friend list");
        } else {
            list_del(accts[me].friends, &accts[me].nfriends, t);
            list_del(accts[t].friends, &accts[t].nfriends, me);
            g_dirty = 1;
            notify_acct(t, "FRIENDREMOVED %s", accts[me].name);
            queue_line(c, "OK removed %s", accts[t].name);
        }
    } else {
        queue_line(c, "ERR usage: FRIEND ADD|ACCEPT|DECLINE|REMOVE <name>");
    }
}

static void cmd_friends(Client *c)
{
    Account *a = &accts[c->acct];
    int j;

    for (j = 0; j < a->nfriends; j++)
        queue_line(c, "FRIEND %s %s", accts[a->friends[j]].name, status_of(a->friends[j]));
    for (j = 0; j < a->nreqs; j++)
        queue_line(c, "REQUEST %s", accts[a->reqs[j]].name);
    queue_line(c, "ENDFRIENDS");
}

/* target account for STATS / HISTORY: argument or own account */
static int target_acct(Client *c, const char *arg)
{
    int t;
    if (arg)
        t = find_acct(arg);
    else if (c->acct >= 0)
        t = c->acct;
    else {
        queue_line(c, "ERR give a player name (you are not logged in)");
        return -1;
    }
    if (t < 0)
        queue_line(c, "ERR no such player");
    return t;
}

static void cmd_stats(Client *c, const char *arg)
{
    int t = target_acct(c, arg);
    if (t < 0)
        return;
    queue_line(c, "STATS %s %d %d %d %d %d", accts[t].name, accts[t].played, accts[t].won,
               accts[t].lost, accts[t].drawn, accts[t].forfeits);
}

static void cmd_history(Client *c, const char *arg)
{
    int t = target_acct(c, arg), j;
    if (t < 0)
        return;
    for (j = 0; j < accts[t].nhist; j++) {
        GameRec *g = &accts[t].hist[j];
        queue_line(c, "GAME %ld %s %d %d %c", (long)g->t, g->opp, g->km, g->okm, g->res);
    }
    queue_line(c, "ENDHISTORY %s", accts[t].name);
}

static void cmd_leaders(Client *c)
{
    int used[10], n = 0, rank, i, k;

    for (rank = 0; rank < 10; rank++) {
        int best = -1;
        for (i = 0; i < nacc; i++) {
            int skip = 0;
            if (accts[i].played == 0)
                continue;
            for (k = 0; k < n; k++)
                if (used[k] == i)
                    skip = 1;
            if (skip)
                continue;
            if (best < 0 || accts[i].won > accts[best].won ||
                (accts[i].won == accts[best].won && accts[i].played < accts[best].played))
                best = i;
        }
        if (best < 0)
            break;
        used[n++] = best;
        queue_line(c, "LEADER %d %s %d %d", rank + 1, accts[best].name, accts[best].won,
                   accts[best].played);
    }
    queue_line(c, "ENDLEADERS");
}

/* ------------------------------------------------------ command parsing */

static int parse_km(const char *s, int *out)
{
    char *end;
    long v;
    if (!s || !*s)
        return 0;
    v = strtol(s, &end, 10);
    if (*end != '\0' || v < 0 || v > 1000)
        return 0;
    *out = (int)v;
    return 1;
}

static void process_line(int idx, char *line)
{
    Client *c = &clients[idx];
    char *save = NULL;
    char *cmd = strtok_r(line, " \t", &save);

    if (!cmd)
        return;

    if (c->state == ST_NEW && strcmp(cmd, "HELLO") != 0 && strcmp(cmd, "LOGIN") != 0 &&
        strcmp(cmd, "QUIT") != 0) {
        queue_line(c, "ERR say HELLO <name> or LOGIN <name> <password> first");
        return;
    }

    if (strcmp(cmd, "HELLO") == 0) {
        char *name;
        char clean[NAME_LEN + 1];
        if (c->state != ST_NEW) {
            queue_line(c, "ERR already introduced");
            return;
        }
        if (g_accounts && g_require_login) {
            queue_line(c, "ERR this server requires LOGIN <name> <password>");
            return;
        }
        name = strtok_r(NULL, " \t", &save);
        sanitize(clean, name);
        if (clean[0] == '\0')
            snprintf(clean, sizeof clean, "Player%d", idx);
        if (g_accounts && find_acct(clean) >= 0) {
            queue_line(c, "ERR that name is registered - use LOGIN <name> <password>");
            return;
        }
        if (name_in_use(clean, idx)) {
            queue_line(c, "ERR name in use");
            return;
        }
        snprintf(c->name, sizeof c->name, "%s", clean);
        c->state = ST_LOBBY;
        queue_line(c, "WELCOME %d", count_online());
        logf_("hello from %s (%s), %d online", c->name, c->addr, count_online());

    } else if (strcmp(cmd, "LOGIN") == 0) {
        char *name = strtok_r(NULL, " \t", &save);
        char *pw = strtok_r(NULL, " \t", &save);
        do_login(idx, name, pw);

    } else if (strcmp(cmd, "FIND") == 0) {
        char *room;
        if (c->state != ST_LOBBY) {
            queue_line(c, "ERR cannot FIND now");
            return;
        }
        match_detach(c);               /* forget a finished match */
        room = strtok_r(NULL, " \t", &save);
        sanitize(c->room, room);
        try_match(idx);

    } else if (strcmp(cmd, "CANCEL") == 0 || strcmp(cmd, "LEAVE") == 0) {
        leave_current(idx);          /* harmless when idle */

    } else if (strcmp(cmd, "MOVE") == 0) {
        char *kind = strtok_r(NULL, " \t", &save);
        char *arg = strtok_r(NULL, " \t", &save);
        Client *o;

        if (c->state != ST_PLAYING || c->opp < 0) {
            queue_line(c, "ERR not in a match");
            return;
        }
        o = &clients[c->opp];
        if (kind && strcmp(kind, "X") == 0) {
            queue_line(o, "OPP X");
        } else if (kind && (strcmp(kind, "P") == 0 || strcmp(kind, "D") == 0) &&
                   arg && arg[0] >= '0' && arg[0] < '0' + HAND_SLOTS && arg[1] == '\0') {
            queue_line(o, "OPP %s %s", kind, arg);
        } else {
            queue_line(c, "ERR bad MOVE");
        }

    } else if (strcmp(cmd, "RESULT") == 0) {
        int km, okm;
        char *a1 = strtok_r(NULL, " \t", &save);
        char *a2 = strtok_r(NULL, " \t", &save);
        if (c->match < 0) {
            queue_line(c, "ERR no match to report");
        } else if (!parse_km(a1, &km) || !parse_km(a2, &okm)) {
            queue_line(c, "ERR usage: RESULT <my km> <opponent km>");
        } else {
            match_report(c, km, okm);
        }

    } else if (strcmp(cmd, "STATS") == 0 || strcmp(cmd, "HISTORY") == 0 ||
               strcmp(cmd, "LEADERS") == 0) {
        char *arg = strtok_r(NULL, " \t", &save);
        if (!g_accounts) {
            queue_line(c, "ERR accounts are not enabled on this server");
        } else if (cmd[0] == 'S') {
            cmd_stats(c, arg);
        } else if (cmd[0] == 'H') {
            cmd_history(c, arg);
        } else {
            cmd_leaders(c);
        }

    } else if (strcmp(cmd, "FRIEND") == 0 || strcmp(cmd, "FRIENDS") == 0) {
        if (!g_accounts) {
            queue_line(c, "ERR accounts are not enabled on this server");
        } else if (c->acct < 0) {
            queue_line(c, "ERR log in with LOGIN to use friends");
        } else if (cmd[6] == 'S') {
            cmd_friends(c);
        } else {
            char *sub = strtok_r(NULL, " \t", &save);
            char *arg = strtok_r(NULL, " \t", &save);
            cmd_friend(c, sub, arg);
        }

    } else if (strcmp(cmd, "PING") == 0) {
        queue_line(c, "PONG");

    } else if (strcmp(cmd, "QUIT") == 0) {
        c->dead = 1;

    } else {
        queue_line(c, "ERR unknown command");
    }
}

static void read_client(int idx)
{
    Client *c = &clients[idx];
    ssize_t n = recv(c->fd, c->in + c->inlen, (size_t)(IN_SIZE - c->inlen - 1), 0);

    if (n == 0) {
        c->dead = 1;
        return;
    }
    if (n < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
            c->dead = 1;
        return;
    }
    c->inlen += (int)n;
    c->in[c->inlen] = '\0';

    for (;;) {
        char *nl = memchr(c->in, '\n', (size_t)c->inlen);
        int len;
        if (!nl)
            break;
        len = (int)(nl - c->in);
        c->in[len] = '\0';
        if (len > 0 && c->in[len - 1] == '\r')
            c->in[len - 1] = '\0';
        process_line(idx, c->in);
        if (c->dead)
            return;
        memmove(c->in, c->in + len + 1, (size_t)(c->inlen - len - 1));
        c->inlen -= len + 1;
        c->in[c->inlen] = '\0';
    }
    if (c->inlen >= IN_SIZE - 1)       /* line too long: misbehaving client */
        c->dead = 1;
}

static void accept_clients(int lfd)
{
    for (;;) {
        struct sockaddr_in sa;
        socklen_t sl = sizeof sa;
        int fd = accept(lfd, (struct sockaddr *)&sa, &sl);
        int i, one = 1;

        if (fd < 0)
            return;                     /* EAGAIN: nothing more to accept */

        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one);

        for (i = 0; i < MAX_CLIENTS; i++)
            if (clients[i].fd < 0)
                break;
        if (i == MAX_CLIENTS) {
            const char *msg = "ERR server full\n";
            if (send(fd, msg, strlen(msg), MSG_NOSIGNAL) < 0) { /* best effort */ }
            close(fd);
            continue;
        }
        memset(&clients[i], 0, sizeof clients[i]);
        clients[i].fd = fd;
        clients[i].opp = -1;
        clients[i].acct = -1;
        clients[i].match = -1;
        clients[i].state = ST_NEW;
        clients[i].connected = time(NULL);
        {
            unsigned char *ip = (unsigned char *)&sa.sin_addr;
            snprintf(clients[i].addr, sizeof clients[i].addr, "%u.%u.%u.%u:%u",
                     ip[0], ip[1], ip[2], ip[3], ntohs(sa.sin_port));
        }
        logf_("connection from %s", clients[i].addr);
    }
}

/* ----------------------------------------------------------------- main */

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s [port] [-p port] [-d datadir] [--require-login] [--selftest]\n"
            "  -d datadir        enable accounts, game history and friends, stored in datadir\n"
            "  --require-login   with -d: refuse anonymous (HELLO) players\n", prog);
}

int main(int argc, char **argv)
{
    int port = DEFAULT_PORT;
    int lfd, one = 1, i;
    struct sockaddr_in sa;
    struct sigaction act;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--selftest") == 0) {
            return selftest();
        } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            snprintf(g_dir, sizeof g_dir, "%s", argv[++i]);
            g_accounts = 1;
        } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--require-login") == 0) {
            g_require_login = 1;
        } else if (argv[i][0] != '-' && atoi(argv[i]) > 0) {
            port = atoi(argv[i]);
        } else {
            usage(argv[0]);
            return 1;
        }
    }
    if (port < 1 || port > 65535 || (g_require_login && !g_accounts)) {
        usage(argv[0]);
        return 1;
    }

    memset(&act, 0, sizeof act);
    act.sa_handler = on_signal;
    sigaction(SIGINT, &act, NULL);
    sigaction(SIGTERM, &act, NULL);
    signal(SIGPIPE, SIG_IGN);

    seed_random();
    for (i = 0; i < MAX_CLIENTS; i++) {
        clients[i].fd = -1;
        clients[i].opp = -1;
        clients[i].acct = -1;
        clients[i].match = -1;
    }
    for (i = 0; i < MAX_ACCOUNTS; i++)
        accts[i].online = -1;

    if (g_accounts) {
        umask(077);                       /* the database holds password hashes */
        if (mkdir(g_dir, 0700) < 0 && errno != EEXIST) {
            fprintf(stderr, "cannot create %s: %s\n", g_dir, strerror(errno));
            return 1;
        }
        db_load();
    }

    lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) {
        perror("socket");
        return 1;
    }
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    fcntl(lfd, F_SETFL, fcntl(lfd, F_GETFL, 0) | O_NONBLOCK);

    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port = htons((unsigned short)port);
    if (bind(lfd, (struct sockaddr *)&sa, sizeof sa) < 0) {
        perror("bind");
        return 1;
    }
    if (listen(lfd, 64) < 0) {
        perror("listen");
        return 1;
    }
    logf_("HollyBornes server listening on port %d%s", port,
          g_accounts ? " (accounts enabled)" : " (anonymous play only)");

    while (!g_stop) {
        struct pollfd pfds[MAX_CLIENTS + 1];
        int map[MAX_CLIENTS + 1];
        int np = 0, r, k;
        time_t now;

        pfds[np].fd = lfd;
        pfds[np].events = POLLIN;
        pfds[np].revents = 0;
        map[np++] = -1;
        for (i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].fd < 0)
                continue;
            pfds[np].fd = clients[i].fd;
            pfds[np].events = (short)(POLLIN | (clients[i].outlen > 0 ? POLLOUT : 0));
            pfds[np].revents = 0;
            map[np++] = i;
        }

        r = poll(pfds, (nfds_t)np, 1000);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            perror("poll");
            break;
        }

        for (k = 0; k < np; k++) {
            if (!pfds[k].revents)
                continue;
            if (map[k] < 0) {
                accept_clients(lfd);
                continue;
            }
            {
                Client *c = &clients[map[k]];
                if (c->fd < 0)
                    continue;
                if (pfds[k].revents & (POLLERR | POLLNVAL))
                    c->dead = 1;
                if (!c->dead && (pfds[k].revents & (POLLIN | POLLHUP)))
                    read_client(map[k]);
                if (!c->dead && (pfds[k].revents & POLLOUT))
                    flush_client(c);
            }
        }

        /* push out whatever was queued this round, drop silent newcomers, */
        /* then drop dead clients (which may queue OPPLEFT for opponents).  */
        now = time(NULL);
        for (i = 0; i < MAX_CLIENTS; i++) {
            Client *c = &clients[i];
            if (c->fd < 0)
                continue;
            if (c->state == ST_NEW && now - c->connected > HELLO_TIMEOUT)
                c->dead = 1;
            if (!c->dead)
                flush_client(c);
        }
        for (i = 0; i < MAX_CLIENTS; i++)
            if (clients[i].fd >= 0 && clients[i].dead)
                drop_client(i);
        for (i = 0; i < MAX_CLIENTS; i++)       /* deliver the OPPLEFTs now */
            if (clients[i].fd >= 0 && clients[i].outlen > 0)
                flush_client(&clients[i]);

        match_sweep();
        if (g_dirty)
            db_save();
    }

    logf_("shutting down");
    if (g_dirty)
        db_save();
    for (i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].fd >= 0)
            close(clients[i].fd);
    close(lfd);
    return 0;
}
