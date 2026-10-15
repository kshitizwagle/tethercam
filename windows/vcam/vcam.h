#pragma once

// COM objects of the Tethercam virtual camera (docs/TECH_SPEC.md §8, docs/DESIGN.md).
// Frame Server creates the Activator through DllGetClassObject, activates the MediaSource,
// and pulls samples from the MediaStream. Everything here runs inside the FrameServer service.

#include <windows.h>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <wrl.h>
#include <wrl/module.h>
// ks.h redefines GUID_NULL as a macro, so it must come after everything that includes cguid.h.
#include <ks.h>
#include <ksmedia.h>

#include <mutex>

#include "frame_source.h"
#include "tethercam/vcam_ids.h"

// IKsControl (ksproxy.h) without dragging in the DirectShow headers.
#ifndef _IKsControl_
#define _IKsControl_
struct __declspec(uuid("28F54685-06FD-11D2-B27A-00A0C9223196")) IKsControl : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE KsProperty(PKSPROPERTY property, ULONG property_length, LPVOID data,
                                                 ULONG data_length, ULONG* bytes_returned) = 0;
    virtual HRESULT STDMETHODCALLTYPE KsMethod(PKSMETHOD method, ULONG method_length, LPVOID data,
                                               ULONG data_length, ULONG* bytes_returned) = 0;
    virtual HRESULT STDMETHODCALLTYPE KsEvent(PKSEVENT event, ULONG event_length, LPVOID data, ULONG data_length,
                                              ULONG* bytes_returned) = 0;
};
#endif

namespace tethercam::vcam {

using Microsoft::WRL::ChainInterfaces;
using Microsoft::WRL::ClassicCom;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;

class MediaSource;

// Forwards every IMFAttributes method to the member `attrs_`.
#define TETHERCAM_FORWARD_ATTRIBUTES \
    STDMETHODIMP GetItem(REFGUID k, PROPVARIANT* v) override { return attrs_->GetItem(k, v); } \
    STDMETHODIMP GetItemType(REFGUID k, MF_ATTRIBUTE_TYPE* t) override { return attrs_->GetItemType(k, t); } \
    STDMETHODIMP CompareItem(REFGUID k, REFPROPVARIANT v, BOOL* r) override { return attrs_->CompareItem(k, v, r); } \
    STDMETHODIMP Compare(IMFAttributes* a, MF_ATTRIBUTES_MATCH_TYPE m, BOOL* r) override { return attrs_->Compare(a, m, r); } \
    STDMETHODIMP GetUINT32(REFGUID k, UINT32* v) override { return attrs_->GetUINT32(k, v); } \
    STDMETHODIMP GetUINT64(REFGUID k, UINT64* v) override { return attrs_->GetUINT64(k, v); } \
    STDMETHODIMP GetDouble(REFGUID k, double* v) override { return attrs_->GetDouble(k, v); } \
    STDMETHODIMP GetGUID(REFGUID k, GUID* v) override { return attrs_->GetGUID(k, v); } \
    STDMETHODIMP GetStringLength(REFGUID k, UINT32* n) override { return attrs_->GetStringLength(k, n); } \
    STDMETHODIMP GetString(REFGUID k, LPWSTR s, UINT32 n, UINT32* l) override { return attrs_->GetString(k, s, n, l); } \
    STDMETHODIMP GetAllocatedString(REFGUID k, LPWSTR* s, UINT32* l) override { return attrs_->GetAllocatedString(k, s, l); } \
    STDMETHODIMP GetBlobSize(REFGUID k, UINT32* n) override { return attrs_->GetBlobSize(k, n); } \
    STDMETHODIMP GetBlob(REFGUID k, UINT8* b, UINT32 n, UINT32* l) override { return attrs_->GetBlob(k, b, n, l); } \
    STDMETHODIMP GetAllocatedBlob(REFGUID k, UINT8** b, UINT32* n) override { return attrs_->GetAllocatedBlob(k, b, n); } \
    STDMETHODIMP GetUnknown(REFGUID k, REFIID i, LPVOID* p) override { return attrs_->GetUnknown(k, i, p); } \
    STDMETHODIMP SetItem(REFGUID k, REFPROPVARIANT v) override { return attrs_->SetItem(k, v); } \
    STDMETHODIMP DeleteItem(REFGUID k) override { return attrs_->DeleteItem(k); } \
    STDMETHODIMP DeleteAllItems() override { return attrs_->DeleteAllItems(); } \
    STDMETHODIMP SetUINT32(REFGUID k, UINT32 v) override { return attrs_->SetUINT32(k, v); } \
    STDMETHODIMP SetUINT64(REFGUID k, UINT64 v) override { return attrs_->SetUINT64(k, v); } \
    STDMETHODIMP SetDouble(REFGUID k, double v) override { return attrs_->SetDouble(k, v); } \
    STDMETHODIMP SetGUID(REFGUID k, REFGUID v) override { return attrs_->SetGUID(k, v); } \
    STDMETHODIMP SetString(REFGUID k, LPCWSTR v) override { return attrs_->SetString(k, v); } \
    STDMETHODIMP SetBlob(REFGUID k, const UINT8* b, UINT32 n) override { return attrs_->SetBlob(k, b, n); } \
    STDMETHODIMP SetUnknown(REFGUID k, IUnknown* u) override { return attrs_->SetUnknown(k, u); } \
    STDMETHODIMP LockStore() override { return attrs_->LockStore(); } \
    STDMETHODIMP UnlockStore() override { return attrs_->UnlockStore(); } \
    STDMETHODIMP GetCount(UINT32* n) override { return attrs_->GetCount(n); } \
    STDMETHODIMP GetItemByIndex(UINT32 i, GUID* k, PROPVARIANT* v) override { return attrs_->GetItemByIndex(i, k, v); } \
    STDMETHODIMP CopyAllItems(IMFAttributes* dst) override { return attrs_->CopyAllItems(dst); }

class MediaStream
    : public RuntimeClass<RuntimeClassFlags<ClassicCom>,
                          ChainInterfaces<IMFMediaStream2, IMFMediaStream, IMFMediaEventGenerator>, IKsControl> {
public:
    HRESULT RuntimeClassInitialize(MediaSource* source, IMFStreamDescriptor* descriptor);

    // IMFMediaEventGenerator
    STDMETHODIMP GetEvent(DWORD flags, IMFMediaEvent** e) override;
    STDMETHODIMP BeginGetEvent(IMFAsyncCallback* cb, IUnknown* state) override;
    STDMETHODIMP EndGetEvent(IMFAsyncResult* r, IMFMediaEvent** e) override;
    STDMETHODIMP QueueEvent(MediaEventType t, REFGUID ext, HRESULT hr, const PROPVARIANT* v) override;
    // IMFMediaStream
    STDMETHODIMP GetMediaSource(IMFMediaSource** source) override;
    STDMETHODIMP GetStreamDescriptor(IMFStreamDescriptor** sd) override;
    STDMETHODIMP RequestSample(IUnknown* token) override;
    // IMFMediaStream2
    STDMETHODIMP SetStreamState(MF_STREAM_STATE state) override;
    STDMETHODIMP GetStreamState(MF_STREAM_STATE* state) override;
    // IKsControl
    STDMETHODIMP KsProperty(PKSPROPERTY, ULONG, LPVOID, ULONG, ULONG*) override;
    STDMETHODIMP KsMethod(PKSMETHOD, ULONG, LPVOID, ULONG, ULONG*) override;
    STDMETHODIMP KsEvent(PKSEVENT, ULONG, LPVOID, ULONG, ULONG*) override;

    HRESULT Start(IMFMediaType* type);
    HRESULT Stop();
    void Shutdown();
    void SetAllocator(IUnknown* allocator);
    void SetD3DManager(IUnknown* manager);

private:
    HRESULT MakeSample(ComPtr<IMFSample>& sample);

    std::mutex mu_;
    ComPtr<MediaSource> source_;
    ComPtr<IMFMediaEventQueue> queue_;
    ComPtr<IMFStreamDescriptor> descriptor_;
    ComPtr<IMFVideoSampleAllocatorEx> allocator_;
    ComPtr<IUnknown> d3d_manager_;
    MF_STREAM_STATE state_ = MF_STREAM_STATE_STOPPED;
    bool shutdown_ = false;
    uint32_t width_ = 0, height_ = 0, fps_ = 30;
    LONGLONG next_due_ = 0;
    HANDLE timer_ = nullptr;
    FrameSource frames_;
    uint64_t delivered_ = 0, live_ = 0, repeated_ = 0;
    LONGLONG last_sample_time_ = 0;
    bool last_live_ = false;  // marks live <-> placeholder edges with MFSampleExtension_Discontinuity
    bool have_sample_ = false;
};

class MediaSource
    : public RuntimeClass<RuntimeClassFlags<ClassicCom>,
                          ChainInterfaces<IMFMediaSourceEx, IMFMediaSource, IMFMediaEventGenerator>, IMFGetService,
                          IKsControl, IMFSampleAllocatorControl> {
public:
    HRESULT RuntimeClassInitialize(IMFAttributes* activator_attributes);

    // IMFMediaEventGenerator
    STDMETHODIMP GetEvent(DWORD flags, IMFMediaEvent** e) override;
    STDMETHODIMP BeginGetEvent(IMFAsyncCallback* cb, IUnknown* state) override;
    STDMETHODIMP EndGetEvent(IMFAsyncResult* r, IMFMediaEvent** e) override;
    STDMETHODIMP QueueEvent(MediaEventType t, REFGUID ext, HRESULT hr, const PROPVARIANT* v) override;
    // IMFMediaSource
    STDMETHODIMP GetCharacteristics(DWORD* c) override;
    STDMETHODIMP CreatePresentationDescriptor(IMFPresentationDescriptor** pd) override;
    STDMETHODIMP Start(IMFPresentationDescriptor* pd, const GUID* time_format, const PROPVARIANT* start) override;
    STDMETHODIMP Stop() override;
    STDMETHODIMP Pause() override;
    STDMETHODIMP Shutdown() override;
    // IMFMediaSourceEx
    STDMETHODIMP GetSourceAttributes(IMFAttributes** a) override;
    STDMETHODIMP GetStreamAttributes(DWORD id, IMFAttributes** a) override;
    STDMETHODIMP SetD3DManager(IUnknown* manager) override;
    // IMFGetService
    STDMETHODIMP GetService(REFGUID service, REFIID iid, LPVOID* obj) override;
    // IKsControl
    STDMETHODIMP KsProperty(PKSPROPERTY, ULONG, LPVOID, ULONG, ULONG*) override;
    STDMETHODIMP KsMethod(PKSMETHOD, ULONG, LPVOID, ULONG, ULONG*) override;
    STDMETHODIMP KsEvent(PKSEVENT, ULONG, LPVOID, ULONG, ULONG*) override;
    // IMFSampleAllocatorControl
    STDMETHODIMP SetDefaultAllocator(DWORD stream_id, IUnknown* allocator) override;
    STDMETHODIMP GetAllocatorUsage(DWORD stream_id, DWORD* input_stream_id, MFSampleAllocatorUsage* usage) override;

private:
    std::mutex mu_;
    ComPtr<IMFAttributes> attributes_;
    ComPtr<IMFMediaEventQueue> queue_;
    ComPtr<IMFPresentationDescriptor> descriptor_;
    ComPtr<MediaStream> stream_;
    bool shutdown_ = false;
    bool started_once_ = false;
};

class __declspec(uuid("9C8F9375-C87A-46A7-8BFD-552ED47E75BA")) Activator
    : public RuntimeClass<RuntimeClassFlags<ClassicCom>, ChainInterfaces<IMFActivate, IMFAttributes>> {
public:
    TETHERCAM_FORWARD_ATTRIBUTES
    HRESULT RuntimeClassInitialize();
    STDMETHODIMP ActivateObject(REFIID riid, void** ppv) override;
    STDMETHODIMP ShutdownObject() override;
    STDMETHODIMP DetachObject() override;

private:
    ComPtr<IMFAttributes> attrs_;
    ComPtr<MediaSource> source_;
};

}  // namespace tethercam::vcam
