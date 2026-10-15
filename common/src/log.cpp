#include "tethercam/log.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <chrono>
#include <cstdio>
#include <mutex>

namespace tethercam::log {
namespace {

std::mutex g_mu;
FILE* g_file = nullptr;
Level g_min = Level::Info;

const char* Name(Level l) {
    switch (l) {
    case Level::Debug: return "DEBUG";
    case Level::Info: return "INFO ";
    case Level::Warn: return "WARN ";
    case Level::Error: return "ERROR";
    }
    return "?";
}

}  // namespace

void SetFile(const std::filesystem::path& path) {
    std::lock_guard lock(g_mu);
    if (g_file) fclose(g_file);
#ifdef _WIN32
    g_file = _wfsopen(path.c_str(), L"a", _SH_DENYWR);
#else
    g_file = fopen(path.c_str(), "a");
#endif
}

void SetMinLevel(Level level) { g_min = level; }

void Write(Level level, std::string_view msg) {
    if (level < g_min) return;
    const auto now = std::chrono::system_clock::now();
    const std::string line = std::format("{:%H:%M:%S} {} {}\n", std::chrono::floor<std::chrono::milliseconds>(now),
                                         Name(level), msg);
    std::lock_guard lock(g_mu);
    fputs(line.c_str(), stderr);
    if (g_file) {
        fputs(line.c_str(), g_file);
        fflush(g_file);
    }
#ifdef _WIN32
    OutputDebugStringA(line.c_str());
#endif
}

}  // namespace tethercam::log
