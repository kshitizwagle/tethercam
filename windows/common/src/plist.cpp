#include "tethercam/plist.h"

#include <charconv>

namespace tethercam::plist {
namespace {

constexpr char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string Escape(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        default: out += c;
        }
    }
    return out;
}

std::string Unescape(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') {
            out += s[i];
            continue;
        }
        auto semi = s.find(';', i);
        if (semi == std::string_view::npos) {
            out += s[i];
            continue;
        }
        auto ent = s.substr(i + 1, semi - i - 1);
        if (ent == "amp") out += '&';
        else if (ent == "lt") out += '<';
        else if (ent == "gt") out += '>';
        else if (ent == "quot") out += '"';
        else if (ent == "apos") out += '\'';
        else out.append(s.substr(i, semi - i + 1));
        i = semi;
    }
    return out;
}

std::string Base64(const std::vector<uint8_t>& b) {
    std::string out;
    for (size_t i = 0; i < b.size(); i += 3) {
        uint32_t n = uint32_t(b[i]) << 16;
        if (i + 1 < b.size()) n |= uint32_t(b[i + 1]) << 8;
        if (i + 2 < b.size()) n |= b[i + 2];
        out += kB64[(n >> 18) & 63];
        out += kB64[(n >> 12) & 63];
        out += i + 1 < b.size() ? kB64[(n >> 6) & 63] : '=';
        out += i + 2 < b.size() ? kB64[n & 63] : '=';
    }
    return out;
}

std::vector<uint8_t> Unbase64(std::string_view s) {
    std::vector<uint8_t> out;
    uint32_t acc = 0;
    int bits = 0;
    for (char c : s) {
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+') v = 62;
        else if (c == '/') v = 63;
        else continue;  // whitespace and padding
        acc = (acc << 6) | uint32_t(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>(acc >> bits));
        }
    }
    return out;
}

void Write(std::string& out, const Value& v) {
    if (auto s = std::get_if<std::string>(&v.v)) {
        out += "<string>" + Escape(*s) + "</string>";
    } else if (auto i = std::get_if<int64_t>(&v.v)) {
        out += "<integer>" + std::to_string(*i) + "</integer>";
    } else if (auto b = std::get_if<bool>(&v.v)) {
        out += *b ? "<true/>" : "<false/>";
    } else if (auto d = std::get_if<Data>(&v.v)) {
        out += "<data>" + Base64(d->bytes) + "</data>";
    } else if (auto a = v.AsArray()) {
        out += "<array>";
        for (const auto& e : *a) Write(out, e);
        out += "</array>";
    } else if (auto dict = v.AsDict()) {
        out += "<dict>";
        for (const auto& [k, e] : *dict) {
            out += "<key>" + Escape(k) + "</key>";
            Write(out, e);
        }
        out += "</dict>";
    }
}

class Parser {
public:
    explicit Parser(std::string_view s) : s_(s) {}

    std::optional<Value> ParseDocument() {
        // Skip the XML declaration, DOCTYPE and <plist ...> wrapper.
        auto start = s_.find("<plist");
        if (start == std::string_view::npos) return std::nullopt;
        auto gt = s_.find('>', start);
        if (gt == std::string_view::npos) return std::nullopt;
        pos_ = gt + 1;
        auto v = ParseValue(0);
        return v;
    }

private:
    void SkipWs() {
        while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\n' || s_[pos_] == '\r' || s_[pos_] == '\t'))
            ++pos_;
    }

    // Reads "<name>" or "<name/>" and returns the tag name; sets selfClosing.
    std::optional<std::string_view> ReadTag(bool& selfClosing) {
        SkipWs();
        if (pos_ >= s_.size() || s_[pos_] != '<') return std::nullopt;
        auto gt = s_.find('>', pos_);
        if (gt == std::string_view::npos) return std::nullopt;
        auto tag = s_.substr(pos_ + 1, gt - pos_ - 1);
        selfClosing = !tag.empty() && tag.back() == '/';
        if (selfClosing) tag.remove_suffix(1);
        pos_ = gt + 1;
        return tag;
    }

    std::optional<std::string_view> ReadTextUntil(std::string_view closeTag) {
        auto end = s_.find(closeTag, pos_);
        if (end == std::string_view::npos) return std::nullopt;
        auto text = s_.substr(pos_, end - pos_);
        pos_ = end + closeTag.size();
        return text;
    }

    bool PeekClose(std::string_view closeTag) {
        SkipWs();
        if (s_.substr(pos_, closeTag.size()) == closeTag) {
            pos_ += closeTag.size();
            return true;
        }
        return false;
    }

    std::optional<Value> ParseValue(int depth) {
        if (depth > 32) return std::nullopt;
        bool selfClosing = false;
        auto tag = ReadTag(selfClosing);
        if (!tag) return std::nullopt;
        if (*tag == "true") return Value(true);
        if (*tag == "false") return Value(false);
        if (selfClosing) {
            if (*tag == "string") return Value(std::string());
            if (*tag == "dict") return Value(Dict{});
            if (*tag == "array") return Value(Array{});
            if (*tag == "data") return Value(Data{});
            return std::nullopt;
        }
        if (*tag == "string") {
            auto t = ReadTextUntil("</string>");
            if (!t) return std::nullopt;
            return Value(Unescape(*t));
        }
        if (*tag == "integer") {
            auto t = ReadTextUntil("</integer>");
            if (!t) return std::nullopt;
            int64_t n = 0;
            auto [p, ec] = std::from_chars(t->data(), t->data() + t->size(), n);
            if (ec != std::errc()) return std::nullopt;
            return Value(n);
        }
        if (*tag == "data") {
            auto t = ReadTextUntil("</data>");
            if (!t) return std::nullopt;
            return Value(Data{Unbase64(*t)});
        }
        if (*tag == "real" || *tag == "date") {
            // Not used by usbmuxd; keep the text so parsing can continue.
            std::string close = "</" + std::string(*tag) + ">";
            auto t = ReadTextUntil(close);
            if (!t) return std::nullopt;
            return Value(std::string(*t));
        }
        if (*tag == "array") {
            Array a;
            while (!PeekClose("</array>")) {
                auto e = ParseValue(depth + 1);
                if (!e) return std::nullopt;
                a.push_back(std::move(*e));
            }
            return Value(std::move(a));
        }
        if (*tag == "dict") {
            Dict d;
            while (!PeekClose("</dict>")) {
                bool sc = false;
                auto k = ReadTag(sc);
                if (!k || *k != "key" || sc) return std::nullopt;
                auto key = ReadTextUntil("</key>");
                if (!key) return std::nullopt;
                auto e = ParseValue(depth + 1);
                if (!e) return std::nullopt;
                d[Unescape(*key)] = std::move(*e);
            }
            return Value(std::move(d));
        }
        return std::nullopt;
    }

    std::string_view s_;
    size_t pos_ = 0;
};

}  // namespace

const Value* Value::Get(std::string_view key) const {
    auto d = AsDict();
    if (!d) return nullptr;
    auto it = d->find(std::string(key));
    return it == d->end() ? nullptr : &it->second;
}

std::string ToXml(const Value& root) {
    std::string out =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<plist version=\"1.0\">";
    Write(out, root);
    out += "</plist>\n";
    return out;
}

std::optional<Value> FromXml(std::string_view xml) { return Parser(xml).ParseDocument(); }

}  // namespace tethercam::plist
