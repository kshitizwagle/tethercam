#include "tethercam/noise.h"

#include <cstdlib>
#include <cstring>
#include <string_view>

#include "monocypher.h"

#ifdef _WIN32
#include <windows.h>

#include <bcrypt.h>
#else
#include <sys/random.h>
#endif

namespace tethercam::noise {
namespace {

constexpr std::string_view kProtocolName = "Noise_NNpsk0_25519_ChaChaPoly_BLAKE2b";
constexpr size_t kBlockSize = 128;  // BLAKE2b block length, for HMAC
constexpr size_t kDhSize = 32;

using Hash = std::array<uint8_t, kHashSize>;

// HMAC-BLAKE2b (RFC 2104), as Noise's HKDF requires; keys here are never longer than a block.
Hash Hmac(std::span<const uint8_t> key, std::span<const uint8_t> a, std::span<const uint8_t> b = {}) {
    uint8_t pad[kBlockSize] = {};
    std::memcpy(pad, key.data(), key.size());
    for (auto& c : pad) c ^= 0x36;
    Hash inner, out;
    crypto_blake2b_ctx ctx;
    crypto_blake2b_init(&ctx, kHashSize);
    crypto_blake2b_update(&ctx, pad, kBlockSize);
    crypto_blake2b_update(&ctx, a.data(), a.size());
    crypto_blake2b_update(&ctx, b.data(), b.size());
    crypto_blake2b_final(&ctx, inner.data());
    for (auto& c : pad) c ^= 0x36 ^ 0x5c;
    crypto_blake2b_init(&ctx, kHashSize);
    crypto_blake2b_update(&ctx, pad, kBlockSize);
    crypto_blake2b_update(&ctx, inner.data(), inner.size());
    crypto_blake2b_final(&ctx, out.data());
    crypto_wipe(pad, sizeof(pad));
    crypto_wipe(inner.data(), inner.size());
    return out;
}

// Noise HKDF with two or three outputs.
void Hkdf(const Hash& ck, std::span<const uint8_t> ikm, Hash& o1, Hash& o2, Hash* o3 = nullptr) {
    const Hash temp = Hmac(ck, ikm);
    const uint8_t one = 1, two = 2, three = 3;
    o1 = Hmac(temp, {&one, 1});
    o2 = Hmac(temp, o1, {&two, 1});
    if (o3) *o3 = Hmac(temp, o2, {&three, 1});
}

void Nonce(uint64_t n, uint8_t out[12]) {
    std::memset(out, 0, 4);
    for (int i = 0; i < 8; ++i) out[4 + i] = static_cast<uint8_t>(n >> (8 * i));
}

}  // namespace

void RandomBytes(std::span<uint8_t> out) {
#ifdef _WIN32
    if (BCryptGenRandom(nullptr, out.data(), static_cast<ULONG>(out.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        std::abort();
#else
    size_t done = 0;
    while (done < out.size()) {
        const ssize_t n = getrandom(out.data() + done, out.size() - done, 0);
        if (n < 0) std::abort();
        done += size_t(n);
    }
#endif
}

void CipherState::SetKey(std::span<const uint8_t, kKeySize> key) {
    std::memcpy(key_.data(), key.data(), kKeySize);
    n_ = 0;
    has_key_ = true;
}

CipherState::~CipherState() { crypto_wipe(key_.data(), key_.size()); }

void CipherState::Encrypt(std::span<const uint8_t> ad, std::span<const uint8_t> plaintext, uint8_t* out) {
    uint8_t nonce[12];
    Nonce(n_++, nonce);
    // A fresh context per message is the standard (RFC 8439) construction; Monocypher only ratchets
    // the key when a context is reused.
    crypto_aead_ctx ctx;
    crypto_aead_init_ietf(&ctx, key_.data(), nonce);
    crypto_aead_write(&ctx, out, out + plaintext.size(), ad.data(), ad.size(), plaintext.data(), plaintext.size());
    crypto_wipe(&ctx, sizeof(ctx));
}

bool CipherState::Decrypt(std::span<const uint8_t> ad, std::span<const uint8_t> ciphertext, uint8_t* out) {
    if (ciphertext.size() < kTagSize) return false;
    const size_t len = ciphertext.size() - kTagSize;
    uint8_t nonce[12];
    Nonce(n_, nonce);
    crypto_aead_ctx ctx;
    crypto_aead_init_ietf(&ctx, key_.data(), nonce);
    const int bad = crypto_aead_read(&ctx, out, ciphertext.data() + len, ad.data(), ad.size(), ciphertext.data(), len);
    crypto_wipe(&ctx, sizeof(ctx));
    if (bad) return false;
    ++n_;
    return true;
}

Handshake::Handshake(bool initiator, std::span<const uint8_t> prologue, std::span<const uint8_t, kKeySize> psk,
                     std::span<const uint8_t> ephemeral)
    : initiator_(initiator) {
    static_assert(kProtocolName.size() <= kHashSize);
    std::memcpy(h_.data(), kProtocolName.data(), kProtocolName.size());  // padded with zeros
    ck_ = h_;
    MixHash(prologue);
    std::memcpy(psk_.data(), psk.data(), kKeySize);
    if (ephemeral.size() == kDhSize) {
        std::memcpy(e_priv_.data(), ephemeral.data(), kDhSize);
        fixed_ephemeral_ = true;
    }
}

Handshake::~Handshake() {
    crypto_wipe(ck_.data(), ck_.size());
    crypto_wipe(psk_.data(), psk_.size());
    crypto_wipe(e_priv_.data(), e_priv_.size());
}

void Handshake::MixHash(std::span<const uint8_t> data) {
    crypto_blake2b_ctx ctx;
    crypto_blake2b_init(&ctx, kHashSize);
    crypto_blake2b_update(&ctx, h_.data(), h_.size());
    crypto_blake2b_update(&ctx, data.data(), data.size());
    crypto_blake2b_final(&ctx, h_.data());
}

void Handshake::MixKey(std::span<const uint8_t> ikm) {
    Hash k;
    Hkdf(ck_, ikm, ck_, k);
    cs_.SetKey(std::span<const uint8_t, kKeySize>(k.data(), kKeySize));
    crypto_wipe(k.data(), k.size());
}

void Handshake::MixKeyAndHash(std::span<const uint8_t> ikm) {
    Hash th, k;
    Hkdf(ck_, ikm, ck_, th, &k);
    MixHash(th);
    cs_.SetKey(std::span<const uint8_t, kKeySize>(k.data(), kKeySize));
    crypto_wipe(k.data(), k.size());
}

void Handshake::EncryptAndHash(std::span<const uint8_t> plaintext, std::vector<uint8_t>& out) {
    const size_t at = out.size();
    out.resize(at + plaintext.size() + kTagSize);
    cs_.Encrypt(h_, plaintext, out.data() + at);
    MixHash({out.data() + at, out.size() - at});
}

bool Handshake::DecryptAndHash(std::span<const uint8_t> ciphertext, std::vector<uint8_t>& out) {
    if (ciphertext.size() < kTagSize) return false;
    out.resize(ciphertext.size() - kTagSize);
    if (!cs_.Decrypt(h_, ciphertext, out.data())) return false;
    MixHash(ciphertext);
    return true;
}

bool Handshake::WriteEphemeral(std::vector<uint8_t>& out) {
    if (!fixed_ephemeral_) RandomBytes(e_priv_);
    crypto_x25519_public_key(e_pub_.data(), e_priv_.data());
    out.insert(out.end(), e_pub_.begin(), e_pub_.end());
    MixHash(e_pub_);
    MixKey(e_pub_);  // psk handshakes mix the ephemeral into the key as well
    return true;
}

bool Handshake::ReadEphemeral(std::span<const uint8_t>& in) {
    if (in.size() < kDhSize) return false;
    std::memcpy(re_.data(), in.data(), kDhSize);
    in = in.subspan(kDhSize);
    MixHash(re_);
    MixKey(re_);
    return true;
}

std::vector<uint8_t> Handshake::Write(std::span<const uint8_t> payload) {
    std::vector<uint8_t> out;
    const bool my_turn = initiator_ ? step_ == 0 : step_ == 1;
    if (!my_turn || payload.size() > kMaxMessage - kDhSize - kTagSize) return out;
    if (initiator_) {
        MixKeyAndHash(psk_);
        WriteEphemeral(out);
    } else {
        WriteEphemeral(out);
        uint8_t shared[kDhSize];
        crypto_x25519(shared, e_priv_.data(), re_.data());
        MixKey(shared);
        crypto_wipe(shared, sizeof(shared));
    }
    EncryptAndHash(payload, out);
    ++step_;
    return out;
}

bool Handshake::Read(std::span<const uint8_t> message, std::vector<uint8_t>& payload) {
    const bool their_turn = initiator_ ? step_ == 1 : step_ == 0;
    if (!their_turn || message.size() > kMaxMessage) return false;
    if (!initiator_) {
        MixKeyAndHash(psk_);
        if (!ReadEphemeral(message)) return false;
    } else {
        if (!ReadEphemeral(message)) return false;
        uint8_t shared[kDhSize];
        crypto_x25519(shared, e_priv_.data(), re_.data());
        MixKey(shared);
        crypto_wipe(shared, sizeof(shared));
    }
    if (!DecryptAndHash(message, payload)) return false;
    ++step_;
    return true;
}

void Handshake::Split(CipherState& send, CipherState& recv) const {
    Hash k1, k2;
    Hkdf(ck_, {}, k1, k2);
    const std::span<const uint8_t, kKeySize> a(k1.data(), kKeySize), b(k2.data(), kKeySize);
    send.SetKey(initiator_ ? a : b);
    recv.SetKey(initiator_ ? b : a);
    crypto_wipe(k1.data(), k1.size());
    crypto_wipe(k2.data(), k2.size());
}

}  // namespace tethercam::noise
