#include "poker_crypto.h"

#include <openssl/bn.h>
#include <openssl/crypto.h>   // OPENSSL_free

namespace poker {

namespace {

// RFC 3526 MODP Group 14 — a 2048-bit safe prime (p = 2q + 1, q prime).
const char* kPrimeHex =
    "FFFFFFFFFFFFFFFFC90FDAA22168C234C4C6628B80DC1CD1"
    "29024E088A67CC74020BBEA63B139B22514A08798E3404DD"
    "EF9519B3CD3A431B302B0A6DF25F14374FE1356D6D51C245"
    "E485B576625E7EC6F44C42E9A637ED6B0BFF5CB6F406B7ED"
    "EE386BFB5A899FA5AE9F24117C4B1FE649286651ECE45B3D"
    "C2007CB8A163BF0598DA48361C55D39A69163FA8FD24CF5F"
    "83655D23DCA3AD961C62F356208552BB9ED529077096966D"
    "670C354E4ABC9804F1746C08CA18217C32905E462E36CE3B"
    "E39E772C180E86039B2783A2EC07A28FB5C55DF06F4C52C9"
    "DE2BCBF6955817183995497CEA956AE515D2261898FA0510"
    "15728E5A8AACAA68FFFFFFFFFFFFFFFF";

// Shared prime, parsed once. Leaked at process exit (no teardown needed).
BIGNUM* sharedPrime()
{
    static BIGNUM* p = []() {
        BIGNUM* b = nullptr;
        BN_hex2bn(&b, kPrimeHex);
        return b;
    }();
    return p;
}

// Lowercase, BN-canonical hex for a value (round-tripped so formatting from
// different sources is irrelevant — we still compare cards with BN_cmp).
std::string toHex(const BIGNUM* v)
{
    char* h = BN_bn2hex(v);
    std::string s = h ? h : "";
    OPENSSL_free(h);
    for (char& c : s) {
        if (c >= 'A' && c <= 'F') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

BIGNUM* fromHex(const std::string& hex)
{
    BIGNUM* b = nullptr;
    if (BN_hex2bn(&b, hex.c_str()) == 0) {
        if (b) BN_free(b);
        return nullptr;
    }
    return b;
}

// r = base^exp mod p
std::string modExpHex(const std::string& baseHex, const BIGNUM* exp, BN_CTX* ctx)
{
    BIGNUM* base = fromHex(baseHex);
    if (!base) return "";
    BIGNUM* r = BN_new();
    std::string out;
    if (BN_mod_exp(r, base, exp, sharedPrime(), ctx) == 1) out = toHex(r);
    BN_free(r);
    BN_free(base);
    return out;
}

} // namespace

// ── Keyset implementation ─────────────────────────────────────────────────

struct SraKeyset::Impl {
    BN_CTX* ctx = nullptr;
    BIGNUM* e   = nullptr;             // shuffle encrypt exponent
    BIGNUM* d   = nullptr;             // shuffle decrypt exponent = e^-1 mod (p-1)
    BIGNUM* ePos[kDeckSize] = {};      // per-position encrypt exponents
    BIGNUM* dPos[kDeckSize] = {};      // per-position decrypt exponents

    // Fill (eOut, dOut) with a fresh exponent coprime to p-1 and its inverse.
    void genKeyPair(BIGNUM*& eOut, BIGNUM*& dOut, const BIGNUM* pm1)
    {
        eOut = BN_new();
        dOut = BN_new();
        BIGNUM* g = BN_new();
        do {
            BN_rand_range(eOut, pm1);     // 0 <= e < p-1
            BN_set_bit(eOut, 0);          // force odd (coprime to the factor 2)
            BN_gcd(g, eOut, pm1, ctx);
        } while (!BN_is_one(g) || BN_num_bits(eOut) < 256);
        BN_mod_inverse(dOut, eOut, pm1, ctx);
        BN_free(g);
    }
};

SraKeyset::SraKeyset() : d(new Impl)
{
    d->ctx = BN_CTX_new();

    BIGNUM* pm1 = BN_new();             // p - 1
    BN_sub(pm1, sharedPrime(), BN_value_one());

    d->genKeyPair(d->e, d->d, pm1);
    for (int i = 0; i < kDeckSize; ++i) {
        d->genKeyPair(d->ePos[i], d->dPos[i], pm1);
    }

    BN_free(pm1);
}

SraKeyset::~SraKeyset()
{
    if (d->e) BN_free(d->e);
    if (d->d) BN_free(d->d);
    for (int i = 0; i < kDeckSize; ++i) {
        if (d->ePos[i]) BN_free(d->ePos[i]);
        if (d->dPos[i]) BN_free(d->dPos[i]);
    }
    if (d->ctx) BN_CTX_free(d->ctx);
    delete d;
}

std::string SraKeyset::encryptShuffle(const std::string& valueHex) const
{
    return modExpHex(valueHex, d->e, d->ctx);
}

std::string SraKeyset::decryptShuffle(const std::string& valueHex) const
{
    return modExpHex(valueHex, d->d, d->ctx);
}

std::string SraKeyset::encryptCard(int pos, const std::string& valueHex) const
{
    if (pos < 0 || pos >= kDeckSize) return "";
    return modExpHex(valueHex, d->ePos[pos], d->ctx);
}

std::string SraKeyset::decryptCard(int pos, const std::string& valueHex) const
{
    if (pos < 0 || pos >= kDeckSize) return "";
    return modExpHex(valueHex, d->dPos[pos], d->ctx);
}

std::string SraKeyset::cardDecryptKeyHex(int pos) const
{
    if (pos < 0 || pos >= kDeckSize) return "";
    return toHex(d->dPos[pos]);
}

// ── Statics ─────────────────────────────────────────────────────────────

std::string SraKeyset::applyKey(const std::string& valueHex, const std::string& keyHex)
{
    BIGNUM* key = fromHex(keyHex);
    if (!key) return "";
    BN_CTX* ctx = BN_CTX_new();
    std::string out = modExpHex(valueHex, key, ctx);
    BN_CTX_free(ctx);
    BN_free(key);
    return out;
}

const std::vector<std::string>& SraKeyset::cardCodes()
{
    // code_i = (i + 2)^2 — distinct perfect squares, hence quadratic residues
    // mod p (the values are tiny, far below p, so the square is exact).
    static const std::vector<std::string> codes = []() {
        std::vector<std::string> v;
        v.reserve(kDeckSize);
        for (int i = 0; i < kDeckSize; ++i) {
            const long n = static_cast<long>(i + 2) * (i + 2);
            BIGNUM* b = BN_new();
            BN_set_word(b, static_cast<BN_ULONG>(n));
            v.push_back(toHex(b));
            BN_free(b);
        }
        return v;
    }();
    return codes;
}

int SraKeyset::cardIdForCode(const std::string& valueHex)
{
    BIGNUM* val = fromHex(valueHex);
    if (!val) return -1;
    int found = -1;
    const auto& codes = cardCodes();
    for (int i = 0; i < kDeckSize; ++i) {
        BIGNUM* c = fromHex(codes[i]);
        if (c && BN_cmp(val, c) == 0) found = i;
        if (c) BN_free(c);
        if (found >= 0) break;
    }
    BN_free(val);
    return found;
}

std::string SraKeyset::primeHex()
{
    static std::string p = toHex(sharedPrime());
    return p;
}

} // namespace poker
