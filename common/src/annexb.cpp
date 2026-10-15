#include "tethercam/annexb.h"

namespace tethercam::annexb {
namespace {

constexpr uint8_t kStart[4] = {0, 0, 0, 1};

void Append(std::vector<uint8_t>& out, std::span<const uint8_t> nal) {
    out.insert(out.end(), kStart, kStart + 4);
    out.insert(out.end(), nal.begin(), nal.end());
}

}  // namespace

std::vector<std::span<const uint8_t>> SplitNals(std::span<const uint8_t> d) {
    std::vector<std::span<const uint8_t>> nals;
    const size_t n = d.size();
    size_t i = 0;
    size_t start = SIZE_MAX;
    while (i + 2 < n) {
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
            if (start != SIZE_MAX) {
                size_t end = i;
                while (end > start && d[end - 1] == 0) --end;  // trailing zeros belong to the next start code
                if (end > start) nals.push_back(d.subspan(start, end - start));
            }
            i += 3;
            start = i;
        } else {
            ++i;
        }
    }
    if (start != SIZE_MAX && start < n) nals.push_back(d.subspan(start));
    return nals;
}

bool HasIdr(std::span<const uint8_t> au) {
    for (auto nal : SplitNals(au))
        if (NalType(nal) == 5) return true;
    return false;
}

std::vector<uint8_t> ExtractParameterSets(std::span<const uint8_t> au) {
    std::vector<uint8_t> out;
    for (auto nal : SplitNals(au))
        if (NalType(nal) == 7 || NalType(nal) == 8) Append(out, nal);
    return out;
}

std::vector<uint8_t> StripParameterSets(std::span<const uint8_t> au) {
    std::vector<uint8_t> out;
    for (auto nal : SplitNals(au))
        if (NalType(nal) != 7 && NalType(nal) != 8) Append(out, nal);
    return out;
}

std::vector<uint8_t> AvccToAnnexB(std::span<const uint8_t> avcc) {
    std::vector<uint8_t> out;
    out.reserve(avcc.size() + 16);
    size_t i = 0;
    while (i + 4 <= avcc.size()) {
        const uint32_t len = uint32_t(avcc[i]) << 24 | uint32_t(avcc[i + 1]) << 16 | uint32_t(avcc[i + 2]) << 8 | avcc[i + 3];
        i += 4;
        if (len > avcc.size() - i) break;
        Append(out, avcc.subspan(i, len));
        i += len;
    }
    return out;
}

}  // namespace tethercam::annexb
