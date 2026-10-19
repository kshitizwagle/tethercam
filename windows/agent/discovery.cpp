// Bonjour discovery on Windows through the OS resolver (DnsServiceBrowse, Windows 10+): no Bonjour
// install and no firewall rule needed, because the DNS Client service does the multicast traffic.

#include <winsock2.h>
#include <windows.h>
#include <windns.h>
#include <ws2tcpip.h>

#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "tethercam/discovery.h"
#include "tethercam/log.h"

namespace tethercam::wifi {
namespace {

std::string Narrow(const wchar_t* w) {
    if (!w || !*w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n > 0 ? n - 1 : 0), '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

class WinDiscovery final : public Discovery {
public:
    bool Start() {
        DNS_SERVICE_BROWSE_REQUEST req{};
        req.Version = DNS_QUERY_REQUEST_VERSION1;
        req.InterfaceIndex = 0;
        req.QueryName = L"_tethercam._tcp.local";
        req.pBrowseCallback = &WinDiscovery::OnBrowse;
        req.pQueryContext = this;
        const DNS_STATUS st = DnsServiceBrowse(&req, &browse_cancel_);
        if (st != DNS_REQUEST_PENDING) {
            log::Warn("Bonjour discovery unavailable (DnsServiceBrowse {})", st);
            return false;
        }
        browsing_ = true;
        return true;
    }

    ~WinDiscovery() override {
        std::unique_lock lock(mu_);
        stopping_ = true;
        std::vector<DNS_SERVICE_CANCEL*> pending;
        for (auto& [name, r] : resolving_) pending.push_back(&r->cancel);
        lock.unlock();
        // Without the lock: a cancelled query may complete through its callback right away.
        for (auto* c : pending) DnsServiceResolveCancel(c);
        if (browsing_) DnsServiceBrowseCancel(&browse_cancel_);
        // Cancelled queries still complete through their callbacks; wait for them to drain.
        for (int i = 0; i < 100; ++i) {
            lock.lock();
            const bool idle = resolving_.empty() && !in_browse_;
            lock.unlock();
            if (idle) break;
            Sleep(10);
        }
    }

    std::vector<Discovered> Found() const override {
        std::lock_guard lock(mu_);
        std::vector<Discovered> out;
        for (const auto& [name, d] : found_) out.push_back(d);
        return out;
    }

private:
    struct Resolve {
        WinDiscovery* self = nullptr;
        std::wstring instance;
        DNS_SERVICE_CANCEL cancel{};
    };

    static VOID WINAPI OnBrowse(DWORD status, PVOID ctx, PDNS_RECORD records) {
        auto* self = static_cast<WinDiscovery*>(ctx);
        {
            std::lock_guard lock(self->mu_);
            self->in_browse_ = true;
        }
        if (status == ERROR_SUCCESS) {
            for (PDNS_RECORD r = records; r; r = r->pNext) {
                if (r->wType != DNS_TYPE_PTR || !r->Data.PTR.pNameHost) continue;
                const std::wstring instance = r->Data.PTR.pNameHost;
                if (r->dwTtl == 0) self->Remove(instance);  // goodbye packet
                else self->ResolveInstance(instance);
            }
        }
        if (records) DnsRecordListFree(records, DnsFreeRecordList);
        std::lock_guard lock(self->mu_);
        self->in_browse_ = false;
    }

    void Remove(const std::wstring& instance) {
        std::lock_guard lock(mu_);
        found_.erase(instance);
    }

    void ResolveInstance(const std::wstring& instance) {
        std::lock_guard lock(mu_);
        if (stopping_ || resolving_.count(instance)) return;
        auto r = std::make_unique<Resolve>();
        r->self = this;
        r->instance = instance;
        DNS_SERVICE_RESOLVE_REQUEST req{};
        req.Version = DNS_QUERY_REQUEST_VERSION1;
        req.InterfaceIndex = 0;
        req.QueryName = const_cast<PWSTR>(r->instance.c_str());
        req.pResolveCompletionCallback = &WinDiscovery::OnResolve;
        req.pQueryContext = r.get();
        if (DnsServiceResolve(&req, &r->cancel) == DNS_REQUEST_PENDING) resolving_[instance] = std::move(r);
    }

    static VOID WINAPI OnResolve(DWORD status, PVOID ctx, PDNS_SERVICE_INSTANCE inst) {
        auto* r = static_cast<Resolve*>(ctx);
        WinDiscovery* self = r->self;
        Discovered d;
        if (status == ERROR_SUCCESS && inst) {
            d.name = Narrow(inst->pszInstanceName);
            d.port = inst->wPort;
            char addr[INET6_ADDRSTRLEN] = {};
            if (inst->ip4Address) {
                in_addr a{};
                a.s_addr = *inst->ip4Address;
                inet_ntop(AF_INET, &a, addr, sizeof(addr));
            } else if (inst->ip6Address) {
                inet_ntop(AF_INET6, inst->ip6Address->IP6Byte, addr, sizeof(addr));
            }
            d.host = addr;
            for (DWORD i = 0; i < inst->dwPropertyCount; ++i)
                if (inst->keys[i] && inst->values[i] && _wcsicmp(inst->keys[i], L"id") == 0) d.id_hex = Narrow(inst->values[i]);
        }
        if (inst) DnsServiceFreeInstance(inst);
        std::lock_guard lock(self->mu_);
        if (!d.host.empty() && !d.id_hex.empty()) {
            auto& slot = self->found_[r->instance];
            if (slot.host != d.host || slot.port != d.port) log::Info("Bonjour: {} at {}:{}", d.name, d.host, d.port);
            slot = std::move(d);
        }
        const std::wstring instance = r->instance;
        self->resolving_.erase(instance);  // frees r
    }

    mutable std::mutex mu_;
    DNS_SERVICE_CANCEL browse_cancel_{};
    bool browsing_ = false;
    bool in_browse_ = false;
    bool stopping_ = false;
    std::map<std::wstring, Discovered> found_;
    std::map<std::wstring, std::unique_ptr<Resolve>> resolving_;
};

}  // namespace

std::unique_ptr<Discovery> StartDiscovery() {
    auto d = std::make_unique<WinDiscovery>();
    if (!d->Start()) return nullptr;
    return d;
}

}  // namespace tethercam::wifi
