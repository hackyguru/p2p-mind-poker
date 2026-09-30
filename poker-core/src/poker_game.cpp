#include "poker_game.h"

#include <algorithm>

namespace poker {

// ── Membership ────────────────────────────────────────────────────────────

void PokerTable::upsertSeat(const std::string& id, const std::string& name)
{
    int idx = seatIndex(id);
    if (idx >= 0) {
        m_seats[idx].name = name;
        return;
    }
    Seat s;
    s.id    = id;
    s.name  = name;
    s.chips = kStartingChips;
    m_seats.push_back(s);
    // Keep seats ordered by id so seat geometry is identical on every peer.
    std::sort(m_seats.begin(), m_seats.end(),
              [](const Seat& a, const Seat& b) { return a.id < b.id; });
}

void PokerTable::setChips(const std::string& id, long chips)
{
    const int idx = seatIndex(id);
    if (idx >= 0) m_seats[idx].chips = chips;
}

int PokerTable::seatIndex(const std::string& id) const
{
    for (int i = 0; i < seatCount(); ++i)
        if (m_seats[i].id == id) return i;
    return -1;
}

// ── Turn helpers ────────────────────────────────────────────────────────────

int PokerTable::nextActive(int from) const
{
    const int n = seatCount();
    for (int k = 1; k <= n; ++k) {
        const int i = (from + k) % n;
        const Seat& s = m_seats[i];
        if (s.inHand && !s.folded && !s.allIn) return i;
    }
    return -1;
}

int PokerTable::liveCount() const
{
    int c = 0;
    for (const Seat& s : m_seats)
        if (s.inHand && !s.folded) ++c;
    return c;
}

long PokerTable::pot() const
{
    long p = m_pot;
    for (const Seat& s : m_seats) p += s.committed;
    return p;
}

long PokerTable::toCall(int seatIdx) const
{
    if (seatIdx < 0 || seatIdx >= seatCount()) return 0;
    return std::max(0L, m_currentBet - m_seats[seatIdx].committed);
}

// ── Hand lifecycle ────────────────────────────────────────────────────────

void PokerTable::postBlind(int seatIdx, long amount)
{
    Seat& s = m_seats[seatIdx];
    const long pay = std::min(amount, s.chips);
    s.committed += pay;
    s.chips     -= pay;
    if (s.chips == 0) s.allIn = true;
}

bool PokerTable::startHand(int button)
{
    int funded = 0;
    for (const Seat& s : m_seats)
        if (s.chips > 0) ++funded;
    if (funded < 2) return false;

    for (Seat& s : m_seats) {
        s.committed = 0;
        s.folded    = false;
        s.allIn     = false;
        s.acted     = false;
        s.hole0     = -1;
        s.hole1     = -1;
        s.inHand    = (s.chips > 0);
    }
    m_board.clear();
    m_pot        = 0;
    m_minRaise   = kBigBlind;

    const int n = seatCount();
    m_button = ((button % n) + n) % n;
    if (!m_seats[m_button].inHand) m_button = nextActive(m_button);

    // Heads-up: button posts the small blind. 3+ handed: blind is left of button.
    const int sb = (funded == 2) ? m_button : nextActive(m_button);
    const int bb = nextActive(sb);
    postBlind(sb, kSmallBlind);
    postBlind(bb, kBigBlind);

    m_currentBet = std::max(m_seats[sb].committed, m_seats[bb].committed);
    m_phase      = Phase::Preflop;
    m_toAct      = nextActive(bb);   // UTG (heads-up: the button/SB)
    return true;
}

void PokerTable::resetActedExcept(int seatIdx)
{
    for (int i = 0; i < seatCount(); ++i) {
        Seat& s = m_seats[i];
        if (i != seatIdx && s.inHand && !s.folded && !s.allIn) s.acted = false;
    }
}

void PokerTable::recomputeToAct(int fromIdx)
{
    if (liveCount() <= 1) { m_toAct = -1; return; }
    const int n = seatCount();
    for (int k = 1; k <= n; ++k) {
        const int i = (fromIdx + k) % n;
        Seat& s = m_seats[i];
        if (s.inHand && !s.folded && !s.allIn &&
            (!s.acted || s.committed != m_currentBet)) {
            m_toAct = i;
            return;
        }
    }
    m_toAct = -1;
}

bool PokerTable::applyAction(const std::string& id, const std::string& kind, long amount)
{
    const int idx = seatIndex(id);
    if (idx < 0 || idx != m_toAct) return false;
    Seat& s = m_seats[idx];
    if (!s.inHand || s.folded || s.allIn) return false;

    if (kind == "fold") {
        s.folded = true;
        s.acted  = true;
    } else if (kind == "check") {
        if (s.committed != m_currentBet) return false;   // can't check facing a bet
        s.acted = true;
    } else if (kind == "call") {
        const long need = m_currentBet - s.committed;
        const long pay  = std::min(need, s.chips);
        s.committed += pay;
        s.chips     -= pay;
        if (s.chips == 0) s.allIn = true;
        s.acted = true;
    } else if (kind == "raise") {
        if (amount <= 0) return false;
        const long target = m_currentBet + amount;
        const long need   = target - s.committed;
        if (need >= s.chips) {
            // Can't cover the full raise — shove all-in for the stack.
            const long pay = s.chips;
            s.committed += pay;
            s.chips      = 0;
            s.allIn      = true;
            if (s.committed > m_currentBet) {
                const long inc = s.committed - m_currentBet;
                if (inc > m_minRaise) m_minRaise = inc;
                m_currentBet = s.committed;
                resetActedExcept(idx);
            }
        } else {
            if (amount < m_minRaise) return false;        // raise too small
            s.committed  = target;
            s.chips     -= need;
            m_minRaise   = amount;
            m_currentBet = target;
            resetActedExcept(idx);
        }
        s.acted = true;
    } else {
        return false;
    }

    recomputeToAct(idx);
    return true;
}

// ── Cards ───────────────────────────────────────────────────────────────────

void PokerTable::setBoard(const std::vector<int>& cards)
{
    m_board = cards;
    if (m_board.size() > 5) m_board.resize(5);
}

void PokerTable::setHole(const std::string& id, int c0, int c1)
{
    const int idx = seatIndex(id);
    if (idx < 0) return;
    m_seats[idx].hole0 = c0;
    m_seats[idx].hole1 = c1;
}

// ── Street / resolution ──────────────────────────────────────────────────────

void PokerTable::collectBets()
{
    for (Seat& s : m_seats) {
        m_pot += s.committed;
        s.committed = 0;
    }
}

void PokerTable::advanceStreet()
{
    collectBets();
    m_currentBet = 0;
    m_minRaise   = kBigBlind;
    for (Seat& s : m_seats)
        if (s.inHand && !s.folded && !s.allIn) s.acted = false;

    switch (m_phase) {
        case Phase::Preflop: m_phase = Phase::Flop;     break;
        case Phase::Flop:    m_phase = Phase::Turn;     break;
        case Phase::Turn:    m_phase = Phase::River;    break;
        case Phase::River:   m_phase = Phase::Showdown; break;
        default:                                         break;
    }

    if (m_phase == Phase::Showdown) {
        m_toAct = -1;
    } else {
        // Postflop, first to act is the first active seat left of the button.
        m_toAct = nextActive(m_button);
    }
}

std::vector<int> PokerTable::showdownWinners() const
{
    std::vector<int> winners;
    if (m_board.size() < 5) return winners;

    std::vector<int> best;
    for (int i = 0; i < seatCount(); ++i) {
        const Seat& s = m_seats[i];
        if (!s.inHand || s.folded) continue;
        if (s.hole0 < 0 || s.hole1 < 0) continue;        // hole not revealed
        const int cards[7] = {
            s.hole0, s.hole1,
            m_board[0], m_board[1], m_board[2], m_board[3], m_board[4]
        };
        const std::vector<int> score = evaluate7(cards);
        if (winners.empty() || score > best) {
            best = score;
            winners.clear();
            winners.push_back(i);
        } else if (score == best) {
            winners.push_back(i);
        }
    }
    return winners;
}

void PokerTable::endHand(const std::vector<int>& winners)
{
    collectBets();
    if (!winners.empty()) {
        const long share = m_pot / static_cast<long>(winners.size());
        const long rem   = m_pot % static_cast<long>(winners.size());
        for (size_t k = 0; k < winners.size(); ++k) {
            m_seats[winners[k]].chips += share + (k == 0 ? rem : 0);
        }
    }
    m_pot   = 0;
    m_toAct = -1;
    m_phase = Phase::HandOver;
}

int PokerTable::nextButton() const
{
    const int n = seatCount();
    if (n == 0) return 0;
    for (int k = 1; k <= n; ++k) {
        const int i = (m_button + k) % n;
        if (m_seats[i].chips > 0) return i;
    }
    return m_button;
}

// ── Hand evaluation ──────────────────────────────────────────────────────────

int rankOf(int card) { return card % 13; }
int suitOf(int card) { return card / 13; }

namespace {

// Score for an exact 5-card hand. Lexicographically comparable; bigger wins.
// Layout: [category, tiebreakers…]. Category 8=straight flush … 0=high card.
std::vector<int> evaluate5(const int c[5])
{
    int cnt[13] = {0};
    int suits[5];
    for (int i = 0; i < 5; ++i) {
        cnt[c[i] % 13]++;
        suits[i] = c[i] / 13;
    }
    bool flush = true;
    for (int i = 1; i < 5; ++i)
        if (suits[i] != suits[0]) { flush = false; break; }

    int straightHigh = -1;
    for (int hi = 12; hi >= 4; --hi) {
        bool ok = true;
        for (int k = 0; k < 5; ++k)
            if (!cnt[hi - k]) { ok = false; break; }
        if (ok) { straightHigh = hi; break; }
    }
    if (straightHigh < 0 && cnt[12] && cnt[3] && cnt[2] && cnt[1] && cnt[0])
        straightHigh = 3;   // wheel A-2-3-4-5, the five plays as high card

    // Rank groups, ordered by (count desc, rank desc).
    std::vector<std::pair<int,int>> groups;   // {count, rank}
    for (int r = 12; r >= 0; --r)
        if (cnt[r]) groups.emplace_back(cnt[r], r);
    std::stable_sort(groups.begin(), groups.end(),
        [](const std::pair<int,int>& a, const std::pair<int,int>& b) {
            return a.first > b.first;
        });

    if (flush && straightHigh >= 0) return {8, straightHigh};
    if (groups[0].first == 4)       return {7, groups[0].second, groups[1].second};
    if (groups[0].first == 3 && groups.size() > 1 && groups[1].first >= 2)
                                    return {6, groups[0].second, groups[1].second};
    if (flush) {
        std::vector<int> s = {5};
        for (const auto& g : groups) s.push_back(g.second);
        return s;
    }
    if (straightHigh >= 0)          return {4, straightHigh};
    if (groups[0].first == 3)
        return {3, groups[0].second, groups[1].second, groups[2].second};
    if (groups[0].first == 2 && groups.size() > 1 && groups[1].first == 2)
        return {2, groups[0].second, groups[1].second, groups[2].second};
    if (groups[0].first == 2)
        return {1, groups[0].second, groups[1].second, groups[2].second, groups[3].second};

    std::vector<int> s = {0};
    for (const auto& g : groups) s.push_back(g.second);
    return s;
}

} // namespace

std::vector<int> evaluate7(const int cards[7])
{
    std::vector<int> best;
    int five[5];
    for (int a = 0; a < 7; ++a)
    for (int b = a + 1; b < 7; ++b)
    for (int cc = b + 1; cc < 7; ++cc)
    for (int dd = cc + 1; dd < 7; ++dd)
    for (int e = dd + 1; e < 7; ++e) {
        five[0] = cards[a]; five[1] = cards[b]; five[2] = cards[cc];
        five[3] = cards[dd]; five[4] = cards[e];
        const std::vector<int> score = evaluate5(five);
        if (best.empty() || score > best) best = score;
    }
    return best;
}

std::string handCategoryName(const std::vector<int>& score)
{
    if (score.empty()) return "—";
    switch (score[0]) {
        case 8: return "Straight Flush";
        case 7: return "Four of a Kind";
        case 6: return "Full House";
        case 5: return "Flush";
        case 4: return "Straight";
        case 3: return "Three of a Kind";
        case 2: return "Two Pair";
        case 1: return "Pair";
        default: return "High Card";
    }
}

} // namespace poker
