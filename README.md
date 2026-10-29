# HollyBornes

A Hollywood take on the classic French road-race card game **Mille Bornes**, written in [Hollywood](https://www.hollywood-mal.com) with the [RapaGUI](https://www.hollywood-mal.com/rapagui.html) plugin. Be the first to travel exactly 1000 km, against the computer or against another person online. With an optional account server your games are recorded and you can make friends.

## Requirements

- [Hollywood](https://www.hollywood-mal.com) 9.0 or later
- The [RapaGUI](https://www.hollywood-mal.com/rapagui.html) plugin (`rapagui.hwp`), installed so Hollywood can find it. The script loads it with `@REQUIRE "rapagui.hwp"` (see Troubleshooting).
- For online play: a running matchmaking server (see below). Building it needs a C compiler, POSIX sockets (Linux, macOS, BSD, or WSL on Windows) and the SQLite development files: `sudo apt install libsqlite3-dev` on Debian/Ubuntu, `sudo dnf install sqlite-devel` on Fedora; macOS already includes SQLite.

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

Server options:

| Option | Meaning |
|--------|---------|
| `-d datadir` | Turn on **accounts**: logins, recorded games, friends and a leaderboard, stored in an SQLite database (`hollybornes.db`) in `datadir` |
| `--require-login` | With `-d`: only registered players may connect (no guests) |
| `-p port` | Same as the positional port |
| `--selftest` | Check the built-in password hashing and exit |

```
./hollyserver 5555 -d ./hollydata                  # accounts, guests still allowed
./hollyserver 5555 -d ./hollydata --require-login  # accounts only
```

### 2. Connect from the game

In the **Play online** panel of the game window:

| Field | Meaning |
|-------|---------|
| Server | Host name or IP address of the machine running `hollyserver`. Defaults to `millebornes.gameplayer.club`; use `localhost` to test against a server on your own computer. |
| Port | The server port (default 5555) |
| Name | The name your opponent sees (your account name if you use a password) |
| Password | Optional. Leave it empty to play as a **guest**. With a password you log in to your account, or create it if the name is new. Needs a server started with `-d`. |
| Room code | Optional. Two players who enter the same code are matched with each other, which is handy for playing with a friend. Leave it empty to be matched with the next random player. |

The **Status** line at the top of the Online panel shows where you stand:

| Status | Meaning |
|--------|---------|
| `not connected` | No connection to the server |
| `connecting...` | Connected, waiting for the server to accept your name |
| `connected as guest <name> (not logged in)` | Playing as a guest: no recorded games and no friends |
| `LOGGED IN as <name>  (N games, M won)` | Logged in to your account; the numbers are your totals when you logged in |
| `not logged in - ERR ...` | The server refused the login, for example a wrong password |

Press **Find opponent** (or **Log in** to connect without searching). The log shows "Searching for an opponent..." until someone else joins, then the match starts automatically. The line under the player panels shows whose turn it is.

- Seat order is random. The player in seat 0 moves first.
- **New game** (or the menu item) leaves the match or cancels the search and starts a game against the computer.
- **Disconnect** also closes the connection to the server.
- If your opponent leaves or loses their connection, the game ends and you can search again on the same connection.

To test on one computer, start the server, then run two copies of the game with the Server field set to `localhost` and the same (or an empty) room code.

## Accounts, stats and friends

These need a server started with `-d` and a password in the Online panel. Guests can still play, but nothing is recorded for them.

**Accounts.** The first time you log in with a new name, the account is created with the password you typed. The game log then shows "Account created!" so you know it worked (a returning login shows "Logged in as ..." instead). Names are not case-sensitive. After that, the same name and password log you back in from any computer. A registered name cannot be used by guests.

**Recorded games.** When an online game ends, both players' games report the final distances and the server records the result: games played, won, lost, drawn and forfeits, plus your last 10 games.

- Leaving or disconnecting in the middle of a game counts as a **forfeit** (a loss for you, a win for your opponent).
- If the two reports disagree, the game counts as played but has no winner.
- Games against the computer are not recorded.

**Stats, History and Leaderboard** buttons show the totals and last 10 games of yourself, or of any player whose name you type in the box next to **Add friend**, and the top 10 players by wins.

**Friends.** Type a player's name next to **Add friend** and press the button to send a request. The other player sees it in their Friends list and presses **Accept** (or **Remove / Decline**). If you both ask each other you become friends at once. The list shows whether each friend is online, queued or playing, and you get a message when a friend comes online. To play a friend, both of you enter the same **Room code** and press Find opponent.

**Safety notes.** The connection is not encrypted, so your password travels in clear text. Use a password you don't use anywhere else, or tunnel the connection (for example with `ssh -L 5555:localhost:5555 yourserver`). The server stores only a salted hash. Results are reported by the players' game clients, so two people working together could fake results; this is meant for friendly play.

## How to play

Each turn you draw a card, then play or discard one card:

1. Choose a card by pressing its number button (**1** to **7**) under your hand. The chosen card is marked with `>` and shown in bold.
2. Press **Play card** or **Discard card**.
3. The other player (or the computer) takes their turn.

Your hand is colour-coded: **hazards are red**, **remedies are green**, and distance cards and safeties are gray. Cards you can play right now are marked `<- playable`. The game log shows what happened, newest first.

Every card that is played, by you, the computer or your online opponent, makes a short card-snap sound. The sound is generated by the game when it starts, so there are no sound files to install. Use **Game > Sound on / off** to mute it. Discarded cards are silent. If your system has no audio output, the game says so in the log when you try to toggle the sound and carries on without it.

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
- **Game > Find online opponent** connects (if needed), logs you in, and joins the queue.
- **Game > Rules...** shows a summary of the rules.
- **Game > Sound on / off** switches the card sound on or off.
- **Game > About...** opens a window with the game icon and version information. Press OK, the Escape key or the close button to dismiss it.
- **Game > Quit** exits the program. The keyboard shortcut is the system's standard quit key: **Ctrl+Q** on Windows and Linux, **Command+Q** on macOS. If you quit during an online game, your opponent is told you left, and it counts as a forfeit.

## How the online mode works

The server is a matchmaker and relay (and, in account mode, a record keeper). When two players are paired it shuffles the deck once and sends the same card order to both. After that, each player's moves (play slot 3, discard slot 5, ...) are forwarded to the opponent. Both copies of the game run the same rules on the same deck and stay in step, and each client checks that its opponent's moves are legal. The full message format is in [`server/PROTOCOL.md`](server/PROTOCOL.md).

Because both clients hold the full deck order, a modified client could peek at the opponent's cards. This is fine for games among friends, but it is not cheat-proof.

## Files

| File | Description |
|------|-------------|
| `HollyBornes.hws` | The complete game: rules engine, computer player, online client and RapaGUI interface |
| `icons/` | The application icon as PNG files in ten sizes (16 to 1024 pixels). Keep this folder next to `HollyBornes.hws`. |
| `server/hollyserver.c` | The matchmaking, relay and account server (single-threaded C, `poll()` based) |
| `server/Makefile` | `make` builds the server (linking SQLite), `make test` runs all tests |
| `server/test_server.py` | Protocol tests for matchmaking, rooms, relaying, leaving, disconnects and abuse |
| `server/test_accounts.py` | Tests for logins, recorded games, forfeits, friends, leaderboard and persistence across restarts |
| `server/PROTOCOL.md` | The text protocol spoken between game and server, and the storage format |

When accounts are on, everything is stored in one SQLite file, `hollybornes.db`, in the data directory.

### The database

| Table | Contents |
|-------|----------|
| `players` | One row per account: name, salted password hash, creation time and totals (played, won, lost, drawn, forfeits) |
| `friends` | Friendships, one row per direction |
| `friend_requests` | Open friend requests |
| `games` | Every finished game: time, both players (or guest names), distances and results (`W` won, `L` lost, `D` draw, `F` forfeit, `X` disputed) |

Every change is written to the database immediately, so nothing is lost if the server crashes or the machine loses power. Player totals and friends are also kept in memory while the server runs. The `HISTORY` command reads the games table, so the full history of every game is kept even though the game only shows the last 10.

**Backups.** Stop the server and copy `hollybornes.db`, or take a live backup with the `sqlite3` command-line tool:

```
sqlite3 hollydata/hollybornes.db ".backup backup.db"
```

**Looking at the data.** The file can be opened with any SQLite tool. For example, the ten players with the most wins:

```
sqlite3 hollydata/hollybornes.db "SELECT name, won, played FROM players ORDER BY won DESC LIMIT 10;"
```

Stop the server before changing anything by hand, because it keeps player totals and friends in memory and would not notice the change (and could write the old values back).

**Upgrading from the text-file version.** Earlier versions kept the data in `accounts.db` and `games.log`. If the data directory contains those files and no database yet, the server imports them automatically the first time it starts: accounts, passwords, totals, friendships, open requests and the full game archive. The old files are renamed to `accounts.db.migrated` and `games.log.migrated` and kept as a backup. If the import fails, nothing is changed and the server refuses to start.

## Application icon

The icon is also shown in the **About** window (the 128-pixel version, preloaded with `@BRUSH 1`). The game declares its icon with `@APPICON` at the top of `HollyBornes.hws`, using the PNG files in the `icons` folder (a red race car on a playing card, on a green tile). It shows in the window border and task bar on Windows, macOS and Linux. When you compile the game to an executable, the icon is embedded on Windows and macOS; Linux does not support embedding icons in executables, so there it only appears on the window.

The `icons` folder must sit next to `HollyBornes.hws`: if Hollywood cannot find the files it stops at start-up with a file-not-found error. To use your own icon, replace the PNG files (they should have transparency and the same names and sizes), or delete the `@APPICON` lines if you do not want a custom icon.

## Troubleshooting

**"Loading plugin RapaGUI...failed"**

Hollywood could not load the RapaGUI plugin. Run the script from a terminal or command prompt to see the reason, then check:

- The plugin is installed: `rapagui.hwp` in Hollywood's `Plugins` folder, or next to the script or executable.
- It matches your Hollywood: same platform and the same 32-bit or 64-bit build.
- The `@REQUIRE` line at the top of `HollyBornes.hws` uses the file name, `@REQUIRE "rapagui.hwp"`. On Linux, where file names are case-sensitive, the plain name `"RapaGUI"` can fail to load the plugin.
- On Linux, the GTK libraries are installed. The `gtk3` build of RapaGUI is the most compatible; the "webview" builds need extra components.
- On Hollywood 11, if the plugin is installed in the global plugins folder, add `@OPTIONS {GlobalPlugins = True}` above the `@REQUIRE` line.
- On macOS, a freshly downloaded plugin may be blocked by the quarantine flag.

**The hand shows no colours, or not all seven cards are visible**

The colours use RapaGUI's styled text, which is only available in its Textview widget, so only the hand is colour-coded; the game log and the status lines stay plain. The hand view has a fixed height of 190 pixels, enough for seven lines at normal font sizes. If you use a large system font and the last card is cut off, raise the `height="190"` value on the `hand` textview in `HollyBornes.hws`. If your system shows the escape codes as text instead of colours, tell me what you see. The three colours are set in one place, the `p_CardColor$` function, if you want to change them.

**Cannot connect to the server**

Check that `hollyserver` is running, that the Server and Port fields match, and that the port is open in the firewall if you are connecting across networks.

**"Wrong password" or "that name is registered"**

Account names are not case-sensitive, and a registered name cannot be used by guests. If you forgot the password, the account can only be reset by deleting it while the server is stopped, so the person can register the name again: `sqlite3 hollydata/hollybornes.db "DELETE FROM players WHERE name = 'thename';"`. That also removes their friendships and requests; their past games stay in the history under the name they used.

**Online game ends with "Out of sync"**

Both players must run the same version of the game. If the message appears with matching versions, please report it, along with the log text.

## Notes

The GUI is built with RapaGUI's XML layout system (`moai.CreateApp`) and handles events through event handlers for RapaGUI and network events.

## License

Copyright (C) 2026 Marisa Giancarla.

HollyBornes, including the matchmaking server, is free software released under the **GNU General Public License**, version 3 or (at your option) any later version. You may use, study, share and modify it, and share your modified versions, provided they are released under the same license. The software comes with **no warranty**.

The full text of the license is at <https://www.gnu.org/licenses/gpl-3.0.html>.

The RapaGUI plugin and Hollywood itself are separate products with their own licenses and are not covered by this one.

For more information or support, visit <https://millebornes.gameplayer.club>.
