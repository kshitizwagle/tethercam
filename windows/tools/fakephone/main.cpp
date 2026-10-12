// fakephone: act as a Tethercam device over plain TCP, streaming an .h264 file. Skeleton only (M0).
#include <cstdio>

#include "tethercam/version.h"

int main() {
    std::printf("fakephone %.*s\n", static_cast<int>(tethercam::Version().size()), tethercam::Version().data());
    return 0;
}
