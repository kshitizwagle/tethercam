#include "tethercam/shm.h"

#include <sddl.h>

#include <string>

namespace tethercam::shm {
namespace {

std::wstring Name(const wchar_t* base) {
    wchar_t ns[16] = {};
    const DWORD n = GetEnvironmentVariableW(L"TETHERCAM_SHM_NAMESPACE", ns, 16);
    const bool local = n > 0 && n < 16 && _wcsicmp(ns, L"Local") == 0;
    return std::wstring(local ? L"Local\\" : L"Global\\") + base;
}

Header* H(void* base) { return static_cast<Header*>(base); }
const Header* H(const void* base) { return static_cast<const Header*>(base); }

SlotHeader* Slot(void* base, uint32_t i) {
    return reinterpret_cast<SlotHeader*>(static_cast<uint8_t*>(base) + H(base)->slot_offset[i]);
}
const SlotHeader* Slot(const void* base, uint32_t i) {
    return reinterpret_cast<const SlotHeader*>(static_cast<const uint8_t*>(base) + H(base)->slot_offset[i]);
}

void CopyPlane(uint8_t* dst, uint32_t dst_pitch, const uint8_t* src, uint32_t src_pitch, uint32_t row_bytes,
               uint32_t rows) {
    if (dst_pitch == src_pitch && src_pitch == row_bytes) {
        std::memcpy(dst, src, size_t(row_bytes) * rows);
        return;
    }
    for (uint32_t r = 0; r < rows; ++r) std::memcpy(dst + size_t(r) * dst_pitch, src + size_t(r) * src_pitch, row_bytes);
}

}  // namespace

void InitHeader(void* base) {
    auto* h = H(base);
    std::memset(h, 0, kHeaderBytes);
    h->magic = kMagic;
    h->version = kVersion;
    h->header_size = kHeaderBytes;
    h->slot_count = kSlotCount;
    for (uint32_t i = 0; i < kSlotCount; ++i) h->slot_offset[i] = kHeaderBytes + kSlotCapacity * i;
    h->slot_capacity = kSlotCapacity;
    h->latest_slot = kNoSlot;
}

bool ValidateHeader(const void* base, uint64_t size) {
    const auto* h = H(base);
    if (size < kHeaderBytes || h->magic != kMagic || h->version != kVersion || h->slot_count != kSlotCount)
        return false;
    for (uint32_t i = 0; i < kSlotCount; ++i)
        if (h->slot_offset[i] < kHeaderBytes || h->slot_offset[i] + h->slot_capacity > size) return false;
    return h->slot_capacity >= kSlotHeaderBytes;
}

bool WriteFrame(void* base, uint32_t width, uint32_t height, const uint8_t* y, uint32_t y_pitch, const uint8_t* uv,
                uint32_t uv_pitch, uint64_t pts_100ns) {
    auto* h = H(base);
    if (width == 0 || height == 0 || width > kMaxWidth || height > kMaxHeight || (width & 1) || (height & 1))
        return false;
    const uint64_t bytes = uint64_t(width) * height * 3 / 2;
    if (kSlotHeaderBytes + bytes > h->slot_capacity) return false;

    const uint32_t latest = Atomic(h->latest_slot).load(std::memory_order_acquire);
    const uint32_t i = latest == kNoSlot ? 0 : (latest + 1) % kSlotCount;
    auto* s = Slot(base, i);
    auto seq = Atomic(s->seq);
    const uint32_t start = seq.load(std::memory_order_relaxed) | 1;  // odd: writing
    seq.store(start, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);

    s->width = width;
    s->height = height;
    s->stride = width;
    s->pts_100ns = pts_100ns;
    // A global frame sequence: consumers use it to tell "a new frame arrived" exactly. The
    // previous write's number is visible here (its release store happened before latest_slot).
    s->frame_number = latest == kNoSlot ? 1 : Slot(base, latest)->frame_number + 1;
    uint8_t* data = reinterpret_cast<uint8_t*>(s) + kSlotHeaderBytes;
    CopyPlane(data, width, y, y_pitch, width, height);
    CopyPlane(data + size_t(width) * height, width, uv, uv_pitch, width, height / 2);

    seq.store(start + 1, std::memory_order_release);  // even: complete
    Atomic(h->latest_slot).store(i, std::memory_order_release);
    return true;
}

bool ReadLatest(const void* base, uint32_t width, uint32_t height, uint8_t* dst, uint32_t dst_pitch,
                FrameInfo& info) {
    auto* h = const_cast<Header*>(H(base));
    for (int attempt = 0; attempt < 2; ++attempt) {
        const uint32_t i = Atomic(h->latest_slot).load(std::memory_order_acquire);
        if (i >= kSlotCount) return false;
        auto* s = const_cast<SlotHeader*>(Slot(base, i));
        auto seq = Atomic(s->seq);
        const uint32_t before = seq.load(std::memory_order_acquire);
        if (before & 1) continue;
        const uint32_t w = s->width, hgt = s->height, stride = s->stride;
        if (w != width || hgt != height || stride != w) return false;
        if (kSlotHeaderBytes + uint64_t(w) * hgt * 3 / 2 > h->slot_capacity) return false;
        info = {w, hgt, s->pts_100ns, s->frame_number};
        const uint8_t* data = reinterpret_cast<const uint8_t*>(s) + kSlotHeaderBytes;
        CopyPlane(dst, dst_pitch, data, stride, w, hgt);
        CopyPlane(dst + size_t(dst_pitch) * hgt, dst_pitch, data + size_t(stride) * hgt, stride, w, hgt / 2);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (seq.load(std::memory_order_relaxed) == before) return true;
    }
    return false;
}

bool PeekLatest(const void* base, FrameInfo& info) {
    auto* h = const_cast<Header*>(H(base));
    const uint32_t i = Atomic(h->latest_slot).load(std::memory_order_acquire);
    if (i >= kSlotCount) return false;
    auto* s = const_cast<SlotHeader*>(Slot(base, i));
    auto seq = Atomic(s->seq);
    const uint32_t before = seq.load(std::memory_order_acquire);
    if (before & 1) return false;
    FrameInfo tmp{s->width, s->height, s->pts_100ns, s->frame_number};
    std::atomic_thread_fence(std::memory_order_acquire);
    if (seq.load(std::memory_order_relaxed) != before) return false;
    info = tmp;
    return true;
}

void Objects::Close() {
    if (view) UnmapViewOfFile(view);
    if (mapping) CloseHandle(mapping);
    if (frame_ready) CloseHandle(frame_ready);
    if (control) CloseHandle(control);
    view = mapping = frame_ready = control = nullptr;
}

bool Create(Objects& out) {
    out.Close();
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(kSddl, SDDL_REVISION_1, &sd, nullptr)) return false;
    SECURITY_ATTRIBUTES sa{sizeof(sa), sd, FALSE};
    out.mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, DWORD(kMappingBytes >> 32),
                                     DWORD(kMappingBytes & 0xFFFFFFFF), Name(kMappingName).c_str());
    const bool existed = GetLastError() == ERROR_ALREADY_EXISTS;
    out.frame_ready = CreateEventW(&sa, FALSE, FALSE, Name(kFrameReadyName).c_str());
    out.control = CreateEventW(&sa, FALSE, FALSE, Name(kControlName).c_str());
    LocalFree(sd);
    if (!out.mapping || !out.frame_ready || !out.control) {
        out.Close();
        return false;
    }
    out.view = MapViewOfFile(out.mapping, FILE_MAP_ALL_ACCESS, 0, 0, kMappingBytes);
    if (!out.view) {
        out.Close();
        return false;
    }
    if (!existed || !ValidateHeader(out.view, kMappingBytes)) InitHeader(out.view);
    return true;
}

bool Open(Objects& out) {
    out.Close();
    auto fail = [&](DWORD err) {
        out.Close();
        SetLastError(err);
        return false;
    };
    out.mapping = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, Name(kMappingName).c_str());
    if (!out.mapping) return fail(GetLastError());
    out.frame_ready = OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, Name(kFrameReadyName).c_str());
    if (!out.frame_ready) return fail(GetLastError());
    out.control = OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, Name(kControlName).c_str());
    if (!out.control) return fail(GetLastError());
    out.view = MapViewOfFile(out.mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, kMappingBytes);
    if (!out.view) return fail(GetLastError());
    if (!ValidateHeader(out.view, kMappingBytes)) return fail(ERROR_INVALID_DATA);
    return true;
}

}  // namespace tethercam::shm
