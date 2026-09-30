#ifndef POKER_CRYPTO_H
#define POKER_CRYPTO_H

#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────
// Mental-poker (Shamir–Rivest–Adleman) commutative cipher.
//
// SRA encryption is c = m^e mod p, decryption is m = c^d mod p with
// d = e^-1 mod (p-1). It is *commutative*: (m^a)^b == (m^b)^a mod p, which is
// exactly what lets several players each encrypt the deck in turn, shuffle, and
// later peel their layers off in any order.
//
// Card values are encoded as quadratic residues mod p (perfect squares), so the
// Legendre symbol of a ciphertext equals that of its plaintext and therefore
// leaks no information — every card has Legendre symbol +1.
//
// `p` is RFC 3526 MODP Group 14, a 2048-bit *safe* prime (p = 2q + 1, q prime),
// so any odd e that is not a multiple of q is coprime to p-1 = 2q.
//
// All big integers cross the API / wire as hex strings.
// ─────────────────────────────────────────────────────────────────────────

namespace poker {

constexpr int kDeckSize = 52;

class SraKeyset {
public:
    SraKeyset();   // generates a fresh shuffle key + 52 per-card keys
    ~SraKeyset();
    SraKeyset(const SraKeyset&) = delete;
    SraKeyset& operator=(const SraKeyset&) = delete;

    // Whole-deck single key — used during the shuffle phase.
    std::string encryptShuffle(const std::string& valueHex) const;   // m^e   mod p
    std::string decryptShuffle(const std::string& valueHex) const;   // c^d   mod p

    // Per-position key — used during the lock phase. After locking, the card at
    // each board position is encrypted under every player's per-position key.
    std::string encryptCard(int pos, const std::string& valueHex) const;  // m^e_pos mod p
    std::string decryptCard(int pos, const std::string& valueHex) const;  // c^d_pos mod p

    // The per-position decryption key, published to reveal the card at `pos`.
    std::string cardDecryptKeyHex(int pos) const;                         // d_pos as hex

    // Apply somebody else's published per-position decryption key to a value.
    static std::string applyKey(const std::string& valueHex, const std::string& keyHex);

    // ── Shared, deterministic deck constants (identical on every peer) ──

    // The 52 plaintext card codes (quadratic residues), as hex. Index = card id.
    static const std::vector<std::string>& cardCodes();
    // Recover the card id (0..51) from a fully-decrypted code; -1 if not a code.
    static int cardIdForCode(const std::string& valueHex);

    static std::string primeHex();

private:
    struct Impl;
    Impl* d;
};

} // namespace poker

#endif // POKER_CRYPTO_H
