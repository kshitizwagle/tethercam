#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tethercam {

// Little-endian writer used by the wire protocol (docs/PROTOCOL.md).
class ByteWriter {
public:
    void U8(uint8_t v) { buf_.push_back(v); }
    void U16(uint16_t v) { Put(v); }
    void U32(uint32_t v) { Put(v); }
    void U64(uint64_t v) { Put(v); }
    void Bytes(std::span<const uint8_t> b) { buf_.insert(buf_.end(), b.begin(), b.end()); }
    // u16 length + UTF-8 bytes. Strings longer than 65535 bytes are truncated.
    void Str(std::string_view s) {
        const size_t n = s.size() > 0xFFFF ? 0xFFFF : s.size();
        U16(static_cast<uint16_t>(n));
        buf_.insert(buf_.end(), s.begin(), s.begin() + n);
    }
    std::vector<uint8_t>& Data() { return buf_; }
    std::vector<uint8_t> Take() { return std::move(buf_); }

private:
    template <typename T> void Put(T v) {
        for (size_t i = 0; i < sizeof(T); ++i) buf_.push_back(static_cast<uint8_t>(v >> (8 * i)));
    }
    std::vector<uint8_t> buf_;
};

// Bounds-checked little-endian reader. Any read past the end sets Failed() and returns zeros.
class ByteReader {
public:
    explicit ByteReader(std::span<const uint8_t> data) : data_(data) {}

    uint8_t U8() { return Get<uint8_t>(); }
    uint16_t U16() { return Get<uint16_t>(); }
    uint32_t U32() { return Get<uint32_t>(); }
    uint64_t U64() { return Get<uint64_t>(); }
    std::string Str() {
        const uint16_t n = U16();
        if (!Need(n)) return {};
        std::string s(reinterpret_cast<const char*>(data_.data() + pos_), n);
        pos_ += n;
        return s;
    }
    std::span<const uint8_t> Bytes(size_t n) {
        if (!Need(n)) return {};
        auto s = data_.subspan(pos_, n);
        pos_ += n;
        return s;
    }
    std::span<const uint8_t> Rest() {
        auto s = data_.subspan(pos_);
        pos_ = data_.size();
        return s;
    }
    size_t Remaining() const { return data_.size() - pos_; }
    bool Failed() const { return failed_; }

private:
    bool Need(size_t n) {
        if (failed_ || data_.size() - pos_ < n) {
            failed_ = true;
            return false;
        }
        return true;
    }
    template <typename T> T Get() {
        if (!Need(sizeof(T))) return 0;
        T v = 0;
        for (size_t i = 0; i < sizeof(T); ++i) v |= static_cast<T>(static_cast<T>(data_[pos_ + i]) << (8 * i));
        pos_ += sizeof(T);
        return v;
    }
    std::span<const uint8_t> data_;
    size_t pos_ = 0;
    bool failed_ = false;
};

}  // namespace tethercam
