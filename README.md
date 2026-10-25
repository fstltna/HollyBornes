# HollyBornes

A Hollywood take on the classic French road-race card game **Mille Bornes**, written in [Hollywood](https://www.hollywood-mal.com) with the [RapaGUI](https://www.hollywood-mal.com/rapagui.html) plugin. Be the first to travel exactly 1000 km, against the computer or against another person online.

## Requirements

- [Hollywood](https://www.hollywood-mal.com) 9.0 or later
- The [RapaGUI](https://www.hollywood-mal.com/rapagui.html) plugin, installed so Hollywood can find it
- For online play: a running matchmaking server (see below). Building it needs a C compiler and POSIX sockets (Linux, macOS, BSD, or WSL on Windows).

## Running the game

```
Hollywood HollyBornes.hws
```

You can also compile the script to a standalone executable from the Hollywood IDE or command line.

## Playing online (2 players)

### 1. Start the matchmaking server

```
cd server
make
./hollyserver 5555
```

The port is optional and defaults to 5555. Run it on a machine both players can reach, and open the port in the firewall if the players are on different networks.

### 2. Connect from the game

In the **Play online** panel of the game window:

| Field | Meaning |
|-------|---------|
| Server | Host name or IP address of the machine running `hollyserver` (`localhost` for testing) |
| Port | The server port (default 5555) |
| Name | The name your opponent sees |
| Room code | Optional. Two players who enter the same code are matched with each other, which is handy for playing with a friend. Leave it empty to be matched with the next random player. |

Press **Find opponent**. The log shows "Searching for an opponent..." until someone else joins, then the match starts automatically. The line under the player panels shows whose turn it is.

- Seat order is random. The player in seat 0 moves first.
- **New game** (or the menu item) leaves the match or cancels the search and starts a game against the computer.
- **Disconnect** also closes the connection to the server.
- If your opponent leaves or loses their connection, the game ends and you can search again on the same connection.

To test on one computer, start the server, then run two copies of the game with `localhost` and the same (or an empty) room code.

## How to play

Each turn you draw a card, then play or discard one card:

1. Select a card in the **Your hand** list.
2. Press **Play card** or **Discard card**.
3. The other player (or the computer) takes their turn.

Cards you can play right now are marked `<- playable`. The game log shows what happened, newest first.

### The goal

Be the first to travel exactly **1000 km**. If the draw pile runs out, whoever has gone farthest wins.

### The cards

The deck has 106 cards:

| Type | Cards | Purpose |
|------|-------|---------|
| Distance | 25, 50, 75, 100, 200 km | Move you forward, but only while rolling |
| Hazards | Accident, Out of Gas, Flat Tire, Stop, Speed Limit | Played on your opponent |
| Remedies | Repairs, Gasoline, Spare Tire, GO, End of Limit | Fix hazards on yourself |
| Safeties | Driving Ace, Extra Tank, Puncture-Proof, Right of Way | Permanent protection |

### The rules

- You need a **GO** card to start, and again after every hazard is repaired.
- **Hazards** (Accident, Out of Gas, Flat Tire, Stop) can only be played on an opponent who is rolling. **Speed Limit** can be played at any time.
- **Remedies** cure the matching hazard:
  - Repairs fixes Accident.
  - Gasoline fixes Out of Gas.
  - Spare Tire fixes Flat Tire.
  - GO fixes Stop.
  - End of Limit removes a Speed Limit.
- Under a **Speed Limit** you may only play 25 or 50 km cards.
- Only two **200 km** cards may be played per game.
- You cannot go past 1000 km, so the final card must bring you to exactly 1000.
- **Safeties** protect you permanently against their hazard and give you an extra turn when played:
  - Driving Ace protects against Accident.
  - Extra Tank protects against Out of Gas.
  - Puncture-Proof protects against Flat Tire.
  - Right of Way protects against Stop and Speed Limit, and means you never need a GO card.

### Simplifications

This version leaves out two rules of the original game:

- There is no *coup fourre* (instantly counter-playing a safety when the matching hazard is played on you).
- There are no end-of-hand scoring bonuses. The winner is decided by distance only.

## Menu

- **Game > New game vs computer** starts a game against the computer.
- **Game > Find online opponent** connects (if needed) and joins the queue.
- **Game > Rules...** shows a summary of the rules.
- **Game > About...** shows version information.
- **Game > Quit** exits the program.

## How the online mode works

The server is only a matchmaker and relay. When two players are paired it shuffles the deck once and sends the same card order to both. After that, each player's moves (play slot 3, discard slot 5, ...) are forwarded to the opponent. Both copies of the game run the same rules on the same deck and stay in step, and each client checks that its opponent's moves are legal. The full message format is in [`server/PROTOCOL.md`](server/PROTOCOL.md).

Because both clients hold the full deck order, a modified client could peek at the opponent's cards. This is fine for games among friends, but it is not cheat-proof.

## Files

| File | Description |
|------|-------------|
| `HollyBornes.hws` | The complete game: rules engine, computer player, online client and RapaGUI interface |
| `server/hollyserver.c` | The matchmaking and relay server (single-threaded C, `poll()` based) |
| `server/Makefile` | `make` builds the server, `make test` runs the protocol tests |
| `server/test_server.py` | Protocol tests: matchmaking, rooms, relaying, leaving, disconnects and abuse |
| `server/PROTOCOL.md` | The text protocol spoken between game and server |

## Notes

The GUI is built with RapaGUI's XML layout system (`moai.CreateApp`) and handles events through event handlers for RapaGUI and network events. If you run into problems, check that the RapaGUI plugin is installed correctly and that you are using a recent Hollywood version.

## License

Add your preferred license here.
