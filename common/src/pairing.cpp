#include "tethercam/pairing.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>

#ifdef _WIN32
#include <windows.h>

#include <dpapi.h>
#include <shlobj.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <climits>
#endif

namespace tethercam::wifi {
namespace {

constexpr char kHeader[] = "tethercam-pairing 1";

bool Unhex(const std::string& s, std::vector<uint8_t>& out) {
    if (s.size() % 2) return false;
    out.clear();
    for (size_t i = 0; i < s.size(); i += 2) {
        char* end = nullptr;
        const std::string byte = s.substr(i, 2);
        const long v = std::strtol(byte.c_str(), &end, 16);
        if (end != byte.c_str() + 2) return false;
        out.push_back(static_cast<uint8_t>(v));
    }
    return true;
}

template <size_t N> bool UnhexFixed(const std::string& s, std::array<uint8_t, N>& out) {
    std::vector<uint8_t> v;
    if (!Unhex(s, v) || v.size() != N) return false;
    std::memcpy(out.data(), v.data(), N);
    return true;
}

// Text fields are hex so they never contain spaces; "-" stands for an empty string so the
// whitespace-separated fields never shift.
std::string HexStr(const std::string& s) {
    return s.empty() ? "-" : Hex({reinterpret_cast<const uint8_t*>(s.data()), s.size()});
}

bool UnhexStr(const std::string& s, std::string& out) {
    out.clear();
    if (s == "-") return true;
    std::vector<uint8_t> v;
    if (!Unhex(s, v)) return false;
    out.assign(v.begin(), v.end());
    return true;
}

#ifdef _WIN32
bool Protect(const std::string& in, std::string& out, bool protect) {
    DATA_BLOB src{static_cast<DWORD>(in.size()), reinterpret_cast<BYTE*>(const_cast<char*>(in.data()))};
    DATA_BLOB dst{};
    const BOOL ok = protect ? CryptProtectData(&src, L"Tethercam pairing", nullptr, nullptr, nullptr,
                                               CRYPTPROTECT_UI_FORBIDDEN, &dst)
                            : CryptUnprotectData(&src, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &dst);
    if (!ok) return false;
    out.assign(reinterpret_cast<const char*>(dst.pbData), dst.cbData);
    SecureZeroMemory(dst.pbData, dst.cbData);
    LocalFree(dst.pbData);
    return true;
}
#endif

}  // namespace

std::string Hex(std::span<const uint8_t> bytes) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string s;
    s.reserve(bytes.size() * 2);
    for (uint8_t b : bytes) {
        s += kDigits[b >> 4];
        s += kDigits[b & 15];
    }
    return s;
}

bool SplitHostPort(const std::string& s, std::string& host, uint16_t& port, uint16_t default_port) {
    port = default_port;
    std::string rest;
    if (!s.empty() && s[0] == '[') {
        const auto close = s.find(']');
        if (close == std::string::npos) return false;
        host = s.substr(1, close - 1);
        rest = s.substr(close + 1);
    } else if (std::count(s.begin(), s.end(), ':') > 1) {
        host = s;  // bare IPv6 address, no port
    } else {
        const auto colon = s.find(':');
        host = s.substr(0, colon);
        if (colon != std::string::npos) rest = s.substr(colon);
    }
    if (!rest.empty()) {
        if (rest[0] != ':') return false;
        const int p = std::atoi(rest.c_str() + 1);
        if (p <= 0 || p > 65535) return false;
        port = static_cast<uint16_t>(p);
    }
    return !host.empty();
}

std::vector<Candidate> Candidates(const PairingStore& store, std::span<const Discovered> found,
                                  const std::string& forced) {
    std::vector<Candidate> out;
    auto add = [&](const PairedPhone& p, const std::string& addr, uint16_t default_port) {
        Candidate c;
        c.phone = p;
        if (addr.empty() || !SplitHostPort(addr, c.host, c.port, default_port)) return;
        for (const auto& o : out)
            if (o.phone.device_id == p.device_id && o.host == c.host && o.port == c.port) return;
        out.push_back(std::move(c));
    };
    const auto confirmed = store.Confirmed();
    if (!forced.empty()) {
        if (!confirmed.empty()) add(*confirmed.front(), forced, proto::kWifiPort);
        return out;
    }
    for (const PairedPhone* p : confirmed) {
        add(*p, p->manual, p->port);
        const std::string id = Hex(p->device_id);
        for (const auto& d : found) {
            if (d.id_hex != id || d.host.empty()) continue;
            // An IPv6 literal needs brackets before a port is appended.
            const bool v6 = d.host.find(':') != std::string::npos;
            add(*p, (v6 ? "[" + d.host + "]" : d.host) + ":" + std::to_string(d.port ? d.port : p->port), p->port);
        }
        add(*p, p->address, p->port);
    }
    return out;
}

std::string LocalHostName() {
#ifdef _WIN32
    wchar_t buf[256];
    DWORD n = 256;
    if (!GetComputerNameExW(ComputerNamePhysicalDnsHostname, buf, &n)) return "PC";
    const int len = WideCharToMultiByte(CP_UTF8, 0, buf, int(n), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(len), ' ');
    WideCharToMultiByte(CP_UTF8, 0, buf, int(n), s.data(), len, nullptr, nullptr);
    return s;
#else
    char buf[HOST_NAME_MAX + 1] = {};
    if (gethostname(buf, sizeof(buf) - 1) != 0 || !buf[0]) return "PC";
    return buf;
#endif
}

PairingStore::PairingStore(std::string path) : path_(std::move(path)) { noise::RandomBytes(host_id_); }

std::string PairingStore::DefaultPath() {
#ifdef _WIN32
    wchar_t* base = nullptr;
    std::string out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base))) {
        const std::wstring w = std::wstring(base) + L"\\Tethercam\\pairing.bin";
        const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
        out.resize(size_t(n > 0 ? n - 1 : 0));
        if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out.data(), n, nullptr, nullptr);
    }
    CoTaskMemFree(base);
    return out;
#else
    if (const char* x = std::getenv("XDG_CONFIG_HOME"); x && *x) return std::string(x) + "/tethercam/pairing";
    if (const char* h = std::getenv("HOME"); h && *h) return std::string(h) + "/.config/tethercam/pairing";
    return {};
#endif
}

bool PairingStore::Load() {
    if (path_.empty()) return true;
    std::ifstream f(path_, std::ios::binary);
    if (!f) return true;  // nothing paired yet
    std::string raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::string text;
#ifdef _WIN32
    if (!Protect(raw, text, false)) return false;
#else
    text = std::move(raw);
#endif
    return Parse(text);
}

bool PairingStore::Save() const {
    if (path_.empty()) return true;
    const std::string text = Serialize();
#ifdef _WIN32
    std::string data;
    if (!Protect(text, data, true)) return false;
    const auto slash = path_.find_last_of("\\/");
    if (slash != std::string::npos) {
        const std::string dir = path_.substr(0, slash);
        const int n = MultiByteToWideChar(CP_UTF8, 0, dir.c_str(), -1, nullptr, 0);
        std::wstring wdir(size_t(n), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, dir.c_str(), -1, wdir.data(), n);
        SHCreateDirectoryExW(nullptr, wdir.c_str(), nullptr);
    }
    const std::string tmp = path_ + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f.write(data.data(), std::streamsize(data.size()))) return false;
    }
    const auto wide = [](const std::string& s) {
        const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
        std::wstring w(size_t(n), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
        return w;
    };
    return MoveFileExW(wide(tmp).c_str(), wide(path_).c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
#else
    // Create the directory chain (0700) and write the file 0600 before any key touches the disk.
    for (size_t i = 1; i < path_.size(); ++i)
        if (path_[i] == '/') ::mkdir(path_.substr(0, i).c_str(), 0700);
    const std::string tmp = path_ + ".tmp";
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    size_t done = 0;
    while (done < text.size()) {
        const ssize_t n = ::write(fd, text.data() + done, text.size() - done);
        if (n <= 0) {
            ::close(fd);
            return false;
        }
        done += size_t(n);
    }
    ::fsync(fd);
    ::close(fd);
    return ::rename(tmp.c_str(), path_.c_str()) == 0;
#endif
}

PairedPhone& PairingStore::ForSerial(const std::string& serial) {
    for (auto& p : phones_)
        if (p.serial == serial) return p;
    PairedPhone p;
    p.serial = serial;
    noise::RandomBytes(p.key);
    phones_.push_back(std::move(p));
    return phones_.back();
}

std::vector<const PairedPhone*> PairingStore::Confirmed() const {
    std::vector<const PairedPhone*> out;
    // Apply() moves the latest confirmation to the front.
    for (const auto& p : phones_)
        if (p.confirmed) out.push_back(&p);
    return out;
}

bool PairingStore::Apply(const std::string& serial, const proto::WifiPaired& reply) {
    if (reply.status != proto::WifiPairStatus::Paired) return false;
    PairedPhone& p = ForSerial(serial);
    const bool changed = !p.confirmed || p.device_id != reply.device_id || p.name != reply.name ||
                         p.address != reply.address || p.port != reply.port;
    p.confirmed = true;
    p.device_id = reply.device_id;
    p.name = reply.name;
    p.address = reply.address;
    p.port = reply.port ? reply.port : proto::kWifiPort;
    // The same phone reached through another serial (a fakephone address change) is stale now.
    std::erase_if(phones_, [&](const PairedPhone& o) { return &o != &p && o.device_id == p.device_id; });
    // Most recently confirmed first.
    for (size_t i = 0; i < phones_.size(); ++i) {
        if (phones_[i].serial == serial && i != 0) {
            PairedPhone moved = std::move(phones_[i]);
            phones_.erase(phones_.begin() + ptrdiff_t(i));
            phones_.insert(phones_.begin(), std::move(moved));
            break;
        }
    }
    return changed;
}

void PairingStore::ForgetAll() {
    for (auto& p : phones_) noise::RandomBytes(p.key);  // overwrite before dropping
    phones_.clear();
}

void PairingStore::SetManual(const std::string& serial, const std::string& manual) {
    for (auto& p : phones_)
        if (p.serial == serial) p.manual = manual;
}

std::string PairingStore::Serialize() const {
    std::ostringstream o;
    o << kHeader << "\nhost " << Hex(host_id_) << "\n";
    for (const auto& p : phones_) {
        o << "phone " << HexStr(p.serial) << ' ' << Hex(p.key) << ' ' << (p.confirmed ? 1 : 0) << ' '
          << Hex(p.device_id) << ' ' << p.port << ' ' << HexStr(p.name) << ' ' << HexStr(p.address) << ' '
          << HexStr(p.manual) << "\n";
    }
    return o.str();
}

bool PairingStore::Parse(const std::string& text) {
    std::istringstream in(text);
    std::string line;
    if (!std::getline(in, line) || line != kHeader) return false;
    HostId host{};
    std::vector<PairedPhone> phones;
    bool have_host = false;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        std::string kind;
        ls >> kind;
        if (kind == "host") {
            std::string h;
            ls >> h;
            if (!UnhexFixed(h, host)) return false;
            have_host = true;
        } else if (kind == "phone") {
            std::string serial, key, device, name, address, manual;
            int confirmed = 0;
            unsigned port = 0;
            ls >> serial >> key >> confirmed >> device >> port >> name >> address >> manual;
            PairedPhone p;
            if (!UnhexStr(serial, p.serial) || !UnhexFixed(key, p.key) || !UnhexFixed(device, p.device_id) ||
                !UnhexStr(name, p.name) || !UnhexStr(address, p.address) || !UnhexStr(manual, p.manual) || port > 65535)
                return false;
            p.confirmed = confirmed != 0;
            p.port = port ? static_cast<uint16_t>(port) : proto::kWifiPort;
            phones.push_back(std::move(p));
        }
    }
    if (!have_host) return false;
    host_id_ = host;
    phones_ = std::move(phones);
    return true;
}

}  // namespace tethercam::wifi
