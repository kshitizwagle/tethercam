#pragma once

// Tiny thread-safe logger: stderr, an optional file, and OutputDebugString on Windows.

#include <filesystem>
#include <format>
#include <string>
#include <string_view>

namespace tethercam::log {

enum class Level { Debug, Info, Warn, Error };

void SetFile(const std::filesystem::path& path);
void SetMinLevel(Level level);
void Write(Level level, std::string_view msg);

template <class... A> void Info(std::format_string<A...> f, A&&... a) {
    Write(Level::Info, std::format(f, std::forward<A>(a)...));
}
template <class... A> void Warn(std::format_string<A...> f, A&&... a) {
    Write(Level::Warn, std::format(f, std::forward<A>(a)...));
}
template <class... A> void Error(std::format_string<A...> f, A&&... a) {
    Write(Level::Error, std::format(f, std::forward<A>(a)...));
}
template <class... A> void Debug(std::format_string<A...> f, A&&... a) {
    Write(Level::Debug, std::format(f, std::forward<A>(a)...));
}

}  // namespace tethercam::log
