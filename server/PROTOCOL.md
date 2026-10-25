# HollyBornes network protocol

Plain TCP, UTF-8/ASCII text, one message per line (`\n`, a preceding `\r` is ignored). Maximum line length is 1023 bytes. Fields are separated by single spaces.

## How a match works

The server does **not** know the rules of the game. It does three things:

1. **Matchmaking.** It pairs two waiting players, either with the same room code or, if neither gave one, whoever has waited longest.
2. **Shuffling.** It creates the shuffled 106-card deck and sends the same order to both players, so neither client can rig the draw.
3. **Relaying.** It forwards each player's moves to the opponent, unchanged apart from the `MOVE` / `OPP` prefix.

Both clients run the same rules engine on the same deck and apply the same moves in the same order, so they always agree on the game state ("lockstep").

## Client to server

| Message | Meaning |
|---------|---------|
| `HELLO <name>` | Introduce yourself. Must be the first message. The name is cut to 16 characters and anything other than letters, digits, `_`, `-` and `.` becomes `_`. |
| `FIND [room]` | Enter the matchmaking queue. With a room code you are only matched with someone using the same code. Only allowed while idle. |
| `CANCEL` | Leave the queue. |
| `LEAVE` | Leave the current match (the opponent gets `OPPLEFT`), or leave the queue. Harmless when idle. |
| `MOVE P <slot>` | Play the card in hand slot `slot` (0-6). |
| `MOVE D <slot>` | Discard the card in hand slot `slot` (0-6). |
| `MOVE X` | Pass (no cards left because the draw pile is empty). |
| `PING` | The server answers `PONG`. |
| `QUIT` | Disconnect. |

## Server to client

| Message | Meaning |
|---------|---------|
| `WELCOME <n>` | Reply to `HELLO`; `n` players are online. |
| `WAIT` | You are queued, waiting for an opponent. |
| `MATCH <seat> <opponent> <deck>` | Match found. `seat` is `0` (moves first) or `1`. `deck` is 106 comma-separated card numbers. |
| `OPP P <slot>` / `OPP D <slot>` / `OPP X` | The opponent's move. |
| `OPPLEFT` | The opponent left or disconnected. The match is over and you are back in the lobby. |
| `PONG` | Reply to `PING`. |
| `ERR <text>` | The last message was rejected. |

## Player states

`NEW` (connected) -> `HELLO` -> `LOBBY` -> `FIND` -> `QUEUED` -> match found -> `PLAYING` -> `LEAVE` / game over -> `LOBBY`.

Connections that never send `HELLO` are dropped after 30 seconds.

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
- When a game ends, each client sends `LEAVE` to return to the lobby, so a new `FIND` can be made on the same connection.
