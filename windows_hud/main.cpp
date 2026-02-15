#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <sapi.h>
#include <shellapi.h>
#include <winhttp.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "sapi.lib")
#pragma comment(lib, "winhttp.lib")

namespace {

constexpr UINT_PTR kPollTimerId = 1;
constexpr UINT kPollMs = 100;
constexpr float kFontSize = 14.0f;
constexpr float kMetaFontSize = 9.0f;
constexpr float kPadding = 20.0f;
constexpr float kWidth = 720.0f;
constexpr float kMinHeight = 140.0f;
constexpr float kMaxHeight = 420.0f;
constexpr float kTopMargin = 30.0f;
constexpr float kRightMargin = 30.0f;
constexpr float kCornerRadius = 14.0f;
constexpr BYTE kWindowOpacity = 217;  // ~85% of 255
constexpr int kAppIconResId = 1;
constexpr UINT kTrayCallbackMsg = WM_APP + 1;
constexpr UINT kTrayExitCommand = 1001;
constexpr UINT kTrayToggleSpeechCommand = 1002;
constexpr wchar_t kDefaultSubUrl[] = L"https://approximate.fit/sub";

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
    std::atomic<bool> stopSub{false};
    HICON appIcon = nullptr;
    ISpVoice* voice = nullptr;
    bool speechEnabled = true;
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

float ComputeHeightForText(AppState& s) {
    if (!s.dwriteFactory) {
        return kMinHeight;
    }

    IDWriteTextLayout* mainLayout = nullptr;
    IDWriteTextLayout* metaLayout = nullptr;

    float textAreaWidth = kWidth - (kPadding * 2.0f);

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
            metaHeight = metaMetrics.height + 6.0f;
        }
    }

    float h = (kPadding * 2.0f) + mainMetrics.height + metaHeight;
    if (h < kMinHeight) h = kMinHeight;
    if (h > kMaxHeight) h = kMaxHeight;

    SafeRelease(mainLayout);
    SafeRelease(metaLayout);
    return h;
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

    if (next == s.currentText) {
        return;
    }

    s.currentText = next;
    ParseMainAndMeta(s);
    SpeakLatestText(s, s.mainText);

    float h = ComputeHeightForText(s);
    MoveToTopRight(hwnd, kWidth, h);
    InvalidateRect(hwnd, nullptr, FALSE);
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

            float textAreaWidth = kWidth - (kPadding * 2.0f);
            D2D1_RECT_F mainRect = D2D1::RectF(kPadding, kPadding, kPadding + textAreaWidth, bounds.bottom - kPadding);
            s->rt->DrawTextW(
                s->mainText.c_str(),
                static_cast<UINT32>(s->mainText.size()),
                s->mainFormat,
                mainRect,
                s->fgBrush,
                D2D1_DRAW_TEXT_OPTIONS_CLIP,
                DWRITE_MEASURING_MODE_NATURAL);

            if (!s->metaText.empty()) {
                D2D1_RECT_F metaRect = D2D1::RectF(kPadding, bounds.bottom - kPadding - 24.0f, kPadding + textAreaWidth, bounds.bottom - 6.0f);
                s->rt->DrawTextW(
                    s->metaText.c_str(),
                    static_cast<UINT32>(s->metaText.size()),
                    s->metaFormat,
                    metaRect,
                    s->metaBrush,
                    D2D1_DRAW_TEXT_OPTIONS_CLIP,
                    DWRITE_MEASURING_MODE_NATURAL);
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

    float initialHeight = ComputeHeightForText(state);

    HWND hwnd = CreateWindowExW(
        exStyle,
        wc.lpszClassName,
        L"faeton",
        style,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        static_cast<int>(kWidth),
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

    SetLayeredWindowAttributes(hwnd, 0, kWindowOpacity, LWA_ALPHA);
    MoveToTopRight(hwnd, kWidth, initialHeight);

    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd);

    state.subThread = std::thread(SubscribeLoop, &state);
    RefreshTextIfChanged(hwnd, state);

    MSG msg{};
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    state.stopSub.store(true);
    if (state.subThread.joinable()) {
        state.subThread.join();
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
