#include <windows.h>
#include <d2d1.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dwrite.h>
#include <rpc.h>
#include <sapi.h>
#include <shellapi.h>
#include <winhttp.h>
#include <wincodec.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "rpcrt4.lib")
#pragma comment(lib, "sapi.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "windowsapp.lib")
#pragma comment(lib, "windowscodecs.lib")

namespace {

struct __declspec(uuid("A9B3D012-3DF2-4EE3-B8D1-8695F457D3C1")) IDirect3DDxgiInterfaceAccess : IUnknown {
    virtual HRESULT __stdcall GetInterface(REFIID iid, void** p) = 0;
};

constexpr UINT_PTR kPollTimerId = 1;
constexpr UINT kPollMs = 100;
constexpr float kFontSize = 24.0f;
constexpr float kMetaFontSize = 12.0f;
constexpr float kPadding = 10.0f;
constexpr float kMinWidth = 900.0f;
constexpr float kMaxWidth = 1700.0f;
constexpr float kMinHeight = 180.0f;
constexpr float kMaxHeight = 2000.0f;
constexpr float kTopMargin = 30.0f;
constexpr float kRightMargin = 30.0f;
constexpr float kCornerRadius = 14.0f;
constexpr BYTE kWindowOpacity = 217;  // ~85% of 255
constexpr int kAppIconResId = 1;
constexpr UINT kTrayCallbackMsg = WM_APP + 1;
constexpr UINT kTrayExitCommand = 1001;
constexpr UINT kTrayToggleSpeechCommand = 1002;
constexpr UINT kTrayRequireActiveCommand = 1003;
constexpr wchar_t kDefaultSubUrl[] = L"https://approximate.fit/sub";
constexpr wchar_t kUploadBaseUrl[] = L"https://approximate.fit";
constexpr wchar_t kCaptureTargetExe[] = L"dota2.exe";
constexpr int kCaptureIntervalMs = 5000;
constexpr int kDownsampleDivisor = 4;

struct AppState {
    std::wstring subUrl;

    ID2D1Factory* d2dFactory = nullptr;
    IDWriteFactory* dwriteFactory = nullptr;
    ID2D1HwndRenderTarget* rt = nullptr;
    ID2D1SolidColorBrush* fgBrush = nullptr;
    ID2D1SolidColorBrush* metaBrush = nullptr;
    ID2D1SolidColorBrush* bgBrush = nullptr;
    IDWriteTextFormat* mainFormat = nullptr;
    IDWriteTextFormat* metaFormat = nullptr;

    std::wstring currentText = L"Recording active.";
    std::wstring mainText = L"Recording active.";
    std::wstring metaText;
    std::wstring latestText = L"Recording active.";
    std::mutex textMutex;
    std::thread subThread;
    std::thread captureThread;
    std::atomic<bool> stopSub{false};
    std::atomic<bool> stopCapture{false};
    HICON appIcon = nullptr;
    ISpVoice* voice = nullptr;
    bool speechEnabled = true;
    std::atomic<bool> requireTargetActive{false};
};

void SafeRelease(IUnknown* p) {
    if (p) {
        p->Release();
    }
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) {
        return L"";
    }
    int needed = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (needed <= 0) {
        return L"";
    }
    std::wstring out(needed, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), needed);
    return out;
}

std::wstring Trim(const std::wstring& in) {
    size_t start = 0;
    while (start < in.size() && (in[start] == L' ' || in[start] == L'\n' || in[start] == L'\r' || in[start] == L'\t')) {
        ++start;
    }
    size_t end = in.size();
    while (end > start && (in[end - 1] == L' ' || in[end - 1] == L'\n' || in[end - 1] == L'\r' || in[end - 1] == L'\t')) {
        --end;
    }
    return in.substr(start, end - start);
}

std::wstring ToLower(const std::wstring& in) {
    std::wstring out = in;
    for (auto& ch : out) {
        ch = static_cast<wchar_t>(towlower(ch));
    }
    return out;
}

std::wstring BaseNameLower(const std::wstring& path) {
    size_t sep = path.find_last_of(L"\\/");
    std::wstring base = (sep == std::wstring::npos) ? path : path.substr(sep + 1);
    return ToLower(base);
}

void ParseMainAndMeta(AppState& s) {
    std::wstring trimmed = Trim(s.currentText);
    if (trimmed.empty()) {
        trimmed = L"Recording active.";
    }

    size_t lastNl = trimmed.find_last_of(L'\n');
    if (lastNl == std::wstring::npos) {
        s.mainText = trimmed;
        s.metaText.clear();
        return;
    }

    std::wstring candidateMeta = Trim(trimmed.substr(lastNl + 1));
    std::wstring lower = ToLower(candidateMeta);
    bool isMeta = lower.rfind(L"meta:", 0) == 0 || lower.rfind(L"step:", 0) == 0;
    if (!isMeta) {
        s.mainText = trimmed;
        s.metaText.clear();
        return;
    }

    s.mainText = Trim(trimmed.substr(0, lastNl));
    if (s.mainText.empty()) {
        s.mainText = L"Recording active.";
    }
    s.metaText = candidateMeta;
}

void MoveToTopRight(HWND hwnd, float width, float height) {
    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfo(mon, &mi);

    int x = mi.rcWork.right - static_cast<int>(width) - static_cast<int>(kRightMargin);
    int y = mi.rcWork.top + static_cast<int>(kTopMargin);
    SetWindowPos(hwnd, HWND_TOPMOST, x, y, static_cast<int>(width), static_cast<int>(height), SWP_NOACTIVATE);
}

float MaxHeightForMonitor(HWND hwnd) {
    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfo(mon, &mi);
    float available = static_cast<float>(mi.rcWork.bottom - mi.rcWork.top) - kTopMargin - 20.0f;
    if (available < kMinHeight) {
        return kMinHeight;
    }
    return available;
}

HRESULT EnsureDeviceResources(HWND hwnd, AppState& s) {
    if (s.rt) {
        return S_OK;
    }

    RECT rc{};
    GetClientRect(hwnd, &rc);
    D2D1_SIZE_U sz = D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top);

    HRESULT hr = s.d2dFactory->CreateHwndRenderTarget(
        D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_PREMULTIPLIED)),
        D2D1::HwndRenderTargetProperties(hwnd, sz),
        &s.rt);
    if (FAILED(hr)) {
        return hr;
    }
    // Keep D2D layout coordinates in pixel units to match Win32 window sizing math.
    s.rt->SetDpi(96.0f, 96.0f);

    hr = s.rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.95f), &s.fgBrush);
    if (FAILED(hr)) return hr;
    hr = s.rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.74f), &s.metaBrush);
    if (FAILED(hr)) return hr;
    hr = s.rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1.0f), &s.bgBrush);
    if (FAILED(hr)) return hr;
    return S_OK;
}

void DiscardDeviceResources(AppState& s) {
    SafeRelease(s.fgBrush);
    s.fgBrush = nullptr;
    SafeRelease(s.metaBrush);
    s.metaBrush = nullptr;
    SafeRelease(s.bgBrush);
    s.bgBrush = nullptr;
    SafeRelease(s.rt);
    s.rt = nullptr;
}

float ComputeHeightForText(AppState& s, float panelWidth) {
    if (!s.dwriteFactory) {
        return kMinHeight;
    }

    IDWriteTextLayout* mainLayout = nullptr;
    IDWriteTextLayout* metaLayout = nullptr;

    float textAreaWidth = panelWidth - (kPadding * 2.0f);

    HRESULT hr = s.dwriteFactory->CreateTextLayout(
        s.mainText.c_str(),
        static_cast<UINT32>(s.mainText.size()),
        s.mainFormat,
        textAreaWidth,
        3000.0f,
        &mainLayout);
    if (FAILED(hr)) {
        return kMinHeight;
    }

    DWRITE_TEXT_METRICS mainMetrics{};
    mainLayout->GetMetrics(&mainMetrics);
    DWRITE_OVERHANG_METRICS mainOverhang{};
    mainLayout->GetOverhangMetrics(&mainOverhang);
    float mainHeight = mainMetrics.height + mainOverhang.top + mainOverhang.bottom + 2.0f;

    float metaHeight = 0.0f;
    if (!s.metaText.empty()) {
        hr = s.dwriteFactory->CreateTextLayout(
            s.metaText.c_str(),
            static_cast<UINT32>(s.metaText.size()),
            s.metaFormat,
            textAreaWidth,
            1000.0f,
            &metaLayout);
        if (SUCCEEDED(hr)) {
            DWRITE_TEXT_METRICS metaMetrics{};
            metaLayout->GetMetrics(&metaMetrics);
            DWRITE_OVERHANG_METRICS metaOverhang{};
            metaLayout->GetOverhangMetrics(&metaOverhang);
            metaHeight = metaMetrics.height + metaOverhang.top + metaOverhang.bottom + 6.0f;
        }
    }

    float h = (kPadding * 2.0f) + mainHeight + metaHeight;
    if (h < kMinHeight) h = kMinHeight;
    if (h > kMaxHeight) h = kMaxHeight;

    SafeRelease(mainLayout);
    SafeRelease(metaLayout);
    return h;
}

float MaxWidthForMonitor(HWND hwnd) {
    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfo(mon, &mi);
    float available = static_cast<float>(mi.rcWork.right - mi.rcWork.left) - kRightMargin - 20.0f;
    if (available < kMinWidth) {
        return kMinWidth;
    }
    return available;
}

float ComputeDesiredWidth(HWND hwnd, AppState& s) {
    if (!s.dwriteFactory || !s.mainFormat) {
        return kMinWidth;
    }
    IDWriteTextLayout* mainLayout = nullptr;
    IDWriteTextLayout* metaLayout = nullptr;
    DWRITE_WORD_WRAPPING originalWrap = DWRITE_WORD_WRAPPING_WRAP;
    // Use no-wrap width measurement to pick a panel width, then restore wrapping.
    s.mainFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    HRESULT hr = s.dwriteFactory->CreateTextLayout(
        s.mainText.c_str(),
        static_cast<UINT32>(s.mainText.size()),
        s.mainFormat,
        4096.0f,
        3000.0f,
        &mainLayout);
    s.mainFormat->SetWordWrapping(originalWrap);
    if (FAILED(hr) || !mainLayout) {
        return kMinWidth;
    }
    DWRITE_TEXT_METRICS mainM{};
    mainLayout->GetMetrics(&mainM);
    DWRITE_OVERHANG_METRICS mainOv{};
    mainLayout->GetOverhangMetrics(&mainOv);
    float maxTextW = mainM.widthIncludingTrailingWhitespace + mainOv.left + mainOv.right;
    SafeRelease(mainLayout);

    if (!s.metaText.empty() && s.metaFormat) {
        DWRITE_WORD_WRAPPING metaWrap = DWRITE_WORD_WRAPPING_WRAP;
        s.metaFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        hr = s.dwriteFactory->CreateTextLayout(
            s.metaText.c_str(),
            static_cast<UINT32>(s.metaText.size()),
            s.metaFormat,
            4096.0f,
            1000.0f,
            &metaLayout);
        s.metaFormat->SetWordWrapping(metaWrap);
        if (SUCCEEDED(hr) && metaLayout) {
            DWRITE_TEXT_METRICS metaM{};
            metaLayout->GetMetrics(&metaM);
            DWRITE_OVERHANG_METRICS metaOv{};
            metaLayout->GetOverhangMetrics(&metaOv);
            float metaTextW = metaM.widthIncludingTrailingWhitespace + metaOv.left + metaOv.right;
            if (metaTextW > maxTextW) maxTextW = metaTextW;
        }
        SafeRelease(metaLayout);
    }

    float wanted = maxTextW + (kPadding * 2.0f) + 12.0f;
    float maxW = MaxWidthForMonitor(hwnd);
    if (wanted < kMinWidth) wanted = kMinWidth;
    if (wanted > kMaxWidth) wanted = kMaxWidth;
    if (wanted > maxW) wanted = maxW;
    return wanted;
}

bool ParseArgs(AppState& s) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) {
        return false;
    }

    if (argc >= 2 && argv[1] && argv[1][0] != L'\0') {
        s.subUrl = argv[1];
    } else {
        s.subUrl = kDefaultSubUrl;
    }

    LocalFree(argv);
    return !s.subUrl.empty();
}

bool AddTrayIcon(HWND hwnd, HICON icon) {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = kTrayCallbackMsg;
    nid.hIcon = icon ? icon : LoadIconW(nullptr, IDI_APPLICATION);
    lstrcpynW(nid.szTip, L"faeton", ARRAYSIZE(nid.szTip));
    return Shell_NotifyIconW(NIM_ADD, &nid) == TRUE;
}

void RemoveTrayIcon(HWND hwnd) {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

void ShowTrayMenu(HWND hwnd, AppState* state) {
    HMENU menu = CreatePopupMenu();
    if (!menu) {
        return;
    }
    UINT speakFlags = MF_STRING | ((state && state->speechEnabled) ? MF_CHECKED : MF_UNCHECKED);
    AppendMenuW(menu, speakFlags, kTrayToggleSpeechCommand, L"Speak");
    UINT activeFlags = MF_STRING | ((state && state->requireTargetActive.load()) ? MF_CHECKED : MF_UNCHECKED);
    AppendMenuW(menu, activeFlags, kTrayRequireActiveCommand, L"Require dota2.exe active");
    AppendMenuW(menu, MF_STRING, kTrayExitCommand, L"Exit");

    POINT pt{};
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

std::string TrimAscii(const std::string& in) {
    size_t start = 0;
    while (start < in.size() && (in[start] == ' ' || in[start] == '\n' || in[start] == '\r' || in[start] == '\t')) {
        ++start;
    }
    size_t end = in.size();
    while (end > start && (in[end - 1] == ' ' || in[end - 1] == '\n' || in[end - 1] == '\r' || in[end - 1] == '\t')) {
        --end;
    }
    return in.substr(start, end - start);
}

void ApplySseLine(
    const std::string& line,
    std::string& eventText,
    bool& hasText
) {
    if (line.rfind("data:", 0) != 0) {
        return;
    }
    std::string payload = TrimAscii(line.substr(5));
    size_t colon = payload.find(':');
    if (colon == std::string::npos) {
        return;
    }
    std::string key = TrimAscii(payload.substr(0, colon));
    std::string value = TrimAscii(payload.substr(colon + 1));
    if (key != "text") {
        return;
    }
    size_t pos = 0;
    while ((pos = value.find("\\n", pos)) != std::string::npos) {
        value.replace(pos, 2, "\n");
        pos += 1;
    }
    eventText = value;
    hasText = true;
}

void SetLatestText(AppState& s, const std::wstring& text) {
    std::lock_guard<std::mutex> lock(s.textMutex);
    s.latestText = text.empty() ? L"Recording active." : text;
}

void SpeakLatestText(AppState& s, const std::wstring& text) {
    if (!s.voice || !s.speechEnabled) {
        return;
    }
    std::wstring spoken = Trim(text);
    if (spoken.empty()) {
        return;
    }
    s.voice->Speak(spoken.c_str(), SPF_ASYNC | SPF_PURGEBEFORESPEAK, nullptr);
}

void StopSpeaking(AppState& s) {
    if (!s.voice) {
        return;
    }
    s.voice->Speak(L"", SPF_ASYNC | SPF_PURGEBEFORESPEAK, nullptr);
}

bool IsCaptureTargetActive() {
    HWND fg = GetForegroundWindow();
    if (!fg) {
        return false;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (!pid) {
        return false;
    }
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) {
        return false;
    }
    wchar_t pathBuf[MAX_PATH * 2] = {};
    DWORD pathLen = static_cast<DWORD>(std::size(pathBuf));
    bool ok = QueryFullProcessImageNameW(proc, 0, pathBuf, &pathLen) != FALSE;
    CloseHandle(proc);
    if (!ok) {
        return false;
    }
    std::wstring exe = BaseNameLower(pathBuf);
    return exe == kCaptureTargetExe;
}

HWND GetForegroundCaptureWindow(bool requireTargetExe) {
    HWND fg = GetForegroundWindow();
    if (!fg) {
        return nullptr;
    }
    if (!requireTargetExe) {
        return fg;
    }
    return IsCaptureTargetActive() ? fg : nullptr;
}

bool EncodeBgraToPngBytes(const BYTE* bgra, int width, int height, std::vector<BYTE>& out) {
    out.clear();
    if (!bgra || width <= 0 || height <= 0) {
        return false;
    }

    IWICImagingFactory* factory = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    IPropertyBag2* props = nullptr;
    IStream* stream = nullptr;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    bool success = false;

    if (FAILED(CoCreateInstance(
            CLSID_WICImagingFactory,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_IWICImagingFactory,
            reinterpret_cast<void**>(&factory)))) {
        goto done;
    }
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream))) {
        goto done;
    }
    if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder))) {
        goto done;
    }
    if (FAILED(encoder->Initialize(stream, WICBitmapEncoderNoCache))) {
        goto done;
    }
    if (FAILED(encoder->CreateNewFrame(&frame, &props))) {
        goto done;
    }
    if (FAILED(frame->Initialize(props))) {
        goto done;
    }
    if (FAILED(frame->SetSize(static_cast<UINT>(width), static_cast<UINT>(height)))) {
        goto done;
    }
    if (FAILED(frame->SetPixelFormat(&format))) {
        goto done;
    }
    UINT stride = static_cast<UINT>(width * 4);
    UINT dataSize = static_cast<UINT>(width * height * 4);
    if (FAILED(frame->WritePixels(static_cast<UINT>(height), stride, dataSize, const_cast<BYTE*>(bgra)))) {
        goto done;
    }
    if (FAILED(frame->Commit()) || FAILED(encoder->Commit())) {
        goto done;
    }

    HGLOBAL hGlobal = nullptr;
    if (FAILED(GetHGlobalFromStream(stream, &hGlobal)) || !hGlobal) {
        goto done;
    }
    SIZE_T sz = GlobalSize(hGlobal);
    if (sz == 0) {
        goto done;
    }
    void* mem = GlobalLock(hGlobal);
    if (!mem) {
        goto done;
    }
    out.assign(static_cast<BYTE*>(mem), static_cast<BYTE*>(mem) + sz);
    GlobalUnlock(hGlobal);
    success = true;

done:
    SafeRelease(props);
    SafeRelease(frame);
    SafeRelease(encoder);
    SafeRelease(factory);
    SafeRelease(stream);
    return success;
}

void DownsampleBgra4x(const BYTE* src, int srcW, int srcH, std::vector<BYTE>& dst, int& dstW, int& dstH) {
    dstW = max(1, srcW / kDownsampleDivisor);
    dstH = max(1, srcH / kDownsampleDivisor);
    dst.assign(static_cast<size_t>(dstW) * static_cast<size_t>(dstH) * 4, 0);
    for (int y = 0; y < dstH; ++y) {
        int sy = (y * srcH) / dstH;
        for (int x = 0; x < dstW; ++x) {
            int sx = (x * srcW) / dstW;
            const BYTE* p = src + ((static_cast<size_t>(sy) * static_cast<size_t>(srcW) + sx) * 4);
            BYTE* q = dst.data() + ((static_cast<size_t>(y) * static_cast<size_t>(dstW) + x) * 4);
            q[0] = p[0];
            q[1] = p[1];
            q[2] = p[2];
            q[3] = p[3];
        }
    }
}

bool CaptureWindowDownsampledPng(HWND hwnd, std::vector<BYTE>& pngBytes) {
    if (!hwnd) {
        return false;
    }

    winrt::com_ptr<ID3D11Device> d3dDevice;
    winrt::com_ptr<ID3D11DeviceContext> d3dContext;
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr,
        0,
        D3D11_SDK_VERSION,
        d3dDevice.put(),
        &fl,
        d3dContext.put());
    if (FAILED(hr)) {
        return false;
    }

    winrt::com_ptr<IDXGIDevice> dxgiDevice;
    if (FAILED(d3dDevice->QueryInterface(__uuidof(IDXGIDevice), dxgiDevice.put_void()))) {
        return false;
    }
    winrt::com_ptr<IInspectable> inspectable;
    if (FAILED(CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.get(), inspectable.put()))) {
        return false;
    }
    auto winrtDevice = inspectable.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();

    auto interop = winrt::get_activation_factory<
        winrt::Windows::Graphics::Capture::GraphicsCaptureItem,
        IGraphicsCaptureItemInterop>();
    winrt::Windows::Graphics::Capture::GraphicsCaptureItem item{nullptr};
    if (FAILED(interop->CreateForWindow(hwnd, winrt::guid_of<winrt::Windows::Graphics::Capture::GraphicsCaptureItem>(), winrt::put_abi(item)))) {
        return false;
    }
    auto size = item.Size();
    if (size.Width <= 0 || size.Height <= 0) {
        return false;
    }

    auto pool = winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool::CreateFreeThreaded(
        winrtDevice,
        winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
        1,
        size);
    auto session = pool.CreateCaptureSession(item);
    session.IsBorderRequired(false);
    session.IsCursorCaptureEnabled(false);

    HANDLE frameEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!frameEvent) {
        session.Close();
        pool.Close();
        return false;
    }
    winrt::event_token token = pool.FrameArrived([&](auto&, auto&) {
        SetEvent(frameEvent);
    });

    session.StartCapture();
    DWORD waitRc = WaitForSingleObject(frameEvent, 2000);
    CloseHandle(frameEvent);
    pool.FrameArrived(token);
    if (waitRc != WAIT_OBJECT_0) {
        session.Close();
        pool.Close();
        return false;
    }

    auto frame = pool.TryGetNextFrame();
    if (!frame) {
        session.Close();
        pool.Close();
        return false;
    }

    auto surface = frame.Surface();
    auto unk = surface.as<IUnknown>();
    winrt::com_ptr<IDirect3DDxgiInterfaceAccess> access;
    if (FAILED(unk->QueryInterface(__uuidof(IDirect3DDxgiInterfaceAccess), access.put_void()))) {
        session.Close();
        pool.Close();
        return false;
    }
    winrt::com_ptr<ID3D11Texture2D> gpuTex;
    if (FAILED(access->GetInterface(__uuidof(ID3D11Texture2D), gpuTex.put_void()))) {
        session.Close();
        pool.Close();
        return false;
    }

    D3D11_TEXTURE2D_DESC desc{};
    gpuTex->GetDesc(&desc);
    D3D11_TEXTURE2D_DESC staging = desc;
    staging.BindFlags = 0;
    staging.MiscFlags = 0;
    staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging.Usage = D3D11_USAGE_STAGING;
    winrt::com_ptr<ID3D11Texture2D> cpuTex;
    if (FAILED(d3dDevice->CreateTexture2D(&staging, nullptr, cpuTex.put()))) {
        session.Close();
        pool.Close();
        return false;
    }
    d3dContext->CopyResource(cpuTex.get(), gpuTex.get());

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(d3dContext->Map(cpuTex.get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        session.Close();
        pool.Close();
        return false;
    }

    int srcW = static_cast<int>(desc.Width);
    int srcH = static_cast<int>(desc.Height);
    std::vector<BYTE> srcPixels(static_cast<size_t>(srcW) * static_cast<size_t>(srcH) * 4);
    for (int y = 0; y < srcH; ++y) {
        const BYTE* row = static_cast<const BYTE*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch;
        memcpy(srcPixels.data() + static_cast<size_t>(y) * static_cast<size_t>(srcW) * 4, row, static_cast<size_t>(srcW) * 4);
    }
    d3dContext->Unmap(cpuTex.get(), 0);

    session.Close();
    pool.Close();

    std::vector<BYTE> down;
    int dstW = 0;
    int dstH = 0;
    DownsampleBgra4x(srcPixels.data(), srcW, srcH, down, dstW, dstH);
    return EncodeBgraToPngBytes(down.data(), dstW, dstH, pngBytes);
}

std::string NewUuidV1Filename() {
    UUID id{};
    if (UuidCreateSequential(&id) != RPC_S_OK && UuidCreate(&id) != RPC_S_OK) {
        return "";
    }
    RPC_CSTR s = nullptr;
    if (UuidToStringA(&id, &s) != RPC_S_OK || !s) {
        return "";
    }
    std::string out(reinterpret_cast<char*>(s));
    RpcStringFreeA(&s);
    out += ".png";
    return out;
}

bool UploadPng(const std::vector<BYTE>& pngBytes, const std::string& filename) {
    if (pngBytes.empty() || filename.empty()) {
        return false;
    }

    std::wstring fullUrl = std::wstring(kUploadBaseUrl) + L"/png/" + Utf8ToWide(filename);
    URL_COMPONENTSW parts{};
    wchar_t host[256] = {};
    wchar_t path[2048] = {};
    parts.dwStructSize = sizeof(parts);
    parts.lpszHostName = host;
    parts.dwHostNameLength = static_cast<DWORD>(std::size(host));
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = static_cast<DWORD>(std::size(path));
    parts.dwSchemeLength = 1;
    if (!WinHttpCrackUrl(fullUrl.c_str(), 0, 0, &parts)) {
        return false;
    }

    std::wstring hostName(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring pathName(parts.lpszUrlPath, parts.dwUrlPathLength);
    DWORD flags = (parts.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;

    HINTERNET session = WinHttpOpen(L"faeton/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        return false;
    }
    WinHttpSetTimeouts(session, 5000, 5000, 10000, 15000);
    HINTERNET connect = WinHttpConnect(session, hostName.c_str(), parts.nPort, 0);
    if (!connect) {
        WinHttpCloseHandle(session);
        return false;
    }
    HINTERNET request = WinHttpOpenRequest(connect, L"POST", pathName.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!request) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return false;
    }

    BOOL ok = WinHttpSendRequest(
        request,
        L"Content-Type: image/png\r\n",
        -1L,
        const_cast<BYTE*>(pngBytes.data()),
        static_cast<DWORD>(pngBytes.size()),
        static_cast<DWORD>(pngBytes.size()),
        0);
    if (ok) {
        ok = WinHttpReceiveResponse(request, nullptr);
    }
    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    if (ok) {
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX);
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return ok && status >= 200 && status < 300;
}

void CaptureLoop(AppState* state) {
    while (!state->stopCapture.load()) {
        HWND target = GetForegroundCaptureWindow(state->requireTargetActive.load());
        if (target) {
            std::vector<BYTE> pngBytes;
            if (CaptureWindowDownsampledPng(target, pngBytes)) {
                std::string filename = NewUuidV1Filename();
                UploadPng(pngBytes, filename);
            }
        }
        for (int waited = 0; waited < kCaptureIntervalMs && !state->stopCapture.load(); waited += 100) {
            Sleep(100);
        }
    }
}

void SubscribeLoop(AppState* state) {
    URL_COMPONENTSW parts{};
    wchar_t host[256] = {};
    wchar_t path[2048] = {};
    parts.dwStructSize = sizeof(parts);
    parts.lpszHostName = host;
    parts.dwHostNameLength = static_cast<DWORD>(std::size(host));
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = static_cast<DWORD>(std::size(path));
    parts.dwSchemeLength = 1;

    if (!WinHttpCrackUrl(state->subUrl.c_str(), 0, 0, &parts)) {
        SetLatestText(*state, L"sub error: invalid URL");
        return;
    }

    std::wstring hostName(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring pathName(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (pathName.empty()) {
        pathName = L"/";
    }
    DWORD flags = (parts.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;

    HINTERNET session = WinHttpOpen(L"faeton/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        SetLatestText(*state, L"sub error: WinHttpOpen failed");
        return;
    }
    WinHttpSetTimeouts(session, 5000, 5000, 5000, 30000);

    while (!state->stopSub.load()) {
        HINTERNET connect = WinHttpConnect(session, hostName.c_str(), parts.nPort, 0);
        if (!connect) {
            SetLatestText(*state, L"sub: reconnecting...");
            Sleep(1000);
            continue;
        }

        HINTERNET request = WinHttpOpenRequest(connect, L"GET", pathName.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (!request) {
            WinHttpCloseHandle(connect);
            SetLatestText(*state, L"sub: reconnecting...");
            Sleep(1000);
            continue;
        }

        WinHttpAddRequestHeaders(request, L"Accept: text/event-stream\r\nCache-Control: no-cache\r\n", -1, WINHTTP_ADDREQ_FLAG_ADD);
        BOOL ok = WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
        if (ok) ok = WinHttpReceiveResponse(request, nullptr);
        if (!ok) {
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connect);
            SetLatestText(*state, L"sub: reconnecting...");
            Sleep(1000);
            continue;
        }

        std::string buf;
        std::string line;
        std::string eventText;
        bool hasText = false;
        while (!state->stopSub.load()) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(request, &avail)) {
                break;
            }
            if (avail == 0) {
                break;
            }
            std::string chunk(avail, '\0');
            DWORD read = 0;
            if (!WinHttpReadData(request, chunk.data(), avail, &read) || read == 0) {
                break;
            }
            chunk.resize(read);
            buf.append(chunk);

            size_t nl = 0;
            while ((nl = buf.find('\n')) != std::string::npos) {
                line = buf.substr(0, nl);
                buf.erase(0, nl + 1);
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                if (line.empty()) {
                    if (hasText) {
                        SetLatestText(*state, Utf8ToWide(eventText));
                    }
                    eventText.clear();
                    hasText = false;
                    continue;
                }
                ApplySseLine(line, eventText, hasText);
            }
        }

        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        if (!state->stopSub.load()) {
            Sleep(500);
        }
    }

    WinHttpCloseHandle(session);
}

void RefreshTextIfChanged(HWND hwnd, AppState& s) {
    std::wstring next;
    {
        std::lock_guard<std::mutex> lock(s.textMutex);
        next = s.latestText;
    }
    next = Trim(next);
    if (next.empty()) {
        next = L"Recording active.";
    }

    bool changed = (next != s.currentText);
    if (changed) {
        s.currentText = next;
        ParseMainAndMeta(s);
        SpeakLatestText(s, s.mainText);
    }

    float w = ComputeDesiredWidth(hwnd, s);
    float h = ComputeHeightForText(s, w);
    float monitorMax = MaxHeightForMonitor(hwnd);
    if (h > monitorMax) {
        h = monitorMax;
    }

    RECT rc{};
    GetWindowRect(hwnd, &rc);
    int currentW = rc.right - rc.left;
    int currentH = rc.bottom - rc.top;
    int desiredW = static_cast<int>(w);
    int desiredH = static_cast<int>(h);
    if (currentW != desiredW || currentH != desiredH || changed) {
        MoveToTopRight(hwnd, w, h);
        InvalidateRect(hwnd, nullptr, FALSE);
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    AppState* s = reinterpret_cast<AppState*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));

    switch (msg) {
        case WM_NCCREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCT*>(lParam);
            SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            return DefWindowProc(hwnd, msg, wParam, lParam);
        }
        case WM_CREATE: {
            SetTimer(hwnd, kPollTimerId, kPollMs, nullptr);
            if (s) {
                AddTrayIcon(hwnd, s->appIcon);
            }
            return 0;
        }
        case kTrayCallbackMsg: {
            if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) {
                ShowTrayMenu(hwnd, s);
            } else if (lParam == WM_LBUTTONDBLCLK) {
                DestroyWindow(hwnd);
            }
            return 0;
        }
        case WM_COMMAND: {
            if (LOWORD(wParam) == kTrayToggleSpeechCommand && s) {
                s->speechEnabled = !s->speechEnabled;
                if (!s->speechEnabled) {
                    StopSpeaking(*s);
                }
                return 0;
            }
            if (LOWORD(wParam) == kTrayRequireActiveCommand && s) {
                s->requireTargetActive.store(!s->requireTargetActive.load());
                return 0;
            }
            if (LOWORD(wParam) == kTrayExitCommand) {
                DestroyWindow(hwnd);
                return 0;
            }
            return DefWindowProc(hwnd, msg, wParam, lParam);
        }
        case WM_LBUTTONDOWN: {
            if (s) {
                StopSpeaking(*s);
            }
            return 0;
        }
        case WM_TIMER: {
            if (s && wParam == kPollTimerId) {
                RefreshTextIfChanged(hwnd, *s);
            }
            return 0;
        }
        case WM_SIZE: {
            if (s && s->rt) {
                UINT w = static_cast<UINT>(LOWORD(lParam));
                UINT h = static_cast<UINT>(HIWORD(lParam));
                if (w > 0 && h > 0) {
                    s->rt->Resize(D2D1::SizeU(w, h));
                }
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            if (!s) return 0;
            PAINTSTRUCT ps{};
            BeginPaint(hwnd, &ps);

            if (FAILED(EnsureDeviceResources(hwnd, *s))) {
                EndPaint(hwnd, &ps);
                return 0;
            }

            RECT rc{};
            GetClientRect(hwnd, &rc);
            D2D1_RECT_F bounds = D2D1::RectF(0, 0, static_cast<float>(rc.right), static_cast<float>(rc.bottom));

            s->rt->BeginDraw();
            s->rt->Clear(D2D1::ColorF(0, 0, 0, 0));
            s->rt->FillRoundedRectangle(
                D2D1::RoundedRect(bounds, kCornerRadius, kCornerRadius),
                s->bgBrush);

            float textAreaWidth = bounds.right - bounds.left - (kPadding * 2.0f);
            float metaReserve = 0.0f;
            if (!s->metaText.empty()) {
                IDWriteTextLayout* metaMeasure = nullptr;
                HRESULT mmhr = s->dwriteFactory->CreateTextLayout(
                    s->metaText.c_str(),
                    static_cast<UINT32>(s->metaText.size()),
                    s->metaFormat,
                    textAreaWidth,
                    100.0f,
                    &metaMeasure);
                if (SUCCEEDED(mmhr) && metaMeasure) {
                    DWRITE_TEXT_METRICS mm{};
                    metaMeasure->GetMetrics(&mm);
                    DWRITE_OVERHANG_METRICS mo{};
                    metaMeasure->GetOverhangMetrics(&mo);
                    metaReserve = mm.height + mo.top + mo.bottom + 6.0f;
                }
                SafeRelease(metaMeasure);
            }
            IDWriteTextLayout* mainLayout = nullptr;
            HRESULT lhr = s->dwriteFactory->CreateTextLayout(
                s->mainText.c_str(),
                static_cast<UINT32>(s->mainText.size()),
                s->mainFormat,
                textAreaWidth,
                bounds.bottom - (kPadding * 2.0f) - metaReserve,
                &mainLayout);
            if (SUCCEEDED(lhr) && mainLayout) {
                s->rt->DrawTextLayout(
                    D2D1::Point2F(kPadding, kPadding),
                    mainLayout,
                    s->fgBrush,
                    D2D1_DRAW_TEXT_OPTIONS_CLIP);
                SafeRelease(mainLayout);
            }

            if (!s->metaText.empty()) {
                IDWriteTextLayout* metaLayout = nullptr;
                float metaMaxHeight = 40.0f;
                HRESULT mhr = s->dwriteFactory->CreateTextLayout(
                    s->metaText.c_str(),
                    static_cast<UINT32>(s->metaText.size()),
                    s->metaFormat,
                    textAreaWidth,
                    metaMaxHeight,
                    &metaLayout);
                if (SUCCEEDED(mhr) && metaLayout) {
                    DWRITE_TEXT_METRICS metaMetrics{};
                    metaLayout->GetMetrics(&metaMetrics);
                    float metaY = bounds.bottom - kPadding - metaMetrics.height;
                    s->rt->DrawTextLayout(
                        D2D1::Point2F(kPadding, metaY),
                        metaLayout,
                        s->metaBrush,
                        D2D1_DRAW_TEXT_OPTIONS_CLIP);
                    SafeRelease(metaLayout);
                }
            }

            HRESULT hr = s->rt->EndDraw();
            if (hr == D2DERR_RECREATE_TARGET) {
                DiscardDeviceResources(*s);
            }

            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_DESTROY: {
            KillTimer(hwnd, kPollTimerId);
            RemoveTrayIcon(hwnd);
            PostQuitMessage(0);
            return 0;
        }
        default:
            return DefWindowProc(hwnd, msg, wParam, lParam);
    }
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int) {
    HRESULT comHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    AppState state;
    if (!ParseArgs(state)) {
        MessageBoxW(nullptr, L"Failed to initialize subscribe URL.", L"faeton", MB_ICONERROR);
        if (SUCCEEDED(comHr)) {
            CoUninitialize();
        }
        return 2;
    }

    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &state.d2dFactory);
    if (FAILED(hr)) {
        return 1;
    }
    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(&state.dwriteFactory));
    if (FAILED(hr)) {
        SafeRelease(state.d2dFactory);
        return 1;
    }

    hr = state.dwriteFactory->CreateTextFormat(
        L"Consolas",
        nullptr,
        DWRITE_FONT_WEIGHT_SEMI_BOLD,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        kFontSize,
        L"en-us",
        &state.mainFormat);
    if (FAILED(hr)) {
        SafeRelease(state.dwriteFactory);
        SafeRelease(state.d2dFactory);
        return 1;
    }
    state.mainFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    state.mainFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    state.mainFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);

    hr = state.dwriteFactory->CreateTextFormat(
        L"Consolas",
        nullptr,
        DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        kMetaFontSize,
        L"en-us",
        &state.metaFormat);
    if (FAILED(hr)) {
        SafeRelease(state.mainFormat);
        SafeRelease(state.dwriteFactory);
        SafeRelease(state.d2dFactory);
        return 1;
    }
    state.metaFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    state.metaFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    state.metaFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_ALL, IID_ISpVoice, reinterpret_cast<void**>(&state.voice));
    if (state.voice) {
        state.voice->SetRate(5);
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"FaetonHudWindow";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    state.appIcon = static_cast<HICON>(LoadImageW(
        hInstance, MAKEINTRESOURCEW(kAppIconResId), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE));
    wc.hIcon = state.appIcon ? state.appIcon : LoadIconW(nullptr, IDI_APPLICATION);
    wc.hIconSm = state.appIcon ? state.appIcon : LoadIconW(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);

    DWORD exStyle = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_NOACTIVATE;
    DWORD style = WS_POPUP;

    float initialWidth = kMinWidth;
    float initialHeight = ComputeHeightForText(state, initialWidth);

    HWND hwnd = CreateWindowExW(
        exStyle,
        wc.lpszClassName,
        L"faeton",
        style,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        static_cast<int>(initialWidth),
        static_cast<int>(initialHeight),
        nullptr,
        nullptr,
        hInstance,
        &state);

    if (!hwnd) {
        SafeRelease(state.metaFormat);
        SafeRelease(state.mainFormat);
        SafeRelease(state.dwriteFactory);
        SafeRelease(state.d2dFactory);
        return 1;
    }

    initialWidth = ComputeDesiredWidth(hwnd, state);
    initialHeight = ComputeHeightForText(state, initialWidth);
    float monitorMax = MaxHeightForMonitor(hwnd);
    if (initialHeight > monitorMax) {
        initialHeight = monitorMax;
    }
    SetLayeredWindowAttributes(hwnd, 0, kWindowOpacity, LWA_ALPHA);
    MoveToTopRight(hwnd, initialWidth, initialHeight);

    RECT clientRc{};
    RECT winRc{};
    GetClientRect(hwnd, &clientRc);
    GetWindowRect(hwnd, &winRc);
    std::printf(
        "faeton hud size: kWidth=%.1f client=%ldx%ld window=%ldx%ld\n",
        initialWidth,
        static_cast<long>(clientRc.right - clientRc.left),
        static_cast<long>(clientRc.bottom - clientRc.top),
        static_cast<long>(winRc.right - winRc.left),
        static_cast<long>(winRc.bottom - winRc.top));
    std::fflush(stdout);

    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd);

    state.subThread = std::thread(SubscribeLoop, &state);
    state.captureThread = std::thread(CaptureLoop, &state);
    RefreshTextIfChanged(hwnd, state);

    MSG msg{};
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    state.stopSub.store(true);
    state.stopCapture.store(true);
    if (state.subThread.joinable()) {
        state.subThread.join();
    }
    if (state.captureThread.joinable()) {
        state.captureThread.join();
    }

    DiscardDeviceResources(state);
    SafeRelease(state.metaFormat);
    SafeRelease(state.mainFormat);
    SafeRelease(state.dwriteFactory);
    SafeRelease(state.d2dFactory);
    if (state.appIcon) {
        DestroyIcon(state.appIcon);
        state.appIcon = nullptr;
    }
    SafeRelease(state.voice);
    state.voice = nullptr;
    if (SUCCEEDED(comHr)) {
        CoUninitialize();
    }

    return static_cast<int>(msg.wParam);
}
