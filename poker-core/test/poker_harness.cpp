// Offline harness: drives the exact mental-poker protocol from poker_plugin.cpp
// across N in-process players, then exercises the betting engine + evaluator.
// poker_crypto and poker_game are Qt-free, so this links without Basecamp.
#include "poker_crypto.h"
#include "poker_game.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <vector>

using namespace poker;

static int g_fail = 0;
static void check(bool ok, const std::string& what)
{
    if (!ok) { ++g_fail; printf("  FAIL  %s\n", what.c_str()); }
    else     {           printf("  ok    %s\n", what.c_str()); }
}

// ── The protocol, exactly as poker_plugin.cpp sequences it ──────────────────
static bool runDeal(int N, bool verbose)
{
    printf("\n-- mental-poker deal, N=%d --\n", N);
    std::vector<std::unique_ptr<SraKeyset>> keys;
    for (int i = 0; i < N; ++i) keys.push_back(std::make_unique<SraKeyset>());

    // Phase 1: shuffle. Each seat encrypts the whole deck with its shuffle key
    // and reorders with its own entropy.
    std::vector<std::string> deck = SraKeyset::cardCodes();
    for (int s = 0; s < N; ++s) {
        std::vector<std::string> enc;
        for (const std::string& v : deck) enc.push_back(keys[s]->encryptShuffle(v));
        std::mt19937 g(1234 + s * 77);          // fixed seed: reproducible harness
        std::shuffle(enc.begin(), enc.end(), g);
        deck = enc;
    }

    // Phase 2: lock. Each seat strips its shuffle key and re-encrypts every
    // position under a distinct per-position key. No reshuffle.
    for (int s = 0; s < N; ++s) {
        std::vector<std::string> out;
        for (int j = 0; j < (int)deck.size(); ++j) {
            std::string v = keys[s]->decryptShuffle(deck[j]);
            v = keys[s]->encryptCard(j, v);
            out.push_back(v);
        }
        deck = out;
    }

    // Decode a position by applying every seat's per-position key.
    auto decodeWith = [&](int pos, const std::set<int>& seats) -> int {
        std::string v = deck[pos];
        for (int s : seats) {
            v = SraKeyset::applyKey(v, keys[s]->cardDecryptKeyHex(pos));
            if (v.empty()) return -1;
        }
        return SraKeyset::cardIdForCode(v);
    };
    std::set<int> all;
    for (int s = 0; s < N; ++s) all.insert(s);

    // Every one of the 52 positions must decode to a distinct valid card —
    // i.e. the encrypt/shuffle/lock pipeline is a permutation of the deck.
    std::vector<int> decoded;
    std::set<int> seen;
    bool allValid = true;
    for (int p = 0; p < kDeckSize; ++p) {
        const int c = decodeWith(p, all);
        if (c < 0 || c >= kDeckSize || seen.count(c)) { allValid = false; break; }
        seen.insert(c);
        decoded.push_back(c);
    }
    check(allValid && (int)seen.size() == kDeckSize,
          "all 52 positions decode to a distinct card (deck is a permutation)");

    // The deal must be secret: with any seat's own key withheld, that seat's
    // hole cards must NOT decode. This is the property the whole design rests on.
    bool secrecyHolds = true;
    for (int k = 0; k < N && secrecyHolds; ++k) {
        std::set<int> others = all;
        others.erase(k);                        // seat k never publishes its own key
        for (int h = 0; h < 2; ++h) {
            const int pos = 2 * k + h;
            if (decodeWith(pos, others) >= 0) { secrecyHolds = false; break; }
        }
    }
    check(secrecyHolds, "hole cards do NOT decode without their owner's key");

    // And the owner (its own key + everyone else's) must succeed.
    bool ownerReads = true;
    for (int k = 0; k < N; ++k) {
        const int c0 = decodeWith(2 * k,     all);
        const int c1 = decodeWith(2 * k + 1, all);
        if (c0 < 0 || c1 < 0) { ownerReads = false; break; }
        if (verbose)
            printf("     seat %d holes: %d,%d\n", k, c0, c1);
    }
    check(ownerReads, "each seat reads its own holes with the full key set");

    // Board: the five positions after the holes, revealed by all seats.
    bool boardOk = true;
    for (int i = 0; i < 5; ++i)
        if (decodeWith(2 * N + i, all) < 0) { boardOk = false; break; }
    check(boardOk, "all five board positions decode");

    // Shuffle must actually reorder — a deck still in code order would mean
    // position p is always card p and the shuffle contributed nothing.
    bool reordered = false;
    for (int p = 0; p < kDeckSize; ++p) if (decoded[p] != p) { reordered = true; break; }
    check(reordered, "deck order differs from the plaintext code order");
    return true;
}

// ── Evaluator ───────────────────────────────────────────────────────────────
static int C(int rank, int suit) { return suit * 13 + rank; }   // rank 0='2' .. 12='A'

static void testEvaluator()
{
    printf("\n-- hand evaluator --\n");
    auto cat = [](std::vector<int> s) { return handCategoryName(s); };

    // Royal flush (T J Q K A of spades = ranks 8..12, suit 3).
    { int h[7] = { C(8,3),C(9,3),C(10,3),C(11,3),C(12,3), C(0,0),C(1,1) };
      check(cat(evaluate7(h)) == "Straight Flush", "royal flush -> Straight Flush"); }
    // Quads.
    { int h[7] = { C(5,0),C(5,1),C(5,2),C(5,3),C(9,0), C(2,1),C(3,2) };
      check(cat(evaluate7(h)) == "Four of a Kind", "quads -> Four of a Kind"); }
    // Full house.
    { int h[7] = { C(5,0),C(5,1),C(5,2),C(9,0),C(9,1), C(2,1),C(3,2) };
      check(cat(evaluate7(h)) == "Full House", "trips+pair -> Full House"); }
    // Flush.
    { int h[7] = { C(1,2),C(4,2),C(6,2),C(9,2),C(11,2), C(0,0),C(3,1) };
      check(cat(evaluate7(h)) == "Flush", "five hearts -> Flush"); }
    // Wheel straight A-2-3-4-5.
    { int h[7] = { C(12,0),C(0,1),C(1,2),C(2,3),C(3,0), C(7,1),C(9,2) };
      check(cat(evaluate7(h)) == "Straight", "A-2-3-4-5 -> Straight (wheel)"); }
    // Two pair.
    { int h[7] = { C(5,0),C(5,1),C(9,2),C(9,3),C(2,0), C(3,1),C(7,2) };
      check(cat(evaluate7(h)) == "Two Pair", "two pair -> Two Pair"); }
    // High card.
    { int h[7] = { C(12,0),C(10,1),C(8,2),C(5,3),C(3,0), C(1,1),C(0,2) };
      check(cat(evaluate7(h)) == "High Card", "no made hand -> High Card"); }

    // Ordering: a straight flush must outrank quads, quads outrank a boat, etc.
    int sf[7] = { C(8,3),C(9,3),C(10,3),C(11,3),C(12,3), C(0,0),C(1,1) };
    int q[7]  = { C(5,0),C(5,1),C(5,2),C(5,3),C(9,0), C(2,1),C(3,2) };
    int fh[7] = { C(5,0),C(5,1),C(5,2),C(9,0),C(9,1), C(2,1),C(3,2) };
    check(evaluate7(sf) > evaluate7(q), "straight flush beats quads");
    check(evaluate7(q)  > evaluate7(fh), "quads beat a full house");

    // Kicker: same pair, better kicker must win.
    int p1[7] = { C(5,0),C(5,1),C(12,2),C(7,3),C(3,0), C(1,1),C(0,2) };  // pair 7s, A kicker
    int p2[7] = { C(5,0),C(5,1),C(11,2),C(7,3),C(3,0), C(1,1),C(0,2) };  // pair 7s, K kicker
    check(evaluate7(p1) > evaluate7(p2), "same pair, ace kicker beats king kicker");
}

// ── Betting engine ──────────────────────────────────────────────────────────
static void testBetting()
{
    printf("\n-- betting engine (heads-up) --\n");
    PokerTable t;
    t.upsertSeat("aaa", "Alice");
    t.upsertSeat("bbb", "Bob");
    check(t.startHand(0), "startHand accepted with 2 funded seats");
    check(t.pot() == kSmallBlind + kBigBlind, "pot holds both blinds after the deal");

    // Heads-up: the button posts the small blind and acts first pre-flop.
    const int btn = t.button();
    check(t.toAct() == btn, "heads-up: button acts first pre-flop");

    const std::string btnId = t.seats()[btn].id;
    const std::string othId = t.seats()[1 - btn].id;

    check(!t.applyAction(othId, "check", 0), "out-of-turn action is rejected");
    check(!t.applyAction(btnId, "check", 0), "check facing the big blind is rejected");
    check(t.applyAction(btnId, "call", 0), "button calls the big blind");
    check(t.applyAction(othId, "check", 0), "big blind checks -> pre-flop closed");
    check(t.roundComplete(), "betting round reports complete");

    t.setBoard({ C(2,0), C(7,1), C(11,2) });
    t.advanceStreet();
    check(t.phase() == Phase::Flop, "street advanced to the flop");
    check(t.pot() == 2 * kBigBlind, "pot carries over into the flop");
    check(t.currentBet() == 0, "current bet resets on a new street");

    printf("\n-- betting engine (3-handed, raise + fold) --\n");
    PokerTable u;
    u.upsertSeat("p1", "One");
    u.upsertSeat("p2", "Two");
    u.upsertSeat("p3", "Three");
    check(u.startHand(0), "3-handed hand starts");
    const int a = u.toAct();
    const std::string aId = u.seats()[a].id;
    check(!u.applyAction(aId, "raise", 1), "sub-minimum raise is rejected");
    check(u.applyAction(aId, "raise", kBigBlind), "min-raise accepted");
    check(u.currentBet() == 2 * kBigBlind, "current bet is blind + raise");
    // The other two fold; the raiser takes it down.
    int f1 = u.toAct(); check(u.applyAction(u.seats()[f1].id, "fold", 0), "second seat folds");
    int f2 = u.toAct(); check(u.applyAction(u.seats()[f2].id, "fold", 0), "third seat folds");
    check(u.liveCount() == 1, "one live seat remains");

    const long potBefore = u.pot();
    std::vector<int> winners = { a };
    const long chipsBefore = u.seats()[a].chips;
    u.endHand(winners);
    check(u.seats()[a].chips == chipsBefore + potBefore, "fold winner is paid the whole pot");
    check(u.pot() == 0, "pot is emptied after the hand");

    // Chip conservation across the table.
    long total = 0;
    for (const Seat& s : u.seats()) total += s.chips;
    check(total == 3 * kStartingChips, "chips are conserved (no minting or burning)");

    printf("\n-- split pot --\n");
    PokerTable v;
    v.upsertSeat("x1", "X");
    v.upsertSeat("x2", "Y");
    v.startHand(0);
    const long before = v.seats()[0].chips + v.seats()[1].chips;
    const long pot = v.pot();
    v.endHand({ 0, 1 });
    check(v.seats()[0].chips + v.seats()[1].chips == before + pot,
          "split pot returns every chip to the two winners");
}

int main()
{
    printf("=== p2p-poker offline harness ===\n");
    runDeal(2, true);
    runDeal(3, false);
    runDeal(6, false);
    testEvaluator();
    testBetting();
    printf("\n=== %s (%d failure%s) ===\n", g_fail ? "FAILURES" : "ALL PASS",
           g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
