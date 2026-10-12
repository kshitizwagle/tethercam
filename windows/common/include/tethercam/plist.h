#pragma once

// Minimal XML property-list support for the usbmuxd protocol (docs/TECH_SPEC.md §4).
// Only dict, array, string, integer, bool and data are supported, which is all usbmuxd uses.

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace tethercam::plist {

struct Value;
using Dict = std::map<std::string, Value>;
using Array = std::vector<Value>;
struct Data {
    std::vector<uint8_t> bytes;
};

struct Value {
    std::variant<std::monostate, std::string, int64_t, bool, Data, std::shared_ptr<Array>, std::shared_ptr<Dict>> v;

    Value() = default;
    Value(std::string s) : v(std::move(s)) {}
    Value(const char* s) : v(std::string(s)) {}
    Value(int64_t i) : v(i) {}
    Value(int i) : v(int64_t(i)) {}
    Value(bool b) : v(b) {}
    Value(Data d) : v(std::move(d)) {}
    Value(Array a) : v(std::make_shared<Array>(std::move(a))) {}
    Value(Dict d) : v(std::make_shared<Dict>(std::move(d))) {}

    const std::string* AsString() const { return std::get_if<std::string>(&v); }
    std::optional<int64_t> AsInt() const {
        if (auto p = std::get_if<int64_t>(&v)) return *p;
        return std::nullopt;
    }
    const Dict* AsDict() const {
        auto p = std::get_if<std::shared_ptr<Dict>>(&v);
        return p ? p->get() : nullptr;
    }
    const Array* AsArray() const {
        auto p = std::get_if<std::shared_ptr<Array>>(&v);
        return p ? p->get() : nullptr;
    }
    // Looks up a key when this is a dict.
    const Value* Get(std::string_view key) const;
};

std::string ToXml(const Value& root);
// Returns nullopt on malformed input. Nesting is limited to 32 levels.
std::optional<Value> FromXml(std::string_view xml);

}  // namespace tethercam::plist
