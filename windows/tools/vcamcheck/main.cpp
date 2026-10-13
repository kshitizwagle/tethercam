// vcamcheck: opens a video capture device the way apps do (Media Foundation source reader),
// reads frames and reports what it got. Used to verify the Tethercam virtual camera headlessly.
//
//   vcamcheck list
//   vcamcheck read [--name Tethercam] [--size WxH] [--fps N] [--frames N] [--save out.bmp]
//   vcamcheck direct --dll PATH [same options]   load the media source in-process (no Frame Server)

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wil/com.h>
#include <wil/resource.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "tethercam/image.h"
#include "tethercam/media.h"
#include "tethercam/vcam_ids.h"

using namespace tethercam;

namespace {

struct Device {
    std::wstring name;
    wil::com_ptr<IMFActivate> activate;
};

std::vector<Device> Enumerate() {
    std::vector<Device> out;
    wil::com_ptr<IMFAttributes> attrs;
    MFCreateAttributes(&attrs, 1);
    attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    IMFActivate** devices = nullptr;
    UINT32 count = 0;
    if (FAILED(MFEnumDeviceSources(attrs.get(), &devices, &count))) return out;
    for (UINT32 i = 0; i < count; ++i) {
        wil::unique_cotaskmem_string name;
        UINT32 len = 0;
        devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &len);
        Device d;
        d.name = name ? name.get() : L"?";
        d.activate = devices[i];
        devices[i]->Release();
        out.push_back(std::move(d));
    }
    CoTaskMemFree(devices);
    return out;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    media::MfScope mf;
    if (!mf.Ok()) return 1;
    std::wstring command = argc > 1 ? argv[1] : L"read";
    std::wstring want = L"Tethercam";
    uint32_t w = 1920, h = 1080, fps = 30;
    int frames = 60;
    std::string save;
    std::wstring dll_path = L"tethercam-vcam.dll";
    for (int i = 2; i < argc; ++i) {
        std::wstring k = argv[i];
        auto next = [&]() -> std::wstring { return i + 1 < argc ? argv[++i] : L""; };
        if (k == L"--name") want = next();
        else if (k == L"--dll") dll_path = next();
        else if (k == L"--size") swscanf_s(next().c_str(), L"%ux%u", &w, &h);
        else if (k == L"--fps") fps = static_cast<uint32_t>(_wtoi(next().c_str()));
        else if (k == L"--frames") frames = _wtoi(next().c_str());
        else if (k == L"--save") {
            auto s = next();
            for (wchar_t c : s) save += static_cast<char>(c);
        }
    }

    wil::com_ptr<IMFMediaSource> source;
    HRESULT hr = S_OK;
    if (command == L"direct") {
        // Load tethercam-vcam.dll in this process and activate its media source without Frame Server.
        HMODULE dll = LoadLibraryW(dll_path.c_str());
        if (!dll) return std::wprintf(L"cannot load %s\n", dll_path.c_str()), 2;
        using GetClassObject = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, LPVOID*);
        auto get = reinterpret_cast<GetClassObject>(GetProcAddress(dll, "DllGetClassObject"));
        wil::com_ptr<IClassFactory> factory;
        if (!get || FAILED(hr = get(kVcamClsid, IID_PPV_ARGS(&factory))))
            return std::printf("DllGetClassObject failed: 0x%08lx\n", hr), 3;
        wil::com_ptr<IMFActivate> activate;
        if (FAILED(hr = factory->CreateInstance(nullptr, IID_PPV_ARGS(&activate))))
            return std::printf("CreateInstance failed: 0x%08lx\n", hr), 3;
        if (FAILED(hr = activate->ActivateObject(IID_PPV_ARGS(&source))))
            return std::printf("ActivateObject failed: 0x%08lx\n", hr), 3;
        std::printf("activated media source from DLL\n");
    } else {
        auto devices = Enumerate();
        if (command == L"list") {
            std::printf("%zu video capture device(s)\n", devices.size());
            for (auto& d : devices) std::wprintf(L"  %s\n", d.name.c_str());
            return 0;
        }
        Device* dev = nullptr;
        for (auto& d : devices)
            if (d.name == want || d.name.rfind(want + L" (", 0) == 0) dev = &d;  // Windows appends " (Windows Virtual Camera)"
        if (!dev) {
            std::wprintf(L"camera \"%s\" not found\n", want.c_str());
            return 2;
        }
        hr = dev->activate->ActivateObject(IID_PPV_ARGS(&source));
        if (FAILED(hr)) return std::printf("ActivateObject failed: 0x%08lx\n", hr), 3;
    }

    // List the media types the camera offers.
    wil::com_ptr<IMFPresentationDescriptor> pd;
    source->CreatePresentationDescriptor(&pd);
    BOOL selected = FALSE;
    wil::com_ptr<IMFStreamDescriptor> sd;
    pd->GetStreamDescriptorByIndex(0, &selected, &sd);
    wil::com_ptr<IMFMediaTypeHandler> handler;
    sd->GetMediaTypeHandler(&handler);
    DWORD types = 0;
    handler->GetMediaTypeCount(&types);
    std::printf("%lu media type(s):", types);
    for (DWORD i = 0; i < types; ++i) {
        wil::com_ptr<IMFMediaType> t;
        handler->GetMediaTypeByIndex(i, &t);
        UINT32 tw = 0, th = 0, num = 0, den = 1;
        MFGetAttributeSize(t.get(), MF_MT_FRAME_SIZE, &tw, &th);
        MFGetAttributeRatio(t.get(), MF_MT_FRAME_RATE, &num, &den);
        std::printf(" %ux%u@%u", tw, th, den ? num / den : 0);
    }
    std::printf("\n");

    wil::com_ptr<IMFSourceReader> reader;
    hr = MFCreateSourceReaderFromMediaSource(source.get(), nullptr, &reader);
    if (FAILED(hr)) return std::printf("MFCreateSourceReaderFromMediaSource failed: 0x%08lx\n", hr), 4;
    bool set = false;
    for (DWORD i = 0; i < types && !set; ++i) {
        wil::com_ptr<IMFMediaType> t;
        reader->GetNativeMediaType(0, i, &t);
        UINT32 tw = 0, th = 0, num = 0, den = 1;
        MFGetAttributeSize(t.get(), MF_MT_FRAME_SIZE, &tw, &th);
        MFGetAttributeRatio(t.get(), MF_MT_FRAME_RATE, &num, &den);
        if (tw == w && th == h && den && num / den == fps) set = SUCCEEDED(reader->SetCurrentMediaType(0, nullptr, t.get()));
    }
    if (!set) return std::printf("camera does not offer %ux%u@%u\n", w, h, fps), 5;

    const auto t0 = std::chrono::steady_clock::now();
    int got = 0;
    LONGLONG first_ts = -1, last_ts = 0;
    while (got < frames) {
        DWORD stream = 0, flags = 0;
        LONGLONG ts = 0;
        wil::com_ptr<IMFSample> sample;
        hr = reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &stream, &flags, &ts, &sample);
        if (FAILED(hr)) return std::printf("ReadSample failed: 0x%08lx\n", hr), 6;
        if (!sample) continue;
        if (first_ts < 0) first_ts = ts;
        last_ts = ts;
        ++got;
        if (got == frames && !save.empty()) {
            wil::com_ptr<IMFMediaBuffer> buf;
            sample->ConvertToContiguousBuffer(&buf);
            BYTE* p = nullptr;
            DWORD len = 0;
            buf->Lock(&p, nullptr, &len);
            if (len >= w * h * 3 / 2) {
                image::Nv12View v{p, p + size_t(w) * h, w, w, w, h};
                std::printf("saved %s: %s\n", save.c_str(), image::SaveBmp(v, save.c_str()) ? "ok" : "failed");
            }
            buf->Unlock();
        }
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("read %d frames at %ux%u in %.2f s: %.1f fps (sample clock %.1f fps)\n", got, w, h, secs, got / secs,
                last_ts > first_ts ? (got - 1) * 1e7 / double(last_ts - first_ts) : 0.0);
    source->Shutdown();
    return 0;
}
