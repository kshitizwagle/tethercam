// Bonjour discovery on Linux through avahi-daemon (libavahi-client). Without Avahi at build time
// (TETHERCAM_AVAHI undefined) discovery is off and the agent relies on the address the phone reported
// over USB or the one the user entered.

#include "tethercam/discovery.h"

#ifdef TETHERCAM_AVAHI
#include <avahi-client/client.h>
#include <avahi-client/lookup.h>
#include <avahi-common/address.h>
#include <avahi-common/malloc.h>
#include <avahi-common/strlst.h>
#include <avahi-common/thread-watch.h>

#include <map>
#include <mutex>

#include "tethercam/log.h"
#endif

namespace tethercam::wifi {

#ifdef TETHERCAM_AVAHI
namespace {

class AvahiDiscovery final : public Discovery {
public:
    bool Start() {
        poll_ = avahi_threaded_poll_new();
        if (!poll_) return false;
        int err = 0;
        // NO_FAIL: keeps working across avahi-daemon restarts (and starts before the daemon is up).
        client_ = avahi_client_new(avahi_threaded_poll_get(poll_), AVAHI_CLIENT_NO_FAIL, &OnClient, this, &err);
        if (!client_) {
            log::Warn("Bonjour discovery unavailable (Avahi error {})", err);
            return false;
        }
        return avahi_threaded_poll_start(poll_) == 0;
    }

    ~AvahiDiscovery() override {
        if (poll_) avahi_threaded_poll_stop(poll_);
        if (client_) avahi_client_free(client_);  // frees the browser and any running resolvers
        if (poll_) avahi_threaded_poll_free(poll_);
    }

    std::vector<Discovered> Found() const override {
        std::lock_guard lock(mu_);
        std::vector<Discovered> out;
        for (const auto& [name, d] : found_) out.push_back(d);
        return out;
    }

private:
    // Callbacks run on Avahi's poll thread (or inside avahi_client_new).
    static void OnClient(AvahiClient* c, AvahiClientState state, void* self_ptr) {
        auto* self = static_cast<AvahiDiscovery*>(self_ptr);
        if (state == AVAHI_CLIENT_S_RUNNING && !self->browser_) {
            self->browser_ = avahi_service_browser_new(c, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC, kServiceType, nullptr,
                                                       AvahiLookupFlags(0), &OnBrowse, self);
        } else if (state == AVAHI_CLIENT_FAILURE || state == AVAHI_CLIENT_CONNECTING) {
            if (self->browser_) avahi_service_browser_free(self->browser_);
            self->browser_ = nullptr;
            std::lock_guard lock(self->mu_);
            self->found_.clear();
        }
    }

    static void OnBrowse(AvahiServiceBrowser* b, AvahiIfIndex iface, AvahiProtocol proto, AvahiBrowserEvent event,
                         const char* name, const char* type, const char* domain, AvahiLookupResultFlags,
                         void* self_ptr) {
        auto* self = static_cast<AvahiDiscovery*>(self_ptr);
        if (event == AVAHI_BROWSER_NEW) {
            // IPv4 addresses: a link-local IPv6 address would also need the interface scope.
            avahi_service_resolver_new(avahi_service_browser_get_client(b), iface, proto, name, type, domain,
                                       AVAHI_PROTO_INET, AvahiLookupFlags(0), &OnResolve, self);
        } else if (event == AVAHI_BROWSER_REMOVE) {
            std::lock_guard lock(self->mu_);
            self->found_.erase(name);
        }
    }

    static void OnResolve(AvahiServiceResolver* r, AvahiIfIndex, AvahiProtocol, AvahiResolverEvent event,
                          const char* name, const char*, const char*, const char*, const AvahiAddress* address,
                          uint16_t port, AvahiStringList* txt, AvahiLookupResultFlags, void* self_ptr) {
        auto* self = static_cast<AvahiDiscovery*>(self_ptr);
        if (event == AVAHI_RESOLVER_FOUND && address) {
            Discovered d;
            d.name = name;
            d.port = port;
            char buf[AVAHI_ADDRESS_STR_MAX] = {};
            avahi_address_snprint(buf, sizeof(buf), address);
            d.host = buf;
            if (AvahiStringList* id = avahi_string_list_find(txt, "id")) {
                char* key = nullptr;
                char* value = nullptr;
                if (avahi_string_list_get_pair(id, &key, &value, nullptr) == 0 && value) d.id_hex = value;
                avahi_free(key);
                avahi_free(value);
            }
            if (!d.host.empty() && !d.id_hex.empty()) {
                std::lock_guard lock(self->mu_);
                auto& slot = self->found_[name];
                if (slot.host != d.host || slot.port != d.port) log::Info("Bonjour: {} at {}:{}", d.name, d.host, d.port);
                slot = std::move(d);
            }
        }
        avahi_service_resolver_free(r);
    }

    AvahiThreadedPoll* poll_ = nullptr;
    AvahiClient* client_ = nullptr;
    AvahiServiceBrowser* browser_ = nullptr;
    mutable std::mutex mu_;
    std::map<std::string, Discovered> found_;
};

}  // namespace

std::unique_ptr<Discovery> StartDiscovery() {
    auto d = std::make_unique<AvahiDiscovery>();
    if (!d->Start()) return nullptr;
    return d;
}
#else
std::unique_ptr<Discovery> StartDiscovery() { return nullptr; }
#endif

}  // namespace tethercam::wifi
