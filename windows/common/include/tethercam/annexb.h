#pragma once

// H.264 Annex-B helpers (start-code delimited NAL units).

#include <cstdint>
#include <span>
#include <vector>

namespace tethercam::annexb {

// Splits an Annex-B buffer into NAL units (without start codes).
std::vector<std::span<const uint8_t>> SplitNals(std::span<const uint8_t> data);

// H.264 nal_unit_type of a NAL (without start code). 5 = IDR, 7 = SPS, 8 = PPS.
inline uint8_t NalType(std::span<const uint8_t> nal) { return nal.empty() ? 0 : nal[0] & 0x1F; }

// True if the access unit contains an IDR slice.
bool HasIdr(std::span<const uint8_t> au);

// Collects SPS and PPS NALs (with 4-byte start codes). Empty if none.
std::vector<uint8_t> ExtractParameterSets(std::span<const uint8_t> au);

// Removes SPS/PPS NALs, keeping everything else (with 4-byte start codes).
std::vector<uint8_t> StripParameterSets(std::span<const uint8_t> au);

// Converts AVCC (4-byte big-endian length prefixes) to Annex-B.
std::vector<uint8_t> AvccToAnnexB(std::span<const uint8_t> avcc);

}  // namespace tethercam::annexb
