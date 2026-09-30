#ifndef POKER_GAME_H
#define POKER_GAME_H

#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────
// Deterministic Texas Hold'em engine (simplified): betting state machine +
// 5-of-7 hand evaluator. No Qt, no networking, no crypto — every peer feeds it
// the identical sequence of actions / revealed cards and gets identical state,
// which is what makes a trustless table possible.
//
// Card id 0..51: rank = id % 13 (0='2' … 12='A'), suit = id / 13.
//
// Simplifications (see README): single main pot, no side pots; a player who
// cannot cover a bet goes all-in for their stack and stays eligible for the
// whole pot. Blinds are fixed; the button rotates each hand.
// ─────────────────────────────────────────────────────────────────────────

namespace poker {

constexpr long kStartingChips = 1000;
constexpr long kSmallBlind    = 5;
constexpr long kBigBlind       = 10;

enum class Phase { Idle, Preflop, Flop, Turn, River, Showdown, HandOver };

struct Seat {
    std::string id;            // peer senderId
    std::string name;
    long chips     = 0;
    long committed = 0;        // chips committed this betting round
    bool inHand    = false;    // dealt into the current hand
    bool folded    = false;
    bool allIn     = false;
    bool acted     = false;    // has acted since the last raise this round
    int  hole0     = -1;       // revealed hole cards (-1 until known)
    int  hole1     = -1;
};

class PokerTable {
public:
    // ── Membership ──
    void upsertSeat(const std::string& id, const std::string& name);
    void setChips(const std::string& id, long chips); // authoritative sync at hand start
    int  seatIndex(const std::string& id) const;     // -1 if absent
    int  seatCount() const { return static_cast<int>(m_seats.size()); }
    const std::vector<Seat>& seats() const { return m_seats; }

    // ── Hand lifecycle (deterministic) ──
    bool  startHand(int button);                      // false if < 2 funded players
    Phase phase()      const { return m_phase; }
    int   button()     const { return m_button; }
    long  pot()        const;
    long  currentBet() const { return m_currentBet; }
    long  minRaise()   const { return m_minRaise; }
    int   toAct()      const { return m_toAct; }      // seat index, -1 if round done
    long  toCall(int seatIdx) const;
    int   liveCount()  const;                         // non-folded seats in the hand
    bool  roundComplete() const { return m_toAct < 0; }

    // Apply a player action. Rejects out-of-turn / illegal actions (returns false).
    // kind ∈ {fold, check, call, raise}; amount = raise increment above current bet.
    bool applyAction(const std::string& id, const std::string& kind, long amount);

    // ── Cards (filled in as the crypto reveals them) ──
    void setBoard(const std::vector<int>& cards);     // up to 5 community cards
    const std::vector<int>& board() const { return m_board; }
    void setHole(const std::string& id, int c0, int c1);

    // ── Street / resolution ──
    void advanceStreet();                             // Preflop→Flop→…→Showdown
    std::vector<int> showdownWinners() const;         // needs 5 board + revealed holes
    void endHand(const std::vector<int>& winners);    // collect pot, pay winners, HandOver

    int nextButton() const;                           // rotate to next funded seat

private:
    int  nextActive(int from) const;                  // next inHand && !folded && !allIn
    void postBlind(int seatIdx, long amount);
    void resetActedExcept(int seatIdx);
    void recomputeToAct(int fromIdx);
    void collectBets();

    std::vector<Seat> m_seats;
    std::vector<int>  m_board;
    Phase m_phase      = Phase::Idle;
    int   m_button     = 0;
    long  m_pot        = 0;
    long  m_currentBet = 0;
    long  m_minRaise   = kBigBlind;
    int   m_toAct      = -1;
};

// ── Hand evaluation ──
int rankOf(int card);          // 0..12
int suitOf(int card);          // 0..3
// Comparable score for the best 5-card hand out of 7; bigger is better.
std::vector<int> evaluate7(const int cards[7]);
// Human-readable category name for a score produced by evaluate7 (e.g. "Flush").
std::string handCategoryName(const std::vector<int>& score);

} // namespace poker

#endif // POKER_GAME_H
