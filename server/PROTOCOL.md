# HollyBornes network protocol

Plain TCP, UTF-8/ASCII text, one message per line (`\n`, a preceding `\r` is ignored). Maximum line length is 1023 bytes. Fields are separated by single spaces.

## How a match works

The server does **not** know the rules of the game. It does three things:

1. **Matchmaking.** It pairs two waiting players, either with the same room code or, if neither gave one, whoever has waited longest.
2. **Shuffling.** It creates the shuffled 106-card deck and sends the same order to both players, so neither client can rig the draw.
3. **Relaying.** It forwards each player's moves to the opponent, unchanged apart from the `MOVE` / `OPP` prefix.

Both clients run the same rules engine on the same deck and apply the same moves in the same order, so they always agree on the game state ("lockstep").

## Starting the server

```
./hollyserver [port] [-d datadir] [--require-login]
./hollyserver --selftest
```

| Option | Meaning |
|--------|---------|
| `port` or `-p port` | TCP port, default 5555 |
| `-d datadir` | Turn on **account mode**: logins, recorded games, friends. Data is stored in an SQLite database in `datadir` (created if missing). Without it the server is anonymous-only and every account command answers `ERR accounts are not enabled`. |
| `--require-login` | With `-d`: refuse anonymous (`HELLO`) players |
| `--selftest` | Check the built-in SHA-256 and exit |

## Client to server

| Message | Meaning |
|---------|---------|
| `HELLO <name>` | Join as a **guest**. Must be the first message (or use `LOGIN`). The name is cut to 16 characters and anything other than letters, digits, `_`, `-` and `.` becomes `_`. Names must be unique among connected players. In account mode a guest cannot use a registered name. |
| `LOGIN <name> <password>` | Account mode only. Log in, or **create the account** if the name is new. Password: 4 to 32 characters, no spaces. Names are case-insensitive. |
| `FIND [room]` | Enter the matchmaking queue. With a room code you are only matched with someone using the same code. Only allowed while idle. |
| `CANCEL` | Leave the queue. |
| `LEAVE` | Leave the current match (the opponent gets `OPPLEFT`), or leave the queue. Harmless when idle. |
| `MOVE P <slot>` | Play the card in hand slot `slot` (0-6). |
| `MOVE D <slot>` | Discard the card in hand slot `slot` (0-6). |
| `MOVE X` | Pass (no cards left because the draw pile is empty). |
| `RESULT <my km> <opponent km>` | Report the final distances when a game ends (0-1000 each). See "How results are recorded". |
| `STATS [name]` | Account mode. Totals for a player (yourself if no name). |
| `HISTORY [name]` | Account mode. The last 10 games of a player (yourself if no name). |
| `LEADERS` | Account mode. Top 10 players by wins. |
| `FRIENDS` | Logged-in players. List friends (with status) and incoming requests. |
| `FRIEND ADD <name>` | Send a friend request. If that player already asked you, you become friends at once. |
| `FRIEND ACCEPT <name>` | Accept a request. |
| `FRIEND DECLINE <name>` | Decline a request. |
| `FRIEND REMOVE <name>` | Remove a friend (they are told). |
| `PING` | The server answers `PONG`. |
| `QUIT` | Disconnect. |

## Server to client

| Message | Meaning |
|---------|---------|
| `WELCOME <n>` | Reply to a successful `HELLO` or `LOGIN`; `n` players are online. |
| `LOGGEDIN <name> <played> <won> <lost> <drawn> <forfeits> <NEW\|RETURNING>` | Sent after `LOGIN`. The name is the account's real spelling. |
| `WAIT` | You are queued, waiting for an opponent. |
| `MATCH <seat> <opponent> <deck>` | Match found. `seat` is `0` (moves first) or `1`. `deck` is 106 comma-separated card numbers. |
| `OPP P <slot>` / `OPP D <slot>` / `OPP X` | The opponent's move. |
| `OPPLEFT` | The opponent left or disconnected. The match is over and you are back in the lobby. |
| `STATS <name> <played> <won> <lost> <drawn> <forfeits>` | Reply to `STATS`. |
| `GAME <unix time> <opponent> <my km> <opponent km> <result>` ... `ENDHISTORY <name>` | Reply to `HISTORY`, newest first. Result is `W` won, `L` lost, `D` draw, `F` forfeited, `X` disputed. |
| `LEADER <rank> <name> <won> <played>` ... `ENDLEADERS` | Reply to `LEADERS`. |
| `FRIEND <name> <online\|offline\|queued\|playing>` ... `REQUEST <name>` ... `ENDFRIENDS` | Reply to `FRIENDS`. |
| `FRIENDREQ <name>` | Someone sent you a friend request (also sent at login for requests still open). |
| `FRIENDADDED <name>` | You and `name` are now friends. |
| `FRIENDREMOVED <name>` | `name` removed you. |
| `FRIENDON <name>` / `FRIENDOFF <name>` | A friend logged in or out. |
| `OK <text>` | A friend command worked. |
| `PONG` | Reply to `PING`. |
| `ERR <text>` | The last message was rejected. |

Notifications such as `OPPLEFT`, `FRIENDREQ` and `FRIENDON` can arrive at any time, including between a request and its multi-line reply, so clients should handle each line by its first word.

## Player states

`NEW` (connected) -> `HELLO` or `LOGIN` -> `LOBBY` -> `FIND` -> `QUEUED` -> match found -> `PLAYING` -> `LEAVE` / game over -> `LOBBY`.

Connections that never send `HELLO` or `LOGIN` are dropped after 30 seconds. After 5 wrong passwords on one connection the server disconnects it.

## How results are recorded

The server cannot see the game, so it relies on the two clients:

1. When a game ends, **each** client sends `RESULT <my km> <opponent km>` and then `LEAVE`.
2. If both reports agree (A's own distance equals B's view of A, and vice versa), the game is recorded: the player with more km wins, equal distances are a draw.
3. If the reports **disagree**, the game counts as played for both players with no winner (`X`).
4. If a player leaves or disconnects **without** reporting while the game is still running, they **forfeit**: a loss and a forfeit for them, a win for the other player.
5. If only one player reports and the other leaves or does not report within 120 seconds, the single report is used.

Only players logged in with an account have their games recorded. Games against the computer are never reported.

Because results are reported by the clients, two players working together could fake a result. This is meant for friendly play, not ranked tournaments.

## Account storage

Account mode keeps everything in one SQLite database, `hollybornes.db`, inside the data directory (created with mode 0700, the database with mode 0600). Every change is committed immediately.

| Table | Columns |
|-------|---------|
| `players` | `id`, `name` (unique, case-insensitive), `salt`, `hash`, `created`, `played`, `won`, `lost`, `drawn`, `forfeits` |
| `friends` | `player_id`, `friend_id`, `since` (one row per direction) |
| `friend_requests` | `from_id`, `to_id`, `created` |
| `games` | `id`, `played_at`, `p0_id`, `p1_id` (NULL for a guest), `name0`, `name1`, `km0`, `km1`, `res0`, `res1`. Player 0 is seat 0. |

`HISTORY` returns the newest 10 rows of `games` for a player, flipped to that player's point of view. For a disputed game each side's own reported distance is stored.

Passwords are never stored. The server keeps a random 16-byte salt and a salted, iterated (5000 rounds) SHA-256 hash per account.

The schema version is kept in `PRAGMA user_version` (currently 1). A server refuses to open a database from a newer version.

**Import of the old text format.** Earlier versions stored `accounts.db` and `games.log` as text files. When the data directory has such a file and the SQLite database has no players yet, the server imports it once at start-up (in a single transaction) and renames the old files to `*.migrated`.

The connection itself is **not encrypted**, so passwords travel in clear text. Use a throw-away password, or put the server behind a TLS or SSH tunnel (for example `ssh -L 5555:localhost:5555 yourserver`).

## Card numbers

| Number | Card | Copies |
|--------|------|--------|
| 0-4 | 25, 50, 75, 100, 200 km | 10, 10, 10, 12, 4 |
| 5-9 | Accident, Out of Gas, Flat Tire, Speed Limit, Stop | 3, 3, 3, 4, 5 |
| 10-14 | Repairs, Gasoline, Spare Tire, End of Limit, GO | 6, 6, 6, 6, 14 |
| 15-18 | Driving Ace, Extra Tank, Puncture-Proof, Right of Way | 1 each |

## Turn flow (what the clients do)

- The deck is a stack: cards are drawn from the **end** of the list.
- Dealing: six cards each, alternating, seat 0 first. Seat 0 then draws a seventh card and moves first.
- A player draws at the start of their turn. The opponent mirrors that draw when the `OPP` message arrives, so both clients take cards from the deck in the same order.
- Playing a safety card gives an extra turn: the player draws again and sends another `MOVE`.
- When the draw pile is empty, a player with no cards sends `MOVE X`.
- When a game ends, each client sends `RESULT` and then `LEAVE` to return to the lobby, so a new `FIND` can be made on the same connection.
