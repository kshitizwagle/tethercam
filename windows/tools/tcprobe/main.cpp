// tcprobe: connect to the phone (usbmuxd or --tcp), print stream stats, dump .h264. Skeleton only (M0).
#include <cstdio>

#include "tethercam/version.h"

int main() {
    std::printf("tcprobe %.*s\n", static_cast<int>(tethercam::Version().size()), tethercam::Version().data());
    return 0;
}
