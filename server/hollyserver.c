/*
 * hollyserver.c  -  matchmaking + relay server for HollyBornes
 *
 * What it does
 *   - accepts TCP clients speaking a tiny line-based text protocol
 *     (see PROTOCOL.md)
 *   - pairs two players who ask for a match (same room code, or both
 *     without one) and shuffles the 106-card deck for them, so neither
 *     client can rig the draw order
 *   - relays each player's moves to the other player
 *
 * The server does NOT know the rules of the game. Both clients run the same
 * rules engine on the same deck and apply the same moves in the same order.
 *
 * Build:  cc -O2 -Wall -Wextra -o hollyserver hollyserver.c
 * Run:    ./hollyserver [port]          (default port 5555)
 *
 * Portable POSIX C (Linux, macOS, BSD). Single thread, poll() based.
 * On Windows use WSL, or port the socket calls to Winsock.
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0 /* SIGPIPE is ignored explicitly below */
#endif

#define DEFAULT_PORT   5555
#define MAX_CLIENTS    256
#define IN_SIZE        1024
#define OUT_SIZE       8192
#define NAME_LEN       16
#define HELLO_TIMEOUT  30 /* seconds a new connection may stay silent */
#define DECK_SIZE      106
#define HAND_SLOTS     7

enum state {
    ST_FREE = 0,
    ST_NEW,     /* connected, no HELLO yet            */
    ST_LOBBY,   /* introduced itself, idle            */
    ST_QUEUED,  /* waiting for an opponent            */
    ST_PLAYING  /* in a match                         */
};

typedef struct {
    int    fd;
    int    state;
    int    opp;                /* index of opponent while PLAYING, else -1 */
    int    dead;               /* drop at the end of this loop iteration   */
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

static Client clients[MAX_CLIENTS];
static volatile sig_atomic_t g_stop = 0;
static unsigned long g_qseq = 0;
static unsigned long g_matches = 0;

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

static void queue_line(Client *c, const char *fmt, ...)
{
    char tmp[1024];
    va_list ap;
    int n;

    if (c->fd < 0 || c->dead)
        return;
    va_start(ap, fmt);
    n = vsnprintf(tmp, sizeof tmp - 1, fmt, ap);
    va_end(ap);
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

/* ---------------------------------------------------------- bookkeeping */

static int count_online(void)
{
    int i, n = 0;
    for (i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].fd >= 0 && clients[i].state >= ST_LOBBY)
            n++;
    return n;
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
    flush_client(c);
    close(c->fd);
    memset(c, 0, sizeof *c);
    c->fd = -1;
    c->opp = -1;
    logf_("disconnect %s (%s), %d online", name, addr, count_online());
}

static void start_match(int a, int b)
{
    char deck[512];
    Client *ca, *cb;

    if (random() & 1) { int t = a; a = b; b = t; }  /* random seat order */
    ca = &clients[a];
    cb = &clients[b];

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

/* -------------------------------------------------------- command logic */

static void process_line(int idx, char *line)
{
    Client *c = &clients[idx];
    char *save = NULL;
    char *cmd = strtok_r(line, " \t", &save);

    if (!cmd)
        return;

    if (c->state == ST_NEW && strcmp(cmd, "HELLO") != 0 && strcmp(cmd, "QUIT") != 0) {
        queue_line(c, "ERR say HELLO <name> first");
        return;
    }

    if (strcmp(cmd, "HELLO") == 0) {
        char *name;
        if (c->state != ST_NEW) {
            queue_line(c, "ERR already introduced");
            return;
        }
        name = strtok_r(NULL, " \t", &save);
        sanitize(c->name, name);
        if (c->name[0] == '\0')
            snprintf(c->name, sizeof c->name, "Player%d", idx);
        c->state = ST_LOBBY;
        queue_line(c, "WELCOME %d", count_online());
        logf_("hello from %s (%s), %d online", c->name, c->addr, count_online());

    } else if (strcmp(cmd, "FIND") == 0) {
        char *room;
        if (c->state != ST_LOBBY) {
            queue_line(c, "ERR cannot FIND now");
            return;
        }
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

int main(int argc, char **argv)
{
    int port = DEFAULT_PORT;
    int lfd, one = 1, i;
    struct sockaddr_in sa;
    struct sigaction act;

    if (argc > 1) {
        port = atoi(argv[1]);
        if (port < 1 || port > 65535) {
            fprintf(stderr, "usage: %s [port]\n", argv[0]);
            return 1;
        }
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
    logf_("HollyBornes server listening on port %d", port);

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
    }

    logf_("shutting down");
    for (i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].fd >= 0)
            close(clients[i].fd);
    close(lfd);
    return 0;
}
