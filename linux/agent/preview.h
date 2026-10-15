#pragma once

// Camera preview window (GTK 3): the live feed with lens, mirror, rotate and quality controls. Same idea
// as windows/agent/preview.h. While it's open the engine keeps the iPhone streaming. Every call must be
// made on the GTK thread.

#include <memory>

#include "engine.h"

namespace tethercam::agent {

class Preview {
public:
    explicit Preview(Engine& engine);
    ~Preview();

    // Creates the window on first use, shows it and starts the engine's preview feed.
    void Show();
    // Brings the toolbar in line with the engine's settings. Cheap; call whenever the status changes.
    void Sync();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace tethercam::agent
