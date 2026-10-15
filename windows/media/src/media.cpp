#include "tethercam/media.h"

#include <codecapi.h>
#include <d3d11.h>
#include <d3d10.h>
#include <dxgi1_6.h>
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

    // A D3D11 device on `adapter` (nullptr = the default one) that can decode H.264 in hardware.
    static wil::com_ptr<ID3D11Device> CreateDecodeDevice(IDXGIAdapter* adapter) {
        UINT flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        static const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                                                   D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
        wil::com_ptr<ID3D11Device> dev;
        if (FAILED(D3D11CreateDevice(adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                     flags, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &dev, nullptr, nullptr)))
            return nullptr;
        auto video = dev.try_query<ID3D11VideoDevice>();
        if (!video) return nullptr;
        for (UINT i = 0, n = video->GetVideoDecoderProfileCount(); i < n; ++i) {
            GUID profile{};
            if (SUCCEEDED(video->GetVideoDecoderProfile(i, &profile)) && profile == D3D11_DECODER_PROFILE_H264_VLD_NOFGT)
                return dev;
        }
        return nullptr;
    }

    // Prefers the high-performance (discrete) GPU and falls back to the next one (integrated) when it
    // can't decode H.264; the default adapter on systems without IDXGIFactory6.
    bool CreateDevice() {
        wil::com_ptr<IDXGIFactory1> factory;
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
            if (auto f6 = factory.try_query<IDXGIFactory6>()) {
                wil::com_ptr<IDXGIAdapter1> adapter;
                for (UINT i = 0; SUCCEEDED(f6->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                                          IID_PPV_ARGS(adapter.put())));
                     ++i) {
                    DXGI_ADAPTER_DESC1 desc{};
                    adapter->GetDesc1(&desc);
                    if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
                    char name[128]{};
                    WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name) - 1, nullptr, nullptr);
                    if ((device = CreateDecodeDevice(adapter.get()))) {
                        log::Info("decoder GPU: {}", name);
                        return true;
                    }
                    log::Info("decoder GPU: {} has no H.264 hardware decode, trying the next one", name);
                }
                return false;
            }
        }
        device = CreateDecodeDevice(nullptr);
        return device != nullptr;
    }

    bool SetupD3D() {
        if (!CreateDevice()) return false;
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

        DecodedFrame frame;
        frame.coded_width = coded_width;
        frame.coded_height = coded_height;
        frame.display_width = display_width;
        frame.display_height = display_height;

        // Hardware frames: offer the DXGI surface itself. Consumers that can scale on the GPU use
        // it and never download; everyone else calls View().
        if (device) {
            auto dxgi = buf.try_query<IMFDXGIBuffer>();
            UINT index = 0;
            if (dxgi && SUCCEEDED(dxgi->GetSubresourceIndex(&index)) &&
                SUCCEEDED(dxgi->GetResource(IID_PPV_ARGS(&frame.texture))))
                frame.subresource = index;
        }

        auto b2d = buf.try_query<IMF2DBuffer>();
        bool got = false, locked2d = false, locked1d = false;
        image::Nv12View view;
        frame.view_fn = [&]() -> image::Nv12View {
            if (got) return view;
            got = true;
            if (b2d) {
                BYTE* scan0 = nullptr;
                LONG pitch = 0;
                if (SUCCEEDED(b2d->Lock2D(&scan0, &pitch))) {
                    view = {scan0, scan0 + size_t(pitch) * coded_height, uint32_t(pitch), uint32_t(pitch), display_width,
                            display_height};
                    locked2d = true;
                }
            } else {
                BYTE* p = nullptr;
                DWORD len = 0;
                if (SUCCEEDED(buf->Lock(&p, nullptr, &len))) {
                    locked1d = true;
                    const uint32_t pitch = static_cast<uint32_t>(std::abs(default_stride));
                    if (size_t(pitch) * coded_height * 3 / 2 <= len)
                        view = {p, p + size_t(pitch) * coded_height, pitch, pitch, display_width, display_height};
                }
            }
            return view;
        };

        cb(frame, pts);

        if (locked2d) b2d->Unlock2D();
        if (locked1d) buf->Unlock();
        if (frame.texture) frame.texture->Release();
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

ID3D11Device* H264Decoder::Device() const { return impl_->device.get(); }

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
// GPU scaling

struct Nv12Scaler::Impl {
    wil::com_ptr<ID3D11Device> device;
    wil::com_ptr<ID3D11DeviceContext> context;
    wil::com_ptr<ID3D11VideoDevice> video;
    wil::com_ptr<ID3D11VideoContext> video_ctx;
    wil::com_ptr<ID3D11VideoProcessorEnumerator> enumerator;
    wil::com_ptr<ID3D11VideoProcessor> processor;
    bool broken = false;  // a setup failure: stay on the CPU path for good
    uint32_t in_w = 0, in_h = 0, out_w = 0, out_h = 0;
    wil::com_ptr<ID3D11Texture2D> out_tex;
    wil::com_ptr<ID3D11VideoProcessorOutputView> out_view;
    wil::com_ptr<ID3D11Texture2D> staging;
    // The decoder rotates a small pool of surfaces; keep the matching input view around.
    ID3D11Texture2D* last_src = nullptr;
    UINT last_subresource = 0;
    wil::com_ptr<ID3D11VideoProcessorInputView> in_view;

    // Everything the processor needs, rebuilt when the geometry changes.
    bool Reset(uint32_t iw, uint32_t ih, uint32_t ow, uint32_t oh) {
        processor.reset();
        enumerator.reset();
        out_tex.reset();
        out_view.reset();
        staging.reset();
        in_view.reset();
        last_src = nullptr;
        last_subresource = 0;
        in_w = iw;
        in_h = ih;
        out_w = ow;
        out_h = oh;

        D3D11_VIDEO_PROCESSOR_CONTENT_DESC desc{};
        desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        desc.InputFrameRate = {30, 1};
        desc.InputWidth = iw;
        desc.InputHeight = ih;
        desc.OutputFrameRate = {30, 1};
        desc.OutputWidth = ow;
        desc.OutputHeight = oh;
        desc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
        if (FAILED(video->CreateVideoProcessorEnumerator(&desc, &enumerator))) return false;
        UINT support = 0;
        if (FAILED(enumerator->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &support)) ||
            (support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) == 0)
            return false;
        support = 0;
        if (FAILED(enumerator->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &support)) ||
            (support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT) == 0)
            return false;
        if (FAILED(video->CreateVideoProcessor(enumerator.get(), 0, &processor))) return false;

        D3D11_TEXTURE2D_DESC td{};
        td.Width = ow;
        td.Height = oh;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_NV12;
        td.SampleDesc = {1, 0};
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (FAILED(device->CreateTexture2D(&td, nullptr, &out_tex))) return false;
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC od{};
        od.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        od.Texture2D.MipSlice = 0;
        if (FAILED(video->CreateVideoProcessorOutputView(out_tex.get(), enumerator.get(), &od, &out_view))) return false;
        td.BindFlags = 0;
        td.Usage = D3D11_USAGE_STAGING;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(device->CreateTexture2D(&td, nullptr, &staging))) return false;

        // Constant stream state: no framerate conversion, no auto-processing, BT.601 limited range
        // (the same convention as the rest of the pipeline), black outside the stretch rect.
        video_ctx->VideoProcessorSetOutputTargetRect(processor.get(), FALSE, nullptr);
        video_ctx->VideoProcessorSetStreamFrameFormat(processor.get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
        video_ctx->VideoProcessorSetStreamAutoProcessingMode(processor.get(), 0, FALSE);
        D3D11_VIDEO_COLOR bg{};
        bg.YCbCr = {16.0f / 255.0f, 128.0f / 255.0f, 128.0f / 255.0f, 1.0f};
        video_ctx->VideoProcessorSetOutputBackgroundColor(processor.get(), FALSE, &bg);
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE cs{};
        cs.Usage = 0;
        cs.RGB_Range = 0;
        cs.YCbCr_Matrix = 0;  // BT.601
        cs.YCbCr_xvYCC = 0;
        cs.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
        video_ctx->VideoProcessorSetStreamColorSpace(processor.get(), 0, &cs);
        video_ctx->VideoProcessorSetOutputColorSpace(processor.get(), &cs);
        return true;
    }
};

Nv12Scaler::Nv12Scaler() = default;
Nv12Scaler::~Nv12Scaler() = default;

bool Nv12Scaler::Init(ID3D11Device* device) {
    impl_ = std::make_unique<Impl>();
    if (!device) return false;
    auto& d = *impl_;
    d.device.reset(device);
    d.device->AddRef();  // wil::reset takes ownership, the decoder keeps its own reference
    if (FAILED(d.device->GetImmediateContext(&d.context))) return false;
    if (!(d.video = d.device.try_query<ID3D11VideoDevice>())) return false;
    if (!(d.video_ctx = d.device.try_query<ID3D11VideoContext>())) return false;
    return true;
}

bool Nv12Scaler::Ok() const { return impl_ && !impl_->broken && impl_->video_ctx != nullptr; }

bool Nv12Scaler::Scale(const DecodedFrame& src, uint32_t dst_width, uint32_t dst_height,
                       const std::function<bool(const image::Nv12View&)>& write) {
    if (!impl_) return false;
    auto& d = *impl_;
    if (d.broken || !src.texture) return false;
    if (!d.processor || d.in_w != src.coded_width || d.in_h != src.coded_height || d.out_w != dst_width ||
        d.out_h != dst_height) {
        if (!d.Reset(src.coded_width, src.coded_height, dst_width, dst_height)) {
            d.broken = true;
            return false;
        }
        log::Info("GPU scale ready: {}x{} -> {}x{}", src.coded_width, src.coded_height, dst_width, dst_height);
    }

    if (d.last_src != src.texture || d.last_subresource != src.subresource || !d.in_view) {
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC id{};
        id.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        id.Texture2D.MipSlice = 0;
        id.Texture2D.ArraySlice = src.subresource;
        wil::com_ptr<ID3D11VideoProcessorInputView> view;
        if (FAILED(d.video->CreateVideoProcessorInputView(src.texture, d.enumerator.get(), &id, &view))) return false;
        d.in_view = std::move(view);
        d.last_src = src.texture;
        d.last_subresource = src.subresource;
    }

    const RECT src_rect{0, 0, LONG(src.display_width), LONG(src.display_height)};
    const image::Rect fit = image::LetterboxRect(src.display_width, src.display_height, dst_width, dst_height);
    const RECT dst_rect{LONG(fit.x), LONG(fit.y), LONG(fit.x + fit.w), LONG(fit.y + fit.h)};
    d.video_ctx->VideoProcessorSetStreamSourceRect(d.processor.get(), 0, TRUE, &src_rect);
    d.video_ctx->VideoProcessorSetStreamOutputRect(d.processor.get(), 0, &dst_rect);

    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = d.in_view.get();
    if (FAILED(d.video_ctx->VideoProcessorBlt(d.processor.get(), d.out_view.get(), 0, 1, &stream))) return false;

    d.context->CopyResource(d.staging.get(), d.out_tex.get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(d.context->Map(d.staging.get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;
    const auto* y = static_cast<const uint8_t*>(mapped.pData);
    image::Nv12View view{y, y + size_t(mapped.RowPitch) * dst_height, mapped.RowPitch, mapped.RowPitch, dst_width,
                         dst_height};
    const bool ok = write(view);
    d.context->Unmap(d.staging.get(), 0);
    return ok;
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
