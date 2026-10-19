#pragma once

// Noise_NNpsk0_25519_ChaChaPoly_BLAKE2b (noiseprotocol.org, revision 34) on Monocypher: the handshake
// that opens every Wi-Fi session on the pairing key exchanged over USB (docs/PROTOCOL.md § Wireless
// transport). Platform-neutral; the iOS app implements the same construction on the same library.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace tethercam::noise {

inline constexpr size_t kKeySize = 32;
inline constexpr size_t kTagSize = 16;
inline constexpr size_t kHashSize = 64;
inline constexpr size_t kMaxMessage = 65535;  // Noise's limit on one message, tag included
inline constexpr size_t kMaxPlaintext = kMaxMessage - kTagSize;

using Key = std::array<uint8_t, kKeySize>;

// Fills `out` from the operating system's CSPRNG. Aborts if the OS can't provide randomness.
void RandomBytes(std::span<uint8_t> out);

// ChaCha20-Poly1305 with Noise's nonce layout (32 zero bits, then a 64-bit little-endian counter).
class CipherState {
public:
    void SetKey(std::span<const uint8_t, kKeySize> key);
    bool HasKey() const { return has_key_; }
    // Writes plaintext.size() + kTagSize bytes to `out`.
    void Encrypt(std::span<const uint8_t> ad, std::span<const uint8_t> plaintext, uint8_t* out);
    // Writes ciphertext.size() - kTagSize bytes to `out`. False if the tag doesn't verify (the nonce
    // is not advanced, and the caller must drop the connection).
    bool Decrypt(std::span<const uint8_t> ad, std::span<const uint8_t> ciphertext, uint8_t* out);
    ~CipherState();

private:
    Key key_{};
    uint64_t n_ = 0;
    bool has_key_ = false;
};

// One NNpsk0 handshake: initiator (host) writes message 1, responder (device) answers with message 2.
//   -> psk, e
//   <- e, ee
class Handshake {
public:
    // `ephemeral` fixes the ephemeral private key (test vectors only); empty = random.
    Handshake(bool initiator, std::span<const uint8_t> prologue, std::span<const uint8_t, kKeySize> psk,
              std::span<const uint8_t> ephemeral = {});
    ~Handshake();
    Handshake(const Handshake&) = delete;
    Handshake& operator=(const Handshake&) = delete;

    // Next handshake message carrying `payload` (encrypted). Empty if it isn't this side's turn.
    std::vector<uint8_t> Write(std::span<const uint8_t> payload = {});
    // Processes the peer's message; false if it is malformed or fails authentication.
    bool Read(std::span<const uint8_t> message, std::vector<uint8_t>& payload);
    bool Done() const { return step_ == 2; }
    // After Done(): the transport ciphers for this side.
    void Split(CipherState& send, CipherState& recv) const;
    const std::array<uint8_t, kHashSize>& HandshakeHash() const { return h_; }

private:
    void MixHash(std::span<const uint8_t> data);
    void MixKey(std::span<const uint8_t> ikm);
    void MixKeyAndHash(std::span<const uint8_t> ikm);
    void EncryptAndHash(std::span<const uint8_t> plaintext, std::vector<uint8_t>& out);
    bool DecryptAndHash(std::span<const uint8_t> ciphertext, std::vector<uint8_t>& out);
    bool WriteEphemeral(std::vector<uint8_t>& out);
    bool ReadEphemeral(std::span<const uint8_t>& in);

    bool initiator_;
    int step_ = 0;  // messages processed
    std::array<uint8_t, kHashSize> ck_{}, h_{};
    CipherState cs_;
    Key psk_{}, e_priv_{}, e_pub_{}, re_{};
    bool fixed_ephemeral_ = false;
};

}  // namespace tethercam::noise
