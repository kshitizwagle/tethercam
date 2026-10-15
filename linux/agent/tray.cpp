#include "tray.h"

#include <glib-unix.h>
#include <gtk/gtk.h>
#include <libayatana-appindicator/app-indicator.h>

#include <csignal>
#include <format>

#include "tethercam/version.h"

namespace tethercam::agent {
namespace {

const char* StatusText(AgentStatus s) {
    switch (s) {
    case AgentStatus::NoDevice: return "Waiting for iPhone (USB)";
    case AgentStatus::AppNotOpen: return "Open Tethercam on your iPhone";
    case AgentStatus::Connected: return "Connected";
    case AgentStatus::Streaming: return "Streaming";
    case AgentStatus::PausedOnPhone: return "Paused on iPhone";
    case AgentStatus::NoUsbmuxd: return "usbmuxd is not installed";
    default: return "Starting";
    }
}

// Themed icon names present in Adwaita and Breeze. Themes without them (and trays that can't resolve
// them) get the app's own icon, which the package installs into hicolor.
const char* IconFor(AgentStatus s) {
    const char* name = (s == AgentStatus::Streaming || s == AgentStatus::Connected) ? "camera-web"
                                                                                     : "camera-web-symbolic";
    GtkIconTheme* theme = gtk_icon_theme_get_default();
    return theme && gtk_icon_theme_has_icon(theme, name) ? name : "tethercam";
}

struct Ctx {
    Tray* tray;
    Engine* engine;
    bool* building;
};

GtkWidget* Item(GtkWidget* menu, const std::string& label, bool sensitive = true) {
    GtkWidget* item = gtk_menu_item_new_with_label(label.c_str());
    gtk_widget_set_sensitive(item, sensitive);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    return item;
}

template <class F> GtkWidget* Check(GtkWidget* menu, const std::string& label, bool on, bool* building, F on_toggle) {
    GtkWidget* item = gtk_check_menu_item_new_with_label(label.c_str());
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item), on);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    auto* fn = new F(std::move(on_toggle));
    struct Data { F* fn; bool* building; };
    auto* data = new Data{fn, building};
    g_signal_connect_data(
        item, "toggled",
        G_CALLBACK(+[](GtkCheckMenuItem* w, gpointer p) {
            auto* d = static_cast<Data*>(p);
            if (!*d->building) (*d->fn)(gtk_check_menu_item_get_active(w));
        }),
        data, +[](gpointer p, GClosure*) {
            auto* d = static_cast<Data*>(p);
            delete d->fn;
            delete d;
        }, GConnectFlags(0));
    return item;
}

template <class F> void OnActivate(GtkWidget* item, F fn) {
    auto* f = new F(std::move(fn));
    g_signal_connect_data(
        item, "activate", G_CALLBACK(+[](GtkMenuItem*, gpointer p) { (*static_cast<F*>(p))(); }), f,
        +[](gpointer p, GClosure*) { delete static_cast<F*>(p); }, GConnectFlags(0));
}

GtkWidget* Submenu(GtkWidget* menu, const std::string& label) {
    GtkWidget* item = Item(menu, label);
    GtkWidget* sub = gtk_menu_new();
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(item), sub);
    return sub;
}

}  // namespace

bool Tray::Available(int* argc, char*** argv) { return gtk_init_check(argc, argv); }

void Tray::Create() {
    // app_indicator_new is marked deprecated in favour of the newer glib-only library, which distros
    // don't ship yet.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    auto* ind = app_indicator_new("tethercam", IconFor(AgentStatus::None), APP_INDICATOR_CATEGORY_HARDWARE);
    app_indicator_set_status(ind, APP_INDICATOR_STATUS_ACTIVE);
    app_indicator_set_title(ind, "Tethercam");
#pragma GCC diagnostic pop
    indicator_ = ind;
    Rebuild();
}

void Tray::Refresh() { g_idle_add(&Tray::OnIdle, this); }

int Tray::OnIdle(void* self) {
    static_cast<Tray*>(self)->Rebuild();
    return G_SOURCE_REMOVE;
}

void Tray::Rebuild() {
    const Snapshot s = engine_.Get();
    const std::string device = engine_.CameraDevice();
    // Skip identical rebuilds (status JSON covers everything the menu shows, plus the device path).
    const std::string key = agent::StatusJson(s) + device + std::to_string(s.cameras.size());
    if (key == last_key_) return;
    last_key_ = key;

    auto* ind = static_cast<AppIndicator*>(indicator_);
    app_indicator_set_icon_full(ind, IconFor(s.status), StatusText(s.status));
    building_ = true;
    GtkWidget* menu = gtk_menu_new();

    std::string status = StatusText(s.status);
    if (!s.device_name.empty()) status += ": " + s.device_name;
    Item(menu, status, false);
    if (!s.detail.empty()) Item(menu, s.detail, false);
    Item(menu, device.empty() ? "No virtual camera (load v4l2loopback)" : "Camera device: " + device, false);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

    const proto::Camera* selected = nullptr;
    for (const auto& c : s.cameras) {
        if (c.camera_id == s.selected_camera) selected = &c;
        const int id = c.camera_id;
        Check(menu, c.label, c.camera_id == s.selected_camera, &building_, [this, id](bool on) {
            if (on) engine_.SelectCamera(id);
            else Refresh();  // a radio-style item can't be switched off
        });
    }
    if (!s.cameras.empty()) gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

    GtkWidget* quality = Submenu(menu, "Capture quality");
    for (int i = 0; i <= int(proto::Quality::UHD60); ++i) {
        const auto q = proto::Quality(i);
        const bool supported = q == proto::Quality::Auto || !selected || SupportsQuality(selected->formats, q);
        GtkWidget* item = Check(quality, i == 0 ? "Auto (1080p30)" : QualityLabel(q), s.quality == q, &building_,
                                [this, q](bool on) {
                                    if (on) engine_.SetQuality(q);
                                    else Refresh();
                                });
        gtk_widget_set_sensitive(item, supported);
    }
    Check(menu, "Mirror", s.mirror, &building_, [this](bool on) { engine_.SetMirror(on); });
    GtkWidget* rotate = Submenu(menu, "Rotate");
    for (uint16_t deg : {0, 90, 180, 270}) {
        Check(rotate, std::format("{}°", deg), s.rotation == deg, &building_, [this, deg](bool on) {
            if (on) engine_.SetRotation(deg);
            else Refresh();
        });
    }
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    OnActivate(Item(menu, "Open log folder"), [this] {
        const std::string uri = "file://" + log_dir_.string();
        g_app_info_launch_default_for_uri(uri.c_str(), nullptr, nullptr);
    });
    Item(menu, std::format("Tethercam {} (GPL-3.0)", Version()), false);
    OnActivate(Item(menu, "Quit"), [] { gtk_main_quit(); });

    gtk_widget_show_all(menu);
    app_indicator_set_menu(ind, GTK_MENU(menu));
    building_ = false;
}

void Tray::Run() {
    auto quit = +[](gpointer) -> gboolean {
        gtk_main_quit();
        return G_SOURCE_REMOVE;
    };
    g_unix_signal_add(SIGINT, quit, nullptr);
    g_unix_signal_add(SIGTERM, quit, nullptr);
    gtk_main();
}

}  // namespace tethercam::agent
