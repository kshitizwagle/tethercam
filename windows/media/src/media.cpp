#include "tethercam/media.h"

#include <codecapi.h>
#include <d3d11.h>
#include <d3d10.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wmcodecdsp.h>
#include <wil/com.h>
#include <wil/result.h>

#include <cstring>

#include "tethercam/annexb.h"
#include "tethercam/log.h"

namespace tethercam::media {
namespace {

wil::com_ptr<IMFSample> MakeSample(std::span<const uint8_t> data, int64_t pts, int64_t duration) {
    wil::com_ptr<IMFMediaBuffer> buf;
    if (FAILED(MFCreateMemoryBuffer(static_cast<DWORD>(data.size()), &buf))) return nullptr;
    BYTE* p = nullptr;
    if (FAILED(buf->Lock(&p, nullptr, nullptr))) return nullptr;
    std::memcpy(p, data.data(), data.size());
    buf->Unlock();
    buf->SetCurrentLength(static_cast<DWORD>(data.size()));
    wil::com_ptr<IMFSample> s;
    if (FAILED(MFCreateSample(&s))) return nullptr;
    s->AddBuffer(buf.get());
    s->SetSampleTime(pts);
    s->SetSampleDuration(duration);
    return s;
}

void SetVariantU4(ICodecAPI* api, const GUID& key, ULONG value) {
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_UI4;
    v.ulVal = value;
    api->SetValue(&key, &v);
}

void SetVariantBool(ICodecAPI* api, const GUID& key, bool value) {
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_BOOL;
    v.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
    api->SetValue(&key, &v);
}

}  // namespace

MfScope::MfScope() {
    com_ = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    ok_ = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET));
}

MfScope::~MfScope() {
    if (ok_) MFShutdown();
    if (com_) CoUninitialize();
}

// ---------------------------------------------------------------------------------------------
// Decoder

struct H264Decoder::Impl {
    wil::com_ptr<IMFTransform> mft;
    wil::com_ptr<ID3D11Device> device;
    wil::com_ptr<IMFDXGIDeviceManager> manager;
    bool hardware = false;
    bool provides_samples = false;
    DWORD out_size = 0;
    uint32_t width = 0, height = 0, fps = 30;
    uint32_t coded_width = 0, coded_height = 0;
    uint32_t display_width = 0, display_height = 0;
    LONG default_stride = 0;

    bool SetupD3D() {
        UINT flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        static const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                                                   D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, ARRAYSIZE(levels),
                                     D3D11_SDK_VERSION, &device, nullptr, nullptr)))
            return false;
        if (auto mt = device.try_query<ID3D10Multithread>()) mt->SetMultithreadProtected(TRUE);
        UINT token = 0;
        if (FAILED(MFCreateDXGIDeviceManager(&token, &manager))) return false;
        if (FAILED(manager->ResetDevice(device.get(), token))) return false;
        wil::com_ptr<IMFAttributes> attrs;
        if (FAILED(mft->GetAttributes(&attrs))) return false;
        if (MFGetAttributeUINT32(attrs.get(), MF_SA_D3D11_AWARE, FALSE) == FALSE) return false;
        return SUCCEEDED(mft->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(manager.get())));
    }

    bool SetOutputNv12() {
        for (DWORD i = 0;; ++i) {
            wil::com_ptr<IMFMediaType> t;
            if (FAILED(mft->GetOutputAvailableType(0, i, &t))) return false;
            GUID sub{};
            t->GetGUID(MF_MT_SUBTYPE, &sub);
            if (sub != MFVideoFormat_NV12) continue;
            if (FAILED(mft->SetOutputType(0, t.get(), 0))) return false;
            MFGetAttributeSize(t.get(), MF_MT_FRAME_SIZE, &coded_width, &coded_height);
            MFVideoArea area{};
            if (SUCCEEDED(t->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8*>(&area), sizeof(area),
                                     nullptr)) &&
                area.Area.cx > 0 && area.Area.cy > 0) {
                display_width = static_cast<uint32_t>(area.Area.cx);
                display_height = static_cast<uint32_t>(area.Area.cy);
            } else {
                display_width = std::min(width, coded_width);
                display_height = std::min(height, coded_height);
            }
            display_width &= ~1u;
            display_height &= ~1u;
            default_stride = static_cast<LONG>(MFGetAttributeUINT32(t.get(), MF_MT_DEFAULT_STRIDE, coded_width));
            MFT_OUTPUT_STREAM_INFO info{};
            mft->GetOutputStreamInfo(0, &info);
            provides_samples =
                (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
            out_size = info.cbSize;
            return true;
        }
    }

    bool EmitSample(IMFSample* sample, const FrameCallback& cb) {
        LONGLONG pts = 0;
        sample->GetSampleTime(&pts);
        wil::com_ptr<IMFMediaBuffer> buf;
        if (FAILED(sample->GetBufferByIndex(0, &buf))) return false;
        if (auto b2d = buf.try_query<IMF2DBuffer>()) {
            BYTE* scan0 = nullptr;
            LONG pitch = 0;
            if (FAILED(b2d->Lock2D(&scan0, &pitch))) return false;
            image::Nv12View v{scan0, scan0 + size_t(pitch) * coded_height, uint32_t(pitch), uint32_t(pitch),
                              display_width, display_height};
            cb(v, pts);
            b2d->Unlock2D();
            return true;
        }
        BYTE* p = nullptr;
        DWORD len = 0;
        if (FAILED(buf->Lock(&p, nullptr, &len))) return false;
        const uint32_t pitch = static_cast<uint32_t>(std::abs(default_stride));
        if (size_t(pitch) * coded_height * 3 / 2 <= len) {
            image::Nv12View v{p, p + size_t(pitch) * coded_height, pitch, pitch, display_width, display_height};
            cb(v, pts);
        }
        buf->Unlock();
        return true;
    }

    // Pulls every available output frame. Returns false on a real error.
    bool Drain(const FrameCallback& cb) {
        for (;;) {
            MFT_OUTPUT_DATA_BUFFER out{};
            wil::com_ptr<IMFSample> own;
            if (!provides_samples) {
                wil::com_ptr<IMFMediaBuffer> buf;
                const DWORD size = out_size ? out_size : DWORD(coded_width * coded_height * 3 / 2);
                if (FAILED(MFCreateMemoryBuffer(size, &buf)) || FAILED(MFCreateSample(&own))) return false;
                own->AddBuffer(buf.get());
                out.pSample = own.get();
            }
            DWORD status = 0;
            const HRESULT hr = mft->ProcessOutput(0, 1, &out, &status);
            if (out.pEvents) out.pEvents->Release();
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return true;
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
                if (!SetOutputNv12()) return false;
                log::Info("decoder output: coded {}x{}, display {}x{}", coded_width, coded_height, display_width,
                          display_height);
                continue;
            }
            if (FAILED(hr)) {
                if (provides_samples && out.pSample) out.pSample->Release();
                return false;
            }
            if (out.pSample) {
                EmitSample(out.pSample, cb);
                if (provides_samples) out.pSample->Release();
            }
        }
    }
};

H264Decoder::H264Decoder() : impl_(std::make_unique<Impl>()) {}
H264Decoder::~H264Decoder() {
    if (impl_->mft) impl_->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
}

bool H264Decoder::IsHardware() const { return impl_->hardware; }

bool H264Decoder::Init(uint32_t width, uint32_t height, uint32_t fps, bool prefer_hardware) {
    auto& d = *impl_;
    d = Impl{};
    d.width = width;
    d.height = height;
    d.fps = fps ? fps : 30;
    if (FAILED(CoCreateInstance(CLSID_CMSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d.mft)))) {
        log::Error("H.264 decoder MFT not available");
        return false;
    }
    if (wil::com_ptr<IMFAttributes> attrs; SUCCEEDED(d.mft->GetAttributes(&attrs))) {
        attrs->SetUINT32(CODECAPI_AVLowLatencyMode, TRUE);
    }
    if (prefer_hardware) {
        d.hardware = d.SetupD3D();
        if (!d.hardware) {
            d.manager.reset();
            d.device.reset();
            log::Warn("hardware decoding unavailable, using software");
        }
    }
    wil::com_ptr<IMFMediaType> in;
    MFCreateMediaType(&in);
    in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    MFSetAttributeSize(in.get(), MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(in.get(), MF_MT_FRAME_RATE, d.fps, 1);
    in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (FAILED(d.mft->SetInputType(0, in.get(), 0))) {
        log::Error("decoder rejected H.264 input type {}x{}", width, height);
        return false;
    }
    if (!d.SetOutputNv12()) {
        log::Error("decoder has no NV12 output");
        return false;
    }
    d.mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    d.mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    log::Info("H.264 decoder ready: {}x{} @ {} ({})", width, height, d.fps, d.hardware ? "hardware" : "software");
    return true;
}

bool H264Decoder::Decode(std::span<const uint8_t> au, int64_t pts_100ns, const FrameCallback& cb) {
    auto& d = *impl_;
    if (!d.mft) return false;
    auto sample = MakeSample(au, pts_100ns, 10'000'000 / d.fps);
    if (!sample) return false;
    HRESULT hr = d.mft->ProcessInput(0, sample.get(), 0);
    if (hr == MF_E_NOTACCEPTING) {
        if (!d.Drain(cb)) return false;
        hr = d.mft->ProcessInput(0, sample.get(), 0);
    }
    if (FAILED(hr)) return false;
    return d.Drain(cb);
}

void H264Decoder::Flush() {
    if (impl_->mft) impl_->mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
}

// ---------------------------------------------------------------------------------------------
// Encoder (used by fakephone to simulate an iPhone)

struct H264Encoder::Impl {
    wil::com_ptr<IMFTransform> mft;
    wil::com_ptr<ICodecAPI> api;
    std::vector<uint8_t> sequence_header;
    uint32_t width = 0, height = 0, fps = 30;
    DWORD out_size = 0;

    void Collect(std::vector<std::vector<uint8_t>>& out) {
        for (;;) {
            wil::com_ptr<IMFMediaBuffer> buf;
            wil::com_ptr<IMFSample> s;
            MFCreateMemoryBuffer(out_size ? out_size : width * height * 2, &buf);
            MFCreateSample(&s);
            s->AddBuffer(buf.get());
            MFT_OUTPUT_DATA_BUFFER ob{};
            ob.pSample = s.get();
            DWORD status = 0;
            const HRESULT hr = mft->ProcessOutput(0, 1, &ob, &status);
            if (ob.pEvents) ob.pEvents->Release();
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
                wil::com_ptr<IMFMediaType> t;
                if (SUCCEEDED(mft->GetOutputAvailableType(0, 0, &t))) mft->SetOutputType(0, t.get(), 0);
                continue;
            }
            if (FAILED(hr)) return;
            BYTE* p = nullptr;
            DWORD len = 0;
            if (FAILED(buf->Lock(&p, nullptr, &len))) return;
            std::vector<uint8_t> au(p, p + len);
            buf->Unlock();
            const bool key = MFGetAttributeUINT32(s.get(), MFSampleExtension_CleanPoint, FALSE) || annexb::HasIdr(au);
            if (key && annexb::ExtractParameterSets(au).empty() && !sequence_header.empty())
                au.insert(au.begin(), sequence_header.begin(), sequence_header.end());
            out.push_back(std::move(au));
        }
    }
};

H264Encoder::H264Encoder() : impl_(std::make_unique<Impl>()) {}
H264Encoder::~H264Encoder() = default;

bool H264Encoder::Init(uint32_t width, uint32_t height, uint32_t fps, uint32_t bitrate_kbps) {
    auto& e = *impl_;
    e = Impl{};
    e.width = width;
    e.height = height;
    e.fps = fps ? fps : 30;
    if (FAILED(CoCreateInstance(CLSID_CMSH264EncoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&e.mft))))
        return false;
    wil::com_ptr<IMFMediaType> out;
    MFCreateMediaType(&out);
    out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    out->SetUINT32(MF_MT_AVG_BITRATE, bitrate_kbps * 1000);
    MFSetAttributeSize(out.get(), MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(out.get(), MF_MT_FRAME_RATE, e.fps, 1);
    MFSetAttributeRatio(out.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    out->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main);
    if (FAILED(e.mft->SetOutputType(0, out.get(), 0))) return false;

    wil::com_ptr<IMFMediaType> in;
    MFCreateMediaType(&in);
    in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    MFSetAttributeSize(in.get(), MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(in.get(), MF_MT_FRAME_RATE, e.fps, 1);
    MFSetAttributeRatio(in.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (FAILED(e.mft->SetInputType(0, in.get(), 0))) return false;

    if ((e.api = e.mft.try_query<ICodecAPI>())) {
        SetVariantBool(e.api.get(), CODECAPI_AVLowLatencyMode, true);
        SetVariantU4(e.api.get(), CODECAPI_AVEncMPVGOPSize, e.fps * 2);
        SetVariantU4(e.api.get(), CODECAPI_AVEncMPVDefaultBPictureCount, 0);
        SetVariantU4(e.api.get(), CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR);
        SetVariantU4(e.api.get(), CODECAPI_AVEncCommonMeanBitRate, bitrate_kbps * 1000);
    }
    wil::com_ptr<IMFMediaType> cur;
    if (SUCCEEDED(e.mft->GetOutputCurrentType(0, &cur))) {
        UINT32 n = 0;
        if (SUCCEEDED(cur->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &n)) && n) {
            e.sequence_header.resize(n);
            cur->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, e.sequence_header.data(), n, nullptr);
        }
    }
    MFT_OUTPUT_STREAM_INFO info{};
    e.mft->GetOutputStreamInfo(0, &info);
    e.out_size = info.cbSize;
    e.mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    e.mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return true;
}

bool H264Encoder::Encode(const image::Nv12Frame& frame, int64_t pts_100ns, bool force_keyframe,
                         std::vector<std::vector<uint8_t>>& out) {
    auto& e = *impl_;
    if (!e.mft) return false;
    if (force_keyframe && e.api) SetVariantU4(e.api.get(), CODECAPI_AVEncVideoForceKeyFrame, 1);
    auto sample = MakeSample(frame.data, pts_100ns, 10'000'000 / e.fps);
    if (!sample) return false;
    HRESULT hr = e.mft->ProcessInput(0, sample.get(), 0);
    if (hr == MF_E_NOTACCEPTING) {
        e.Collect(out);
        hr = e.mft->ProcessInput(0, sample.get(), 0);
    }
    if (FAILED(hr)) return false;
    e.Collect(out);
    return true;
}

// ---------------------------------------------------------------------------------------------
// Placeholder

void RenderPlaceholder(image::Nv12Frame& frame, const std::wstring& title, const std::wstring& detail) {
    const int w = static_cast<int>(frame.width), h = static_cast<int>(frame.height);
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;  // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bmp = dc ? CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
    if (!bmp || !bits) {
        if (bmp) DeleteObject(bmp);
        if (dc) DeleteDC(dc);
        image::FillBlack(frame);
        return;
    }
    HGDIOBJ old = SelectObject(dc, bmp);
    RECT all{0, 0, w, h};
    HBRUSH bg = CreateSolidBrush(RGB(22, 24, 30));
    FillRect(dc, &all, bg);
    DeleteObject(bg);
    SetBkMode(dc, TRANSPARENT);

    HFONT title_font = CreateFontW(-h / 14, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                            CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    HFONT detail_font = CreateFontW(-h / 28, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                              CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    SetTextColor(dc, RGB(240, 240, 245));
    SelectObject(dc, title_font);
    RECT t1{0, h / 2 - h / 9, w, h / 2};
    DrawTextW(dc, title.c_str(), -1, &t1, DT_CENTER | DT_BOTTOM | DT_SINGLELINE);
    SetTextColor(dc, RGB(160, 165, 180));
    SelectObject(dc, detail_font);
    RECT t2{w / 10, h / 2 + h / 40, w - w / 10, h};
    DrawTextW(dc, detail.c_str(), -1, &t2, DT_CENTER | DT_TOP | DT_WORDBREAK);
    GdiFlush();
    image::BgraToNv12(static_cast<const uint8_t*>(bits), uint32_t(w) * 4, frame);

    SelectObject(dc, old);
    DeleteObject(title_font);
    DeleteObject(detail_font);
    DeleteObject(bmp);
    DeleteDC(dc);
}

}  // namespace tethercam::media
