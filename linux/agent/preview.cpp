#include "preview.h"

#include <cairo.h>
#include <gtk/gtk.h>

#include <algorithm>
#include <format>
#include <string>
#include <vector>

#include "tethercam/image.h"
#include "tray.h"

namespace tethercam::agent {

namespace {

constexpr guint kRefreshMs = 33;                   // ~30 Hz
constexpr gint64 kStaleUs = 1500 * G_TIME_SPAN_MILLISECOND;  // no new frame for this long: show the status

}  // namespace

struct Preview::Impl {
    Engine& engine;
    GtkWidget* window = nullptr;
    GtkWidget* area = nullptr;
    GtkWidget* lens_box = nullptr;
    GtkWidget* mirror = nullptr;
    GtkWidget* rotate = nullptr;
    GtkWidget* quality = nullptr;
    guint timer = 0;
    bool building = false;  // true while we change widgets ourselves, so their signals are ignored
    std::string lens_key;

    image::Nv12Frame nv12;
    std::vector<uint8_t> bgra;
    cairo_surface_t* surface = nullptr;
    uint64_t seq = 0;
    gint64 last_frame_us = 0;
    uint32_t rotation = 0;

    explicit Impl(Engine& e) : engine(e) {}
    ~Impl() { Reset(); }

    void Reset() {
        if (surface) cairo_surface_destroy(surface);
        surface = nullptr;
        seq = 0;
        last_frame_us = 0;
    }

    void Build();
    void Sync();
    void Hide();
    // Pulls the newest frame into the cairo surface. True when there was one.
    bool Pull();
    void Draw(cairo_t* cr, int w, int h);
};

void Preview::Impl::Build() {
    window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "Tethercam");
    gtk_window_set_icon_name(GTK_WINDOW(window), "tethercam");
    gtk_window_set_default_size(GTK_WINDOW(window), 960, 600);
    g_signal_connect(window, "delete-event", G_CALLBACK(+[](GtkWidget*, GdkEvent*, gpointer p) -> gboolean {
        static_cast<Impl*>(p)->Hide();
        return TRUE;  // hide, don't destroy
    }), this);

    GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(window), vbox);

    GtkWidget* bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(bar), 6);
    gtk_box_pack_start(GTK_BOX(vbox), bar, FALSE, FALSE, 0);

    lens_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(lens_box), "linked");
    gtk_box_pack_start(GTK_BOX(bar), lens_box, FALSE, FALSE, 0);

    mirror = gtk_toggle_button_new_with_label("Mirror");
    g_signal_connect(mirror, "toggled", G_CALLBACK(+[](GtkToggleButton* b, gpointer p) {
        auto* self = static_cast<Impl*>(p);
        if (!self->building) self->engine.SetMirror(gtk_toggle_button_get_active(b));
    }), this);
    gtk_box_pack_start(GTK_BOX(bar), mirror, FALSE, FALSE, 0);

    rotate = gtk_button_new_with_label("Rotate 0°");
    g_signal_connect(rotate, "clicked", G_CALLBACK(+[](GtkButton*, gpointer p) {
        auto* self = static_cast<Impl*>(p);
        self->engine.SetRotation(uint16_t((self->rotation + 90) % 360));
    }), this);
    gtk_box_pack_start(GTK_BOX(bar), rotate, FALSE, FALSE, 0);

    quality = gtk_combo_box_text_new();
    for (int i = 0; i <= int(proto::Quality::UHD60); ++i)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(quality),
                                       i == 0 ? "Auto (1080p30)" : QualityLabel(proto::Quality(i)));
    g_signal_connect(quality, "changed", G_CALLBACK(+[](GtkComboBox* c, gpointer p) {
        auto* self = static_cast<Impl*>(p);
        const int i = gtk_combo_box_get_active(c);
        if (self->building || i < 0) return;
        self->engine.SetQuality(proto::Quality(i));
    }), this);
    gtk_box_pack_end(GTK_BOX(bar), quality, FALSE, FALSE, 0);

    area = gtk_drawing_area_new();
    gtk_widget_set_hexpand(area, TRUE);
    gtk_widget_set_vexpand(area, TRUE);
    g_signal_connect(area, "draw", G_CALLBACK(+[](GtkWidget* w, cairo_t* cr, gpointer p) -> gboolean {
        static_cast<Impl*>(p)->Draw(cr, gtk_widget_get_allocated_width(w), gtk_widget_get_allocated_height(w));
        return FALSE;
    }), this);
    gtk_box_pack_start(GTK_BOX(vbox), area, TRUE, TRUE, 0);

    gtk_widget_show_all(vbox);
}

void Preview::Impl::Sync() {
    if (!window) return;
    const Snapshot s = engine.Get();
    building = true;

    // Lens buttons: rebuilt only when the list of cameras changes.
    std::string key;
    for (const auto& c : s.cameras) key += std::format("{}:{};", c.camera_id, c.label);
    if (key != lens_key) {
        lens_key = key;
        gtk_container_foreach(GTK_CONTAINER(lens_box), +[](GtkWidget* w, gpointer) { gtk_widget_destroy(w); }, nullptr);
        for (const auto& c : s.cameras) {
            GtkWidget* b = gtk_toggle_button_new_with_label(c.label.c_str());
            g_object_set_data(G_OBJECT(b), "camera", GINT_TO_POINTER(c.camera_id));
            g_signal_connect(b, "toggled", G_CALLBACK(+[](GtkToggleButton* w, gpointer p) {
                auto* self = static_cast<Impl*>(p);
                if (self->building) return;
                if (gtk_toggle_button_get_active(w))
                    self->engine.SelectCamera(GPOINTER_TO_INT(g_object_get_data(G_OBJECT(w), "camera")));
                else self->Sync();  // a lens button can't be switched off
            }), this);
            gtk_box_pack_start(GTK_BOX(lens_box), b, FALSE, FALSE, 0);
            gtk_widget_show(b);
        }
    }
    struct Sel { int id; };
    Sel sel{s.selected_camera};
    gtk_container_foreach(GTK_CONTAINER(lens_box), +[](GtkWidget* w, gpointer p) {
        const bool on = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(w), "camera")) == static_cast<Sel*>(p)->id;
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w), on);
    }, &sel);

    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(mirror), s.mirror);
    rotation = s.rotation;
    gtk_button_set_label(GTK_BUTTON(rotate), std::format("Rotate {}°", s.rotation).c_str());
    gtk_combo_box_set_active(GTK_COMBO_BOX(quality), int(s.quality));
    building = false;
    gtk_widget_queue_draw(area);  // the placeholder text follows the status
}

void Preview::Impl::Hide() {
    engine.SetPreview(false);
    if (timer) g_source_remove(timer);
    timer = 0;
    Reset();
    gtk_widget_hide(window);
}

bool Preview::Impl::Pull() {
    if (!engine.CopyPreview(nv12, seq)) return false;
    const int stride = cairo_format_stride_for_width(CAIRO_FORMAT_RGB24, int(nv12.width));
    bgra.resize(size_t(stride) * nv12.height);
    image::Nv12ToBgra(nv12.View(), bgra.data(), uint32_t(stride));
    // CAIRO_FORMAT_RGB24 is 0xXXRRGGBB in native order: BGRx bytes on little endian.
    if (surface) cairo_surface_destroy(surface);
    surface = cairo_image_surface_create_for_data(bgra.data(), CAIRO_FORMAT_RGB24, int(nv12.width),
                                                  int(nv12.height), stride);
    last_frame_us = g_get_monotonic_time();
    return true;
}

void Preview::Impl::Draw(cairo_t* cr, int w, int h) {
    cairo_set_source_rgb(cr, 0, 0, 0);
    cairo_paint(cr);
    const bool live = surface && g_get_monotonic_time() - last_frame_us < kStaleUs;
    if (live) {
        const double sw = cairo_image_surface_get_width(surface), sh = cairo_image_surface_get_height(surface);
        const double scale = std::min(w / sw, h / sh);
        cairo_translate(cr, (w - sw * scale) / 2, (h - sh * scale) / 2);
        cairo_scale(cr, scale, scale);
        cairo_set_source_surface(cr, surface, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
        cairo_paint(cr);
        return;
    }
    const Snapshot s = engine.Get();
    std::string text = s.status == AgentStatus::Streaming ? "Waiting for video" : StatusText(s.status);
    if (s.status == AgentStatus::NoDevice) text = "Waiting for iPhone";
    cairo_select_font_face(cr, "sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 22);
    cairo_set_source_rgb(cr, 0.8, 0.8, 0.8);
    cairo_text_extents_t ext;
    cairo_text_extents(cr, text.c_str(), &ext);
    cairo_move_to(cr, (w - ext.width) / 2 - ext.x_bearing, h / 2.0);
    cairo_show_text(cr, text.c_str());
}

Preview::Preview(Engine& engine) : impl_(std::make_unique<Impl>(engine)) {}
Preview::~Preview() = default;

void Preview::Show() {
    Impl& d = *impl_;
    if (!d.window) d.Build();
    d.engine.SetPreview(true);
    d.Sync();
    gtk_window_present(GTK_WINDOW(d.window));
    if (!d.timer) {
        d.timer = g_timeout_add(kRefreshMs, +[](gpointer p) -> gboolean {
            auto* self = static_cast<Impl*>(p);
            // Redraw on a new frame, and when the last one has just gone stale.
            const bool stale = self->surface && g_get_monotonic_time() - self->last_frame_us >= kStaleUs;
            if (self->Pull() || stale) {
                if (stale) { cairo_surface_destroy(self->surface); self->surface = nullptr; }
                gtk_widget_queue_draw(self->area);
            }
            return G_SOURCE_CONTINUE;
        }, &d);
    }
}

void Preview::Sync() { impl_->Sync(); }

}  // namespace tethercam::agent
