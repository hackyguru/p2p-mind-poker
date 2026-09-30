# p2p-mind-poker

Trustless multiplayer **Texas Hold'em** played peer-to-peer over
`logos-delivery-module` (Logos's Waku-based pub/sub core module). It started
as Part 9 of the [logos-workshop](https://github.com/hackyguru/logos-workshop) series.
No server, no trusted dealer: the deck is shuffled and dealt with **mental
poker** cryptography so that every player's hole cards stay secret and nobody
controls the shuffle.

> Both modules build, install and load in Basecamp 0.2.3; two peers find each
> other, see each other at the table and start a hand over logos-delivery. A
> complete hand played through to showdown across two GUI instances has **not**
> been observed yet — see *Status & limitations* below.

```
p2p-mind-poker/
├── poker-core/                 # C++ Qt plugin "poker" (delivery + crypto + game engine)
│   ├── src/
│   │   ├── poker_interface.h   # Q_INVOKABLE API surface
│   │   ├── poker_crypto.{h,cpp}# SRA commutative cipher (OpenSSL BIGNUM)
│   │   ├── poker_game.{h,cpp}  # betting state machine + 5-of-7 hand evaluator
│   │   └── poker_plugin.{h,cpp}# delivery wiring + mental-poker protocol orchestration
│   └── test/run.sh             # offline harness — crypto + betting + evaluator
├── poker-ui/                   # QML table view (depends on poker)
│   └── Main.qml
└── install.sh                  # build output -> local Basecamp (quits it first)
```

## Why mental poker?

`delivery_module` is **broadcast** pub/sub — every peer receives every byte
published on a content topic. That's fine for a shared colour everyone
should see but fatal for poker, where your two hole cards must stay private and
no single party may rig the shuffle.

The classic solution is **mental poker** (Shamir–Rivest–Adleman, 1979), built on
a *commutative* cipher: `c = mᵉ mod p`, `m = cᵈ mod p` with `d = e⁻¹ mod (p-1)`.
Because `(mᵃ)ᵇ ≡ (mᵇ)ᵃ`, several players can each encrypt the deck in turn and
later peel their layers off in any order.

`p` is RFC 3526 MODP Group 14, a 2048-bit **safe** prime, and the 52 cards are
encoded as quadratic residues so a ciphertext's Legendre symbol leaks nothing.
This is the workshop's **first module to link an external library** (OpenSSL's
`libcrypto`) — wired through `metadata.json` (`nix.packages.runtime: ["openssl"]`)
plus `FIND_PACKAGES`/`LINK_LIBRARIES` in `CMakeLists.txt`.

## The protocol

All messages are JSON on `/p2p-poker/1/table/json`, discriminated by a `type`
field and deduplicated by a per-message id. Players act in a deterministic order
(sorted by peer id); the lowest id is "coordinator" and only sequences hand
start — it has **no** cryptographic privilege.

1. **Join** — peers announce `{type:"join"}` and take a seat (1000 play-money chips).
2. **Start** — the coordinator broadcasts the participant list, chip counts and
   dealer button; everyone adopts the same hand and generates fresh keys.
3. **Shuffle** — each player in turn encrypts all 52 cards with their whole-deck
   key and shuffles. After everyone, the deck is encrypted by all and in an order
   nobody knows.
4. **Lock** — each player removes their whole-deck key and re-encrypts every
   *position* with a distinct per-card key (no reshuffle). Now each fixed position
   is locked under every player's per-position key.
5. **Deal** — position `2k,2k+1` are seat *k*'s hole cards; the next five are the
   board. To let seat *k* read its hole cards, every *other* player publishes their
   per-position decryption key for those positions (`type:"key"`). Seat *k* applies
   them plus its own key — only it can, because its own key is never published.
   **This is the encrypted broadcast**: the partial keys are public, but only the
   intended owner can finish the decryption.
6. **Betting** — `preflop → flop → turn → river`, with `fold/check/call/raise`.
   Every peer runs the identical deterministic betting engine, so they all agree
   on the pot, the turn order and the result. The flop/turn/river are revealed by
   *all* players publishing their keys for those board positions when the street
   opens.
7. **Showdown** — surviving players publish their own hole keys; everyone now has
   every key, decrypts all live hands, runs the 5-of-7 evaluator and awards the
   pot. Deterministic ⇒ all peers compute the same winner.

## Build

```bash
# Core (first build is long — delivery_module Nim closure + OpenSSL)
cd poker-core
nix flake update
nix build '.#lgx-portable' --out-link result-portable

# UI — its `poker` input points at this repo's poker-core on GitHub;
# override it to build against your local checkout instead
cd ../poker-ui
nix build --override-input poker path:../poker-core '.#lgx-portable' --out-link result-portable
```

Install both `result-portable/*.lgx` from Basecamp's **Modules → Install LGX
Package** (core first, then UI), or run `./install.sh`, which unpacks them
straight into the user module/plugin dirs.

`install.sh` does three things that are easy to miss by hand:

- **Quits Basecamp first** — a module dylib can't be replaced while it's mapped.
- **Re-signs each core dylib** (`rm`, then `cp`, then `codesign --force --sign -`).
  Apple Silicon validates every executable page at map time; overwriting in
  place reuses the inode and the kernel's cached hash goes stale, so Basecamp
  gets `SIGKILL (Code Signature Invalid)` on the next launch.
- **Copies the package's `assets/` directory**, not just the variant payload.
  The UI manifest points `icon` at `assets/icon.png`, which sits at the package
  root; without it the sidebar falls back to a two-letter text tile.

### Offline check

The cryptography and the betting engine are Qt-free, so they can be exercised
without Basecamp at all:

```bash
./poker-core/test/run.sh
```

It runs the real shuffle → lock → deal pipeline for 2, 3 and 6 players and
asserts the properties that matter: the deck stays a permutation, a seat's hole
cards **cannot** be decrypted without that seat's own key, the owner can read
them with the full key set, chips are conserved, and the 5-of-7 evaluator ranks
known hands correctly.

## Run two peers on one machine

Poker needs ≥ 2 players. Only the P2P ports collide, so the core
reads `POKER_TCPPORT` and uses deterministic Instance-A/B node keys + static
nodes so the two instances dial each other directly over loopback.

`open` won't pass an environment variable through to the app, so launch the
bundle's own wrapper script instead — it sets the Qt paths and `exec`s the
binary, and it keeps each peer's log on your terminal:

```bash
BC=/Applications/LogosBasecamp.app/Contents/MacOS/LogosBasecamp

# Instance A — default ports (60000 TCP / 9000 UDP)
"$BC" > /tmp/peerA.log 2>&1 &

# Instance B — overridden ports (60001 TCP / 9001 UDP). Give A a few
# seconds first so B has something to dial.
POKER_TCPPORT=60001 "$BC" > /tmp/peerB.log 2>&1 &
```

Confirm they actually found each other before dealing — each side should log a
relay message to the other's peer id:

```bash
grep -c "relay message" /tmp/peerA.log /tmp/peerB.log
```

In **each** instance: **Start net → Join table** (type a name). Then in the
*coordinator* (the one whose id sorts first — it shows the **Deal hand** button)
press **Deal hand**. Cards appear; bet from the action bar when it's your turn.

> Start networking and Join on every peer **before** dealing the first hand, so
> all joins have propagated. Between runs, clear stragglers:
> `pkill -9 -f LogosBasecamp.bin; pkill -9 -f logos_host`.

## Status & limitations

**Verified on Basecamp 0.2.3 (macOS, darwin-arm64):** both modules build from a
clean `nix build`, install, and load (`Module loaded: poker`); two peers start
their delivery nodes, dial each other over loopback and relay table messages
both ways on `/p2p-poker/1/table/json`; a peer can join and the table shows it
as host. The offline harness passes for 2/3/6 players. **Not yet observed:** a
full hand dealt through to showdown across two GUI instances.

Seven bugs in the original draft were fixed while getting there — worth knowing,
because most of them affect any module written against this pattern:

- **Never redeclare `logosAPI` in your plugin.** `PluginInterface` already has a
  public `LogosAPI* logosAPI`, and the host reads *that* member to decide
  whether the module is wired up. A same-named field in the derived class
  shadows it, so `initLogos()` fills the shadow while the base stays null and
  **every** call in is refused with `QtProviderObject::callMethod: LogosAPI not
  available`. The module looks loaded and does nothing.
- **`logos.callModule` JSON-encodes the return value.** A method returning a
  plain string needs one `JSON.parse`. A method whose
  return value is *itself* a JSON document — like `tableState()` — arrives
  double-encoded, and one parse yields the inner JSON *text*, not an object. The
  UI silently kept rendering defaults. `unwrapRemote()` now parses until the
  result stops being a string.
- **`callModule` is the synchronous, no-argument entry point.** Anything with
  arguments goes through `logos.callModuleAsync(id, method, args, cb)`, which is
  what `joinTable` and `act` now use.
- **The Instance-B PeerID was stale.** `staticNodes` needs the libp2p identity
  that `delivery_module` actually derives from `nodeKey`; the old constant made
  A's dial to B fail the noise handshake, so the pair only worked because B
  dialed A.
- **delivery_module ≥ 0.2.0 delivers payloads as raw bytes, not base64.** The
  handler base64-decoded them anyway, got garbage, and silently dropped every
  message — each player only ever saw their own seat. It now takes raw bytes
  and falls back to base64 for older builds.
- **Relay doesn't preserve order.** The coordinator's 27 KB first shuffle pass
  overtook its small `start` message, was discarded as belonging to an unknown
  hand, and the table hung on "Shuffling deck". Messages for a not-yet-adopted
  hand are now parked and replayed once its `start` arrives.
- **Joins are re-announced.** A `join` published before the other peer's node is
  up is simply lost, so each seat re-broadcasts every 5 s and greets newcomers.

- **First external-lib module.** OpenSSL is wired via `find_package(OpenSSL)` +
  `OpenSSL::Crypto`. If the Nix build can't locate OpenSSL, that CMake wiring is
  the place to adjust (e.g. `OPENSSL_ROOT_DIR`).
- **No zero-knowledge shuffle proof.** SRA gives card *secrecy* and prevents any
  single party from fixing the deal, but a malicious peer could still inject a
  bad shuffle. A production build would add a verifiable-shuffle proof; here the
  honest-but-curious model is assumed (peers follow the protocol; everyone *can*
  reconstruct the full 52-card deck once a hand ends to sanity-check it).
- **Simplified betting.** Single main pot, no side pots; a short stack goes all-in
  for its chips and stays eligible for the whole pot. Fixed blinds (SB 5 / BB 10),
  rotating button.
- **Chips are coordinator-synced at each hand start** to avoid drift if a peer
  missed messages. Join everyone before hand 1 for clean accounting.
- **Crypto cost.** Per hand each player does a few hundred 2048-bit modular
  exponentiations (shuffle + lock + reveals). One-off per hand; fine for a demo.

## Licence

MIT and Apache-2.0 — pick whichever works for you.
