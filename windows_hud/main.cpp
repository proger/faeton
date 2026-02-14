#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <shellapi.h>

#include <chrono>
#include <fstream>
#include <sstream>
#include <string>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

namespace {

constexpr UINT_PTR kPollTimerId = 1;
constexpr UINT kPollMs = 100;
constexpr float kFontSize = 24.0f;
constexpr float kMetaFontSize = 15.0f;
constexpr float kPadding = 20.0f;
constexpr float kWidth = 720.0f;
constexpr float kMinHeight = 140.0f;
constexpr float kMaxHeight = 420.0f;
constexpr float kTopMargin = 30.0f;
constexpr float kRightMargin = 30.0f;
constexpr float kCornerRadius = 14.0f;
constexpr BYTE kWindowOpacity = 217;  // ~85% of 255

struct AppState {
    std::wstring textFile;

    ID2D1Factory* d2dFactory = nullptr;
    IDWriteFactory* dwriteFactory = nullptr;
    ID2D1HwndRenderTarget* rt = nullptr;
    ID2D1SolidColorBrush* fgBrush = nullptr;
    ID2D1SolidColorBrush* metaBrush = nullptr;
    ID2D1SolidColorBrush* bgBrush = nullptr;
    IDWriteTextFormat* mainFormat = nullptr;
    IDWriteTextFormat* metaFormat = nullptr;

    std::wstring currentText = L"Recording active. Waiting for chunk advice...";
    std::wstring mainText = L"Recording active. Waiting for chunk advice...";
    std::wstring metaText;
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

bool ReadFileUtf8(const std::wstring& path, std::wstring& outText) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) {
        return false;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string data = ss.str();

    if (data.size() >= 3 &&
        static_cast<unsigned char>(data[0]) == 0xEF &&
        static_cast<unsigned char>(data[1]) == 0xBB &&
        static_cast<unsigned char>(data[2]) == 0xBF) {
        data = data.substr(3);
    }

    outText = Utf8ToWide(data);
    return true;
}

void ParseMainAndMeta(AppState& s) {
    std::wstring trimmed = Trim(s.currentText);
    if (trimmed.empty()) {
        trimmed = L"Recording active. Waiting for chunk advice...";
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
        s.mainText = L"Recording active. Waiting for chunk advice...";
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

    if (argc >= 2) {
        s.textFile = argv[1];
    }

    LocalFree(argv);
    return !s.textFile.empty();
}

void RefreshTextIfChanged(HWND hwnd, AppState& s) {
    std::wstring next;
    if (!ReadFileUtf8(s.textFile, next)) {
        return;
    }

    next = Trim(next);
    if (next.empty()) {
        next = L"Recording active. Waiting for chunk advice...";
    }

    if (next == s.currentText) {
        return;
    }

    s.currentText = next;
    ParseMainAndMeta(s);

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
            PostQuitMessage(0);
            return 0;
        }
        default:
            return DefWindowProc(hwnd, msg, wParam, lParam);
    }
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int) {
    AppState state;
    if (!ParseArgs(state)) {
        MessageBoxW(nullptr, L"Usage: faeton.exe <text-file>", L"faeton", MB_ICONERROR);
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

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"FaetonHudWindow";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    DWORD exStyle = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE;
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

    RefreshTextIfChanged(hwnd, state);

    MSG msg{};
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    DiscardDeviceResources(state);
    SafeRelease(state.metaFormat);
    SafeRelease(state.mainFormat);
    SafeRelease(state.dwriteFactory);
    SafeRelease(state.d2dFactory);

    return static_cast<int>(msg.wParam);
}
