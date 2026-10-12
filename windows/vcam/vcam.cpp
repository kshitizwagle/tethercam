#include "vcam.h"

#include <cstdio>

#include "tethercam/formats.h"

namespace tethercam::vcam {
namespace {

void Trace(const char* fmt, ...) {
    char buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    OutputDebugStringA("Tethercam vcam: ");
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");
}

HRESULT MakeMediaType(const MediaFormat& f, ComPtr<IMFMediaType>& out) {
    ComPtr<IMFMediaType> t;
    HRESULT hr = MFCreateMediaType(&t);
    if (FAILED(hr)) return hr;
    t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    t->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    MFSetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, f.width, f.height);
    MFSetAttributeRatio(t.Get(), MF_MT_FRAME_RATE, f.fps, 1);
    MFSetAttributeRatio(t.Get(), MF_MT_FRAME_RATE_RANGE_MAX, f.fps, 1);
    MFSetAttributeRatio(t.Get(), MF_MT_FRAME_RATE_RANGE_MIN, f.fps, 1);
    MFSetAttributeRatio(t.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    t->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    t->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    t->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE);
    t->SetUINT32(MF_MT_DEFAULT_STRIDE, f.width);
    t->SetUINT32(MF_MT_SAMPLE_SIZE, f.width * f.height * 3 / 2);
    t->SetUINT32(MF_MT_AVG_BITRATE, f.width * f.height * 12 * f.fps);
    out = t;
    return S_OK;
}

}  // namespace

// =============================================================================================
// Activator

HRESULT Activator::RuntimeClassInitialize() {
    HRESULT hr = MFCreateAttributes(&attrs_, 1);
    if (FAILED(hr)) return hr;
    Trace("activator created");
    return S_OK;
}

STDMETHODIMP Activator::ActivateObject(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (!source_) {
        HRESULT hr = Microsoft::WRL::MakeAndInitialize<MediaSource>(&source_, attrs_.Get());
        if (FAILED(hr)) {
            Trace("MediaSource init failed 0x%08lx", hr);
            return hr;
        }
    }
    return source_.CopyTo(riid, ppv);
}

STDMETHODIMP Activator::ShutdownObject() {
    if (source_) source_->Shutdown();
    source_.Reset();
    return S_OK;
}

STDMETHODIMP Activator::DetachObject() {
    source_.Reset();
    return S_OK;
}

// =============================================================================================
// MediaSource

HRESULT MediaSource::RuntimeClassInitialize(IMFAttributes* activator_attributes) {
    HRESULT hr = MFCreateAttributes(&attributes_, 4);
    if (FAILED(hr)) return hr;
    if (activator_attributes) activator_attributes->CopyAllItems(attributes_.Get());
    if (FAILED(hr = MFCreateEventQueue(&queue_))) return hr;

    const auto formats = AdvertisedFormats();
    std::vector<ComPtr<IMFMediaType>> types(formats.size());
    std::vector<IMFMediaType*> raw(formats.size());
    for (size_t i = 0; i < formats.size(); ++i) {
        if (FAILED(hr = MakeMediaType(formats[i], types[i]))) return hr;
        raw[i] = types[i].Get();
    }
    ComPtr<IMFStreamDescriptor> sd;
    if (FAILED(hr = MFCreateStreamDescriptor(0, static_cast<DWORD>(raw.size()), raw.data(), &sd))) return hr;
    ComPtr<IMFMediaTypeHandler> handler;
    if (FAILED(hr = sd->GetMediaTypeHandler(&handler))) return hr;
    if (FAILED(hr = handler->SetCurrentMediaType(raw[0]))) return hr;
    sd->SetGUID(MF_DEVICESTREAM_STREAM_CATEGORY, PINNAME_VIDEO_CAPTURE);
    sd->SetUINT32(MF_DEVICESTREAM_STREAM_ID, 0);
    sd->SetUINT32(MF_DEVICESTREAM_FRAMESERVER_SHARED, 1);
    sd->SetUINT32(MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES, MFFrameSourceTypes_Color);

    IMFStreamDescriptor* sds[] = {sd.Get()};
    if (FAILED(hr = MFCreatePresentationDescriptor(1, sds, &descriptor_))) return hr;
    if (FAILED(hr = descriptor_->SelectStream(0))) return hr;
    if (FAILED(hr = Microsoft::WRL::MakeAndInitialize<MediaStream>(&stream_, this, sd.Get()))) return hr;
    Trace("media source ready (%zu media types)", formats.size());
    return S_OK;
}

STDMETHODIMP MediaSource::GetEvent(DWORD flags, IMFMediaEvent** e) {
    ComPtr<IMFMediaEventQueue> q;
    {
        std::lock_guard lock(mu_);
        if (shutdown_) return MF_E_SHUTDOWN;
        q = queue_;
    }
    return q->GetEvent(flags, e);  // may block: never under the lock
}

STDMETHODIMP MediaSource::BeginGetEvent(IMFAsyncCallback* cb, IUnknown* state) {
    std::lock_guard lock(mu_);
    if (shutdown_) return MF_E_SHUTDOWN;
    return queue_->BeginGetEvent(cb, state);
}

STDMETHODIMP MediaSource::EndGetEvent(IMFAsyncResult* r, IMFMediaEvent** e) {
    std::lock_guard lock(mu_);
    if (shutdown_) return MF_E_SHUTDOWN;
    return queue_->EndGetEvent(r, e);
}

STDMETHODIMP MediaSource::QueueEvent(MediaEventType t, REFGUID ext, HRESULT hr, const PROPVARIANT* v) {
    std::lock_guard lock(mu_);
    if (shutdown_) return MF_E_SHUTDOWN;
    return queue_->QueueEventParamVar(t, ext, hr, v);
}

STDMETHODIMP MediaSource::GetCharacteristics(DWORD* c) {
    if (!c) return E_POINTER;
    std::lock_guard lock(mu_);
    if (shutdown_) return MF_E_SHUTDOWN;
    *c = MFMEDIASOURCE_IS_LIVE;
    return S_OK;
}

STDMETHODIMP MediaSource::CreatePresentationDescriptor(IMFPresentationDescriptor** pd) {
    if (!pd) return E_POINTER;
    std::lock_guard lock(mu_);
    if (shutdown_) return MF_E_SHUTDOWN;
    return descriptor_->Clone(pd);
}

STDMETHODIMP MediaSource::Start(IMFPresentationDescriptor* pd, const GUID* time_format, const PROPVARIANT* start) {
    if (!pd) return E_POINTER;
    if (time_format && *time_format != GUID_NULL) return MF_E_UNSUPPORTED_TIME_FORMAT;
    std::lock_guard lock(mu_);
    if (shutdown_) return MF_E_SHUTDOWN;

    BOOL selected = FALSE;
    ComPtr<IMFStreamDescriptor> sd;
    HRESULT hr = pd->GetStreamDescriptorByIndex(0, &selected, &sd);
    if (FAILED(hr)) return hr;
    if (selected) {
        ComPtr<IMFMediaTypeHandler> handler;
        ComPtr<IMFMediaType> type;
        if (FAILED(hr = sd->GetMediaTypeHandler(&handler))) return hr;
        if (FAILED(hr = handler->GetCurrentMediaType(&type))) return hr;
        ComPtr<IUnknown> unk;
        stream_.As(&unk);
        if (FAILED(hr = queue_->QueueEventParamUnk(started_once_ ? MEUpdatedStream : MENewStream, GUID_NULL, S_OK,
                                                    unk.Get())))
            return hr;
        started_once_ = true;
        if (FAILED(hr = stream_->Start(type.Get()))) return hr;
    }
    PROPVARIANT time;
    PropVariantInit(&time);
    if (start && start->vt == VT_I8) time = *start;
    else {
        time.vt = VT_I8;
        time.hVal.QuadPart = MFGetSystemTime();
    }
    return queue_->QueueEventParamVar(MESourceStarted, GUID_NULL, S_OK, &time);
}

STDMETHODIMP MediaSource::Stop() {
    std::lock_guard lock(mu_);
    if (shutdown_) return MF_E_SHUTDOWN;
    stream_->Stop();
    PROPVARIANT time;
    PropVariantInit(&time);
    time.vt = VT_I8;
    time.hVal.QuadPart = MFGetSystemTime();
    return queue_->QueueEventParamVar(MESourceStopped, GUID_NULL, S_OK, &time);
}

STDMETHODIMP MediaSource::Pause() { return MF_E_INVALID_STATE_TRANSITION; }

STDMETHODIMP MediaSource::Shutdown() {
    ComPtr<MediaStream> stream;
    {
        std::lock_guard lock(mu_);
        if (shutdown_) return S_OK;
        shutdown_ = true;
        if (queue_) queue_->Shutdown();
        stream = std::move(stream_);
    }
    if (stream) stream->Shutdown();  // outside the lock: the stream calls back into us
    Trace("media source shut down");
    return S_OK;
}

STDMETHODIMP MediaSource::GetSourceAttributes(IMFAttributes** a) {
    if (!a) return E_POINTER;
    std::lock_guard lock(mu_);
    if (shutdown_) return MF_E_SHUTDOWN;
    return attributes_.CopyTo(a);
}

STDMETHODIMP MediaSource::GetStreamAttributes(DWORD id, IMFAttributes** a) {
    if (!a) return E_POINTER;
    std::lock_guard lock(mu_);
    if (shutdown_) return MF_E_SHUTDOWN;
    if (id != 0) return MF_E_INVALIDSTREAMNUMBER;
    BOOL selected = FALSE;
    ComPtr<IMFStreamDescriptor> sd;
    HRESULT hr = descriptor_->GetStreamDescriptorByIndex(0, &selected, &sd);
    if (FAILED(hr)) return hr;
    return sd.CopyTo(a);
}

STDMETHODIMP MediaSource::SetD3DManager(IUnknown* manager) {
    std::lock_guard lock(mu_);
    if (shutdown_) return MF_E_SHUTDOWN;
    stream_->SetD3DManager(manager);
    return S_OK;
}

STDMETHODIMP MediaSource::GetService(REFGUID, REFIID, LPVOID* obj) {
    if (obj) *obj = nullptr;
    return MF_E_UNSUPPORTED_SERVICE;
}

STDMETHODIMP MediaSource::KsProperty(PKSPROPERTY, ULONG, LPVOID, ULONG, ULONG*) {
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}
STDMETHODIMP MediaSource::KsMethod(PKSMETHOD, ULONG, LPVOID, ULONG, ULONG*) {
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}
STDMETHODIMP MediaSource::KsEvent(PKSEVENT, ULONG, LPVOID, ULONG, ULONG*) {
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

STDMETHODIMP MediaSource::SetDefaultAllocator(DWORD stream_id, IUnknown* allocator) {
    std::lock_guard lock(mu_);
    if (shutdown_) return MF_E_SHUTDOWN;
    if (stream_id != 0) return MF_E_INVALIDSTREAMNUMBER;
    stream_->SetAllocator(allocator);
    return S_OK;
}

STDMETHODIMP MediaSource::GetAllocatorUsage(DWORD stream_id, DWORD* input_stream_id, MFSampleAllocatorUsage* usage) {
    if (!input_stream_id || !usage) return E_POINTER;
    if (stream_id != 0) return MF_E_INVALIDSTREAMNUMBER;
    *input_stream_id = stream_id;
    *usage = MFSampleAllocatorUsage_UsesProvidedAllocator;
    return S_OK;
}

// =============================================================================================
// MediaStream

HRESULT MediaStream::RuntimeClassInitialize(MediaSource* source, IMFStreamDescriptor* descriptor) {
    source_ = source;
    descriptor_ = descriptor;
    timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    return MFCreateEventQueue(&queue_);
}

STDMETHODIMP MediaStream::GetEvent(DWORD flags, IMFMediaEvent** e) {
    ComPtr<IMFMediaEventQueue> q;
    {
        std::lock_guard lock(mu_);
        if (shutdown_) return MF_E_SHUTDOWN;
        q = queue_;
    }
    return q->GetEvent(flags, e);
}

STDMETHODIMP MediaStream::BeginGetEvent(IMFAsyncCallback* cb, IUnknown* state) {
    std::lock_guard lock(mu_);
    if (shutdown_) return MF_E_SHUTDOWN;
    return queue_->BeginGetEvent(cb, state);
}

STDMETHODIMP MediaStream::EndGetEvent(IMFAsyncResult* r, IMFMediaEvent** e) {
    std::lock_guard lock(mu_);
    if (shutdown_) return MF_E_SHUTDOWN;
    return queue_->EndGetEvent(r, e);
}

STDMETHODIMP MediaStream::QueueEvent(MediaEventType t, REFGUID ext, HRESULT hr, const PROPVARIANT* v) {
    std::lock_guard lock(mu_);
    if (shutdown_) return MF_E_SHUTDOWN;
    return queue_->QueueEventParamVar(t, ext, hr, v);
}

STDMETHODIMP MediaStream::GetMediaSource(IMFMediaSource** source) {
    if (!source) return E_POINTER;
    std::lock_guard lock(mu_);
    if (shutdown_ || !source_) return MF_E_SHUTDOWN;
    return source_.CopyTo(source);
}

STDMETHODIMP MediaStream::GetStreamDescriptor(IMFStreamDescriptor** sd) {
    if (!sd) return E_POINTER;
    std::lock_guard lock(mu_);
    if (shutdown_) return MF_E_SHUTDOWN;
    return descriptor_.CopyTo(sd);
}

HRESULT MediaStream::MakeSample(ComPtr<IMFSample>& sample) {
    HRESULT hr;
    if (allocator_) {
        hr = allocator_->AllocateSample(&sample);
        if (SUCCEEDED(hr)) return hr;
        Trace("allocator failed 0x%08lx, using memory sample", hr);
    }
    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(hr = MFCreate2DMediaBuffer(width_, height_, MFVideoFormat_NV12.Data1, FALSE, &buffer))) return hr;
    if (FAILED(hr = MFCreateSample(&sample))) return hr;
    return sample->AddBuffer(buffer.Get());
}

STDMETHODIMP MediaStream::RequestSample(IUnknown* token) {
    ComPtr<IMFSample> sample;
    LONGLONG due = 0;
    {
        std::lock_guard lock(mu_);
        if (shutdown_) return MF_E_SHUTDOWN;
        if (state_ != MF_STREAM_STATE_RUNNING) return MF_E_MEDIA_SOURCE_WRONGSTATE;
        due = next_due_;
    }

    // Pace delivery to the negotiated frame rate. Frame Server asks for the next sample as soon
    // as it has one, so without this a live source would spin.
    const LONGLONG interval = 10'000'000 / fps_;
    LONGLONG now = MFGetSystemTime();
    if (due > now && timer_) {
        LARGE_INTEGER rel;
        rel.QuadPart = -(std::min)(due - now, interval);
        SetWaitableTimer(timer_, &rel, 0, nullptr, nullptr, FALSE);
        WaitForSingleObject(timer_, 100);
        now = MFGetSystemTime();
    }

    std::lock_guard lock(mu_);
    if (shutdown_) return MF_E_SHUTDOWN;
    if (state_ != MF_STREAM_STATE_RUNNING) return MF_E_MEDIA_SOURCE_WRONGSTATE;
    next_due_ = (due && now - due < interval) ? due + interval : now + interval;

    HRESULT hr = MakeSample(sample);
    if (FAILED(hr)) return hr;
    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(hr = sample->GetBufferByIndex(0, &buffer))) return hr;

    bool live = false;
    ComPtr<IMF2DBuffer2> b2;
    ComPtr<IMF2DBuffer> b1;
    if (SUCCEEDED(buffer.As(&b2))) {
        BYTE* scan0 = nullptr;
        BYTE* start = nullptr;
        LONG pitch = 0;
        DWORD len = 0;
        if (FAILED(hr = b2->Lock2DSize(MF2DBuffer_LockFlags_Write, &scan0, &pitch, &start, &len))) return hr;
        if (pitch > 0 && DWORD(pitch) * height_ * 3 / 2 <= len)
            live = frames_.Fill(scan0, DWORD(pitch), scan0 + size_t(pitch) * height_, DWORD(pitch));
        b2->Unlock2D();
    } else if (SUCCEEDED(buffer.As(&b1))) {
        BYTE* scan0 = nullptr;
        LONG pitch = 0;
        if (FAILED(hr = b1->Lock2D(&scan0, &pitch))) return hr;
        if (pitch > 0) live = frames_.Fill(scan0, DWORD(pitch), scan0 + size_t(pitch) * height_, DWORD(pitch));
        b1->Unlock2D();
    } else {
        BYTE* p = nullptr;
        DWORD max = 0;
        if (FAILED(hr = buffer->Lock(&p, &max, nullptr))) return hr;
        if (max >= width_ * height_ * 3 / 2) live = frames_.Fill(p, width_, p + size_t(width_) * height_, width_);
        buffer->Unlock();
    }
    buffer->SetCurrentLength(width_ * height_ * 3 / 2);

    sample->SetSampleTime(now);
    sample->SetSampleDuration(interval);
    if (token) sample->SetUnknown(MFSampleExtension_Token, token);
    ++delivered_;
    if (live) ++live_;
    if (delivered_ % 300 == 1) {
        char msg[128];
        std::snprintf(msg, sizeof(msg), "vcam: delivered %llu samples (%llu live) at %ux%u@%u%s", delivered_, live_,
                      width_, height_, fps_, allocator_ ? " via allocator" : "");
        frames_.Report(msg);
    }
    return queue_->QueueEventParamUnk(MEMediaSample, GUID_NULL, S_OK, sample.Get());
}

STDMETHODIMP MediaStream::SetStreamState(MF_STREAM_STATE state) {
    {
        std::lock_guard lock(mu_);
        if (shutdown_) return MF_E_SHUTDOWN;
        if (state == state_) return S_OK;
    }
    if (state == MF_STREAM_STATE_STOPPED) return Stop();
    std::lock_guard lock(mu_);
    if (state == MF_STREAM_STATE_RUNNING) {
        if (width_ == 0) return MF_E_INVALIDREQUEST;
        state_ = MF_STREAM_STATE_RUNNING;
        frames_.Start(width_, height_, fps_);
        return S_OK;
    }
    if (state == MF_STREAM_STATE_PAUSED) {
        state_ = MF_STREAM_STATE_PAUSED;
        return S_OK;
    }
    return E_INVALIDARG;
}

STDMETHODIMP MediaStream::GetStreamState(MF_STREAM_STATE* state) {
    if (!state) return E_POINTER;
    std::lock_guard lock(mu_);
    if (shutdown_) return MF_E_SHUTDOWN;
    *state = state_;
    return S_OK;
}

STDMETHODIMP MediaStream::KsProperty(PKSPROPERTY, ULONG, LPVOID, ULONG, ULONG*) {
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}
STDMETHODIMP MediaStream::KsMethod(PKSMETHOD, ULONG, LPVOID, ULONG, ULONG*) {
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}
STDMETHODIMP MediaStream::KsEvent(PKSEVENT, ULONG, LPVOID, ULONG, ULONG*) {
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

HRESULT MediaStream::Start(IMFMediaType* type) {
    std::lock_guard lock(mu_);
    if (shutdown_) return MF_E_SHUTDOWN;
    UINT32 w = 0, h = 0, num = 30, den = 1;
    MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &w, &h);
    MFGetAttributeRatio(type, MF_MT_FRAME_RATE, &num, &den);
    if (w == 0 || h == 0 || w > shm::kMaxWidth || h > shm::kMaxHeight) return MF_E_INVALIDMEDIATYPE;
    width_ = w;
    height_ = h;
    fps_ = den ? (std::max)(1u, num / den) : 30;
    if (allocator_) {
        if (d3d_manager_) allocator_->SetDirectXManager(d3d_manager_.Get());
        HRESULT hr = allocator_->InitializeSampleAllocator(10, type);
        if (FAILED(hr)) {
            Trace("InitializeSampleAllocator failed 0x%08lx; using memory samples", hr);
            allocator_.Reset();
        }
    }
    state_ = MF_STREAM_STATE_RUNNING;
    next_due_ = 0;
    frames_.Start(width_, height_, fps_);
    Trace("stream started %ux%u@%u", width_, height_, fps_);
    return queue_->QueueEventParamVar(MEStreamStarted, GUID_NULL, S_OK, nullptr);
}

HRESULT MediaStream::Stop() {
    std::lock_guard lock(mu_);
    if (shutdown_) return MF_E_SHUTDOWN;
    if (state_ == MF_STREAM_STATE_STOPPED) return S_OK;
    state_ = MF_STREAM_STATE_STOPPED;
    frames_.Stop();
    if (allocator_) allocator_->UninitializeSampleAllocator();
    return queue_->QueueEventParamVar(MEStreamStopped, GUID_NULL, S_OK, nullptr);
}

void MediaStream::Shutdown() {
    std::lock_guard lock(mu_);
    if (shutdown_) return;
    if (state_ != MF_STREAM_STATE_STOPPED) frames_.Stop();
    shutdown_ = true;
    state_ = MF_STREAM_STATE_STOPPED;
    if (queue_) queue_->Shutdown();
    if (allocator_) allocator_->UninitializeSampleAllocator();
    allocator_.Reset();
    d3d_manager_.Reset();
    source_.Reset();  // break the source <-> stream cycle
    if (timer_) {
        CloseHandle(timer_);
        timer_ = nullptr;
    }
}

void MediaStream::SetAllocator(IUnknown* allocator) {
    std::lock_guard lock(mu_);
    allocator_.Reset();
    if (allocator) allocator->QueryInterface(IID_PPV_ARGS(&allocator_));
}

void MediaStream::SetD3DManager(IUnknown* manager) {
    std::lock_guard lock(mu_);
    d3d_manager_ = manager;
    if (allocator_ && manager) allocator_->SetDirectXManager(manager);
}

}  // namespace tethercam::vcam
