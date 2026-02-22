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
#include <commctrl.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Security.Authorization.AppCapabilityAccess.h>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
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
constexpr float kFontSize = 14.0f;
constexpr float kMetaFontSize = 12.0f;
constexpr float kInputFontSize = kFontSize;
constexpr float kFontStep = 1.0f;
constexpr float kMinMainFontSize = 10.0f;
constexpr float kMaxMainFontSize = 42.0f;
constexpr float kPadding = 10.0f;
constexpr float kMinWidth = 300.0f;
constexpr float kMaxWidth = 567.0f;
constexpr float kMinHeight = 180.0f;
constexpr float kMaxHeight = 2000.0f;
constexpr float kTopMargin = 30.0f;
constexpr float kRightMargin = 30.0f;
constexpr float kCornerRadius = 14.0f;
constexpr float kBaseWindowWidth = kMaxWidth;
constexpr float kBaseWindowHeight = 340.0f;
constexpr float kFontResizeThreshold = 24.0f;
constexpr float kFontGrowWidthPerPoint = 56.0f;
constexpr float kFontGrowHeightPerPoint = 10.0f;
constexpr float kInputMinHeight = 24.0f;
constexpr float kInputGap = 6.0f;
constexpr float kInputSidePadding = 8.0f;
constexpr float kInputBottomPadding = 8.0f;
constexpr float kInputVerticalPadding = 10.0f;
constexpr float kWheelStepPx = 36.0f;
constexpr BYTE kWindowOpacity = 217;  // ~85% of 255
constexpr int kAppIconResId = 1;
constexpr UINT kTrayCallbackMsg = WM_APP + 1;
constexpr UINT kSubmitInputMsg = WM_APP + 2;
constexpr UINT kAdjustFontMsg = WM_APP + 3;
constexpr UINT kTrayExitCommand = 1001;
constexpr UINT kTrayToggleSpeechCommand = 1002;
constexpr UINT kTrayRequireActiveCommand = 1003;
constexpr UINT kTrayToggleVisibilityCommand = 1004;
constexpr wchar_t kDefaultSubUrl[] = L"https://approximate.fit/sub/0";
constexpr wchar_t kDefaultPubUrl[] = L"https://approximate.fit/pub";
constexpr wchar_t kUploadBaseUrl[] = L"https://approximate.fit";
constexpr wchar_t kCaptureTargetExe[] = L"dota2.exe";
constexpr int kCaptureIntervalMs = 5000;
constexpr int kDownsampleDivisor = 4;
constexpr COLORREF kInputTextColor = RGB(242, 242, 242);
constexpr COLORREF kInputBgColor = RGB(0, 0, 0);

struct LogLine {
    std::wstring hhmmss;
    std::wstring text;
};

struct AppState {
    std::wstring subUrl;
    std::wstring pubUrl;
    std::wstring inputFilePath;
    std::wstring outputFilePath;
    bool singlePlayerMode = false;

    ID2D1Factory* d2dFactory = nullptr;
    IDWriteFactory* dwriteFactory = nullptr;
    ID2D1HwndRenderTarget* rt = nullptr;
    ID2D1SolidColorBrush* fgBrush = nullptr;
    ID2D1SolidColorBrush* metaBrush = nullptr;
    ID2D1SolidColorBrush* bgBrush = nullptr;
    IDWriteTextFormat* mainFormat = nullptr;
    IDWriteTextFormat* metaFormat = nullptr;
    IDWriteTextFormat* tsFormat = nullptr;

    std::wstring currentText = L"Recording active.";
    std::wstring mainText = L"Recording active.";
    std::wstring metaText;
    std::wstring latestText = L"Recording active.";
    std::wstring lastPolledInputText;
    std::deque<LogLine> logLines;
    uint64_t dataVersion = 0;
    uint64_t renderedVersion = 0;
    HWND inputEdit = nullptr;
    WNDPROC inputEditOldProc = nullptr;
    HFONT inputFont = nullptr;
    bool ownsInputFont = false;
    int inputTextHeightPx = 0;
    float mainFontSize = kFontSize;
    float inputFontSize = kInputFontSize;
    HBRUSH inputBgBrush = nullptr;
    float scrollOffsetPx = 0.0f;
    float maxScrollOffsetPx = 0.0f;
    int wheelRemainder = 0;
    std::mutex textMutex;
    std::thread subThread;
    std::thread captureThread;
    std::atomic<bool> stopSub{false};
    std::atomic<bool> stopCapture{false};
    bool borderlessCaptureAllowed = false;
    HICON appIcon = nullptr;
    ISpVoice* voice = nullptr;
    bool speechEnabled = false;
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

std::string WideToUtf8(const std::wstring& ws) {
    if (ws.empty()) {
        return "";
    }
    int needed = WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        return "";
    }
    std::string out(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()), out.data(), needed, nullptr, nullptr);
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

    int x = mi.rcWork.left + static_cast<int>(kRightMargin);
    int y = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - static_cast<int>(height)) / 2;
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

void PrintUsage() {
    std::fwprintf(
        stderr,
        L"Usage: faeton.exe [-i <input-file>] [-o <output-file>] [-h|--help]\n\n"
        L"  -i <input-file>   Read overlay text from a local file (single-player mode)\n"
        L"  -o <output-file>  Append 'ask:' submissions to this local file\n"
        L"  (no -i)           Multiplayer mode: read live updates from https://approximate.fit/sub/0\n"
        L"  -h, --help        Show this help\n");
}

enum class ParseArgsResult {
    Ok,
    Help,
    Error,
};

ParseArgsResult ParseArgs(AppState& s) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) {
        std::fwprintf(stderr, L"error: failed to parse command line\n");
        PrintUsage();
        return ParseArgsResult::Error;
    }

    s.subUrl = kDefaultSubUrl;
    s.pubUrl = kDefaultPubUrl;

    ParseArgsResult result = ParseArgsResult::Ok;
    for (int i = 1; i < argc; ++i) {
        std::wstring arg = argv[i] ? argv[i] : L"";
        if (arg == L"-h" || arg == L"--help") {
            PrintUsage();
            result = ParseArgsResult::Help;
            break;
        }
        if (arg == L"-i") {
            if (i + 1 >= argc || !argv[i + 1] || argv[i + 1][0] == L'\0') {
                std::fwprintf(stderr, L"error: missing value for -i\n");
                PrintUsage();
                result = ParseArgsResult::Error;
                break;
            }
            s.inputFilePath = argv[++i];
            continue;
        }
        if (arg == L"-o") {
            if (i + 1 >= argc || !argv[i + 1] || argv[i + 1][0] == L'\0') {
                std::fwprintf(stderr, L"error: missing value for -o\n");
                PrintUsage();
                result = ParseArgsResult::Error;
                break;
            }
            s.outputFilePath = argv[++i];
            continue;
        }
        std::fwprintf(stderr, L"error: unrecognized argument: %ls\n", arg.c_str());
        PrintUsage();
        result = ParseArgsResult::Error;
        break;
    }

    LocalFree(argv);
    if (result != ParseArgsResult::Ok) {
        return result;
    }

    s.singlePlayerMode = !s.inputFilePath.empty();
    if (s.singlePlayerMode && s.outputFilePath.empty()) {
        std::filesystem::path in(s.inputFilePath);
        s.outputFilePath = (in.parent_path() / "_pub.txt").wstring();
    }
    return ParseArgsResult::Ok;
}

bool RebuildTextFormats(AppState& s) {
    if (!s.dwriteFactory) {
        return false;
    }

    SafeRelease(s.mainFormat);
    s.mainFormat = nullptr;
    SafeRelease(s.metaFormat);
    s.metaFormat = nullptr;
    SafeRelease(s.tsFormat);
    s.tsFormat = nullptr;

    HRESULT hr = s.dwriteFactory->CreateTextFormat(
        L"Consolas",
        nullptr,
        DWRITE_FONT_WEIGHT_SEMI_BOLD,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        s.mainFontSize,
        L"en-us",
        &s.mainFormat);
    if (FAILED(hr) || !s.mainFormat) {
        return false;
    }
    s.mainFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    s.mainFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    s.mainFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);

    float metaSize = (s.mainFontSize - 2.0f < 8.0f) ? 8.0f : (s.mainFontSize - 2.0f);
    hr = s.dwriteFactory->CreateTextFormat(
        L"Consolas",
        nullptr,
        DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        metaSize,
        L"en-us",
        &s.metaFormat);
    if (FAILED(hr) || !s.metaFormat) {
        return false;
    }
    s.metaFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    s.metaFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    s.metaFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);

    hr = s.dwriteFactory->CreateTextFormat(
        L"Consolas",
        nullptr,
        DWRITE_FONT_WEIGHT_SEMI_BOLD,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        s.mainFontSize,
        L"en-us",
        &s.tsFormat);
    if (FAILED(hr) || !s.tsFormat) {
        return false;
    }
    s.tsFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    s.tsFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    s.tsFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    return true;
}

void ApplyInputFont(AppState& s) {
    if (s.ownsInputFont && s.inputFont) {
        DeleteObject(s.inputFont);
        s.inputFont = nullptr;
        s.ownsInputFont = false;
    }
    if (!s.inputEdit) {
        return;
    }
    UINT dpi = GetDpiForWindow(s.inputEdit);
    if (dpi == 0) dpi = 96;
    // Match DWrite output sizing (DIP at 96) so input/output track together.
    int height = -MulDiv(static_cast<int>(std::lround(s.inputFontSize)), static_cast<int>(dpi), 96);
    HFONT font = CreateFontW(
        height,
        0,
        0,
        0,
        FW_NORMAL,
        FALSE,
        FALSE,
        FALSE,
        DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,
        FIXED_PITCH | FF_MODERN,
        L"Consolas");
    if (font) {
        s.inputFont = font;
        s.ownsInputFont = true;
    } else {
        s.inputFont = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    }
    SendMessageW(s.inputEdit, WM_SETFONT, reinterpret_cast<WPARAM>(s.inputFont), TRUE);
    SendMessageW(
        s.inputEdit,
        EM_SETMARGINS,
        EC_LEFTMARGIN | EC_RIGHTMARGIN,
        MAKELPARAM(static_cast<WORD>(6), static_cast<WORD>(6)));

    s.inputTextHeightPx = 0;
    HDC dc = GetDC(s.inputEdit);
    if (dc) {
        HFONT old = reinterpret_cast<HFONT>(SelectObject(dc, s.inputFont));
        TEXTMETRICW tm{};
        if (GetTextMetricsW(dc, &tm)) {
            s.inputTextHeightPx = static_cast<int>(tm.tmHeight + tm.tmExternalLeading);
        }
        if (old) {
            SelectObject(dc, old);
        }
        ReleaseDC(s.inputEdit, dc);
    }
    if (s.inputTextHeightPx <= 0) {
        s.inputTextHeightPx = max(12, MulDiv(static_cast<int>(std::lround(s.inputFontSize)), static_cast<int>(dpi), 96));
    }
}

int FontDeltaForKey(WPARAM key) {
    if (key == VK_ADD || key == VK_OEM_PLUS) {
        return +1;
    }
    if (key == VK_SUBTRACT || key == VK_OEM_MINUS) {
        return -1;
    }
    return 0;
}

int ComputeInputHeightPx(const AppState& s) {
    float base = static_cast<float>((s.inputTextHeightPx > 0) ? s.inputTextHeightPx : static_cast<int>(std::ceil(s.inputFontSize)));
    float h = base + kInputVerticalPadding;
    if (h < kInputMinHeight) {
        h = kInputMinHeight;
    }
    return static_cast<int>(std::ceil(h));
}

void LayoutInputControl(HWND hwnd, AppState& s) {
    if (!s.inputEdit) {
        return;
    }
    RECT rc{};
    GetClientRect(hwnd, &rc);
    int cw = rc.right - rc.left;
    int ch = rc.bottom - rc.top;
    int inputH = ComputeInputHeightPx(s);
    int x = static_cast<int>(kInputSidePadding);
    int y = ch - static_cast<int>(kInputBottomPadding) - inputH;
    int w = max(40, cw - static_cast<int>(kInputSidePadding * 2));
    MoveWindow(s.inputEdit, x, max(0, y), w, inputH, TRUE);
}

void ApplyWindowSizeForFont(HWND hwnd, const AppState& s) {
    float grow = s.mainFontSize - kFontResizeThreshold;
    if (grow < 0.0f) {
        grow = 0.0f;
    }
    float targetW = kBaseWindowWidth + (grow * kFontGrowWidthPerPoint);
    float targetH = kBaseWindowHeight + (grow * kFontGrowHeightPerPoint);

    float maxW = MaxWidthForMonitor(hwnd);
    float maxH = MaxHeightForMonitor(hwnd);
    if (targetW > maxW) targetW = maxW;
    if (targetH > maxH) targetH = maxH;
    if (targetW < kMinWidth) targetW = kMinWidth;
    if (targetH < kMinHeight) targetH = kMinHeight;

    MoveToTopRight(hwnd, targetW, targetH);
}

void AdjustFontSizes(AppState& s, int delta, HWND hwnd) {
    if (delta == 0) {
        return;
    }
    float nextMain = std::clamp(s.mainFontSize + (delta * kFontStep), kMinMainFontSize, kMaxMainFontSize);
    float nextInput = nextMain;
    if (nextMain == s.mainFontSize && nextInput == s.inputFontSize) {
        return;
    }
    s.mainFontSize = nextMain;
    s.inputFontSize = nextInput;
    if (!RebuildTextFormats(s)) {
        return;
    }
    ApplyInputFont(s);
    ApplyWindowSizeForFont(hwnd, s);
    LayoutInputControl(hwnd, s);
    InvalidateRect(hwnd, nullptr, FALSE);
    std::printf("font size main=%.1f input=%.1f\n", s.mainFontSize, s.inputFontSize);
    std::fflush(stdout);
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
    bool visible = IsWindowVisible(hwnd) != FALSE;
    AppendMenuW(
        menu,
        MF_STRING,
        kTrayToggleVisibilityCommand,
        visible ? L"Hide HUD" : L"Show HUD");
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

void SetLatestText(AppState& s, const std::wstring& text) {
    std::lock_guard<std::mutex> lock(s.textMutex);
    s.latestText = text.empty() ? L"Recording active." : text;
    s.dataVersion += 1;
}

bool RequestBorderlessCaptureAccess() {
    try {
        if (!winrt::Windows::Foundation::Metadata::ApiInformation::IsMethodPresent(
                L"Windows.Graphics.Capture.GraphicsCaptureAccess", L"RequestAccessAsync")) {
            std::fwprintf(stderr, L"capture borderless: RequestAccessAsync unavailable\n");
            return false;
        }
        auto status = winrt::Windows::Graphics::Capture::GraphicsCaptureAccess::RequestAccessAsync(
                          winrt::Windows::Graphics::Capture::GraphicsCaptureAccessKind::Borderless)
                          .get();
        if (status == winrt::Windows::Security::Authorization::AppCapabilityAccess::AppCapabilityAccessStatus::Allowed) {
            std::fwprintf(stderr, L"capture borderless: allowed\n");
            return true;
        }
        std::fwprintf(stderr, L"capture borderless: denied (status=%d)\n", static_cast<int>(status));
        return false;
    } catch (...) {
        std::fwprintf(stderr, L"capture borderless: request failed\n");
        return false;
    }
}

std::wstring HumanTimeFromUnixText(const std::string& tsText) {
    if (tsText.empty()) {
        return L"--:--:--";
    }
    double ts = 0.0;
    try {
        ts = std::stod(tsText);
    } catch (...) {
        return L"--:--:--";
    }
    if (ts <= 0.0) {
        return L"--:--:--";
    }
    std::time_t sec = static_cast<std::time_t>(ts);
    std::tm tmv{};
    if (localtime_s(&tmv, &sec) != 0) {
        return L"--:--:--";
    }
    wchar_t buf[16] = {};
    if (wcsftime(buf, std::size(buf), L"%H:%M:%S", &tmv) == 0) {
        return L"--:--:--";
    }
    return buf;
}

D2D1_COLOR_F TimestampColor(const std::wstring& stamp) {
    uint32_t h = 2166136261u;
    for (wchar_t ch : stamp) {
        h ^= static_cast<uint32_t>(ch);
        h *= 16777619u;
    }
    float hue = static_cast<float>(h % 360) / 60.0f;
    float c = 0.74f;
    float x = c * (1.0f - fabsf(fmodf(hue, 2.0f) - 1.0f));
    float r = 0.0f, g = 0.0f, b = 0.0f;
    if (hue < 1.0f) { r = c; g = x; }
    else if (hue < 2.0f) { r = x; g = c; }
    else if (hue < 3.0f) { g = c; b = x; }
    else if (hue < 4.0f) { g = x; b = c; }
    else if (hue < 5.0f) { r = x; b = c; }
    else { r = c; b = x; }
    float m = 0.22f;
    return D2D1::ColorF(r + m, g + m, b + m, 0.98f);
}

void AppendLiveLogLine(AppState& s, const std::wstring& hhmmss, const std::wstring& text) {
    std::wstring body = Trim(text);
    if (body.empty()) {
        body = L"Recording active.";
    }
    std::lock_guard<std::mutex> lock(s.textMutex);
    s.logLines.push_back(LogLine{hhmmss.empty() ? L"--:--:--" : hhmmss, body});
    if (s.logLines.size() > 400) {
        s.logLines.pop_front();
    }
    s.latestText = body;
    s.dataVersion += 1;
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

void SetProcessAudioMuted(bool mute) {
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioSessionManager2* manager = nullptr;
    IAudioSessionEnumerator* sessions = nullptr;

    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator),
        nullptr,
        CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator),
        reinterpret_cast<void**>(&enumerator));
    if (FAILED(hr) || !enumerator) {
        return;
    }
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device);
    if (FAILED(hr) || !device) {
        SafeRelease(enumerator);
        return;
    }
    hr = device->Activate(
        __uuidof(IAudioSessionManager2),
        CLSCTX_ALL,
        nullptr,
        reinterpret_cast<void**>(&manager));
    if (FAILED(hr) || !manager) {
        SafeRelease(device);
        SafeRelease(enumerator);
        return;
    }
    hr = manager->GetSessionEnumerator(&sessions);
    if (FAILED(hr) || !sessions) {
        SafeRelease(manager);
        SafeRelease(device);
        SafeRelease(enumerator);
        return;
    }

    DWORD selfPid = GetCurrentProcessId();
    int count = 0;
    sessions->GetCount(&count);
    for (int i = 0; i < count; ++i) {
        IAudioSessionControl* control = nullptr;
        IAudioSessionControl2* control2 = nullptr;
        ISimpleAudioVolume* volume = nullptr;
        if (FAILED(sessions->GetSession(i, &control)) || !control) {
            continue;
        }
        if (SUCCEEDED(control->QueryInterface(__uuidof(IAudioSessionControl2), reinterpret_cast<void**>(&control2))) && control2) {
            DWORD pid = 0;
            if (SUCCEEDED(control2->GetProcessId(&pid)) && pid == selfPid) {
                if (SUCCEEDED(control2->QueryInterface(__uuidof(ISimpleAudioVolume), reinterpret_cast<void**>(&volume))) && volume) {
                    volume->SetMute(mute ? TRUE : FALSE, nullptr);
                }
            }
        }
        SafeRelease(volume);
        SafeRelease(control2);
        SafeRelease(control);
    }

    SafeRelease(sessions);
    SafeRelease(manager);
    SafeRelease(device);
    SafeRelease(enumerator);
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

bool CaptureWindowDownsampledPng(HWND hwnd, bool borderlessAllowed, std::vector<BYTE>& pngBytes) {
    try {
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
    session.IsCursorCaptureEnabled(false);
    if (borderlessAllowed &&
        winrt::Windows::Foundation::Metadata::ApiInformation::IsPropertyPresent(
            L"Windows.Graphics.Capture.GraphicsCaptureSession", L"IsBorderRequired")) {
        session.IsBorderRequired(false);
    }

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
    } catch (...) {
        return false;
    }
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

bool PostPubText(const std::wstring& pubUrl, const std::wstring& text) {
    std::string bodyUtf8 = WideToUtf8(text);
    if (bodyUtf8.empty()) {
        return false;
    }

    URL_COMPONENTSW parts{};
    wchar_t host[256] = {};
    wchar_t path[2048] = {};
    parts.dwStructSize = sizeof(parts);
    parts.lpszHostName = host;
    parts.dwHostNameLength = static_cast<DWORD>(std::size(host));
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = static_cast<DWORD>(std::size(path));
    parts.dwSchemeLength = 1;
    if (!WinHttpCrackUrl(pubUrl.c_str(), 0, 0, &parts)) {
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
        L"Content-Type: text/plain\r\n",
        -1L,
        bodyUtf8.data(),
        static_cast<DWORD>(bodyUtf8.size()),
        static_cast<DWORD>(bodyUtf8.size()),
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

bool AppendTextLineToFile(const std::wstring& path, const std::wstring& text) {
    if (path.empty()) {
        return false;
    }
    try {
        std::filesystem::path p(path);
        if (!p.parent_path().empty()) {
            std::filesystem::create_directories(p.parent_path());
        }
        std::ofstream out(p, std::ios::binary | std::ios::app);
        if (!out.is_open()) {
            return false;
        }
        std::string line = WideToUtf8(text) + "\n";
        out.write(line.data(), static_cast<std::streamsize>(line.size()));
        return out.good();
    } catch (...) {
        return false;
    }
}

void PollInputFile(AppState& s) {
    if (!s.singlePlayerMode || s.inputFilePath.empty()) {
        return;
    }
    std::wstring contentWide;
    try {
        std::ifstream in(std::filesystem::path(s.inputFilePath), std::ios::binary);
        if (!in.is_open()) {
            return;
        }
        std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        contentWide = Utf8ToWide(content);
    } catch (...) {
        return;
    }
    std::wstring normalized = Trim(contentWide);
    if (normalized.empty()) {
        normalized = L"Recording active.";
    }
    std::lock_guard<std::mutex> lock(s.textMutex);
    if (normalized != s.latestText) {
        s.latestText = normalized;
        s.dataVersion += 1;
    }
}

void SubmitInput(AppState& s) {
    if (!s.inputEdit) {
        return;
    }
    int len = GetWindowTextLengthW(s.inputEdit);
    if (len <= 0) {
        return;
    }
    std::wstring raw(static_cast<size_t>(len) + 1, L'\0');
    GetWindowTextW(s.inputEdit, raw.data(), len + 1);
    raw.resize(static_cast<size_t>(len));
    std::wstring text = Trim(raw);
    if (text.empty()) {
        return;
    }
    SetWindowTextW(s.inputEdit, L"");
    if (s.singlePlayerMode) {
        if (!AppendTextLineToFile(s.outputFilePath, text)) {
            SetLatestText(s, L"pub text error: cannot write output file");
        }
        return;
    }
    std::wstring pubUrl = s.pubUrl;
    std::thread([pubUrl, text]() {
        PostPubText(pubUrl, text);
    }).detach();
}

LRESULT CALLBACK InputEditProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    AppState* s = reinterpret_cast<AppState*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (msg == WM_CHAR && wParam == VK_RETURN && s && !s->speechEnabled) {
        return 0;
    }
    if (msg == WM_KEYDOWN) {
        if ((GetKeyState(VK_CONTROL) & 0x8000) != 0) {
            int delta = FontDeltaForKey(wParam);
            if (delta != 0) {
                if (s) {
                    PostMessageW(GetParent(hwnd), kAdjustFontMsg, static_cast<WPARAM>(delta), 0);
                }
                return 0;
            }
        }
        if (wParam == VK_RETURN && s) {
            PostMessageW(GetParent(hwnd), kSubmitInputMsg, 0, 0);
            return 0;
        }
    }
    if (!s || !s->inputEditOldProc) {
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    return CallWindowProcW(s->inputEditOldProc, hwnd, msg, wParam, lParam);
}

void CaptureLoop(AppState* state) {
    while (!state->stopCapture.load()) {
        try {
            HWND target = GetForegroundCaptureWindow(state->requireTargetActive.load());
            if (target) {
                std::vector<BYTE> pngBytes;
                if (CaptureWindowDownsampledPng(target, state->borderlessCaptureAllowed, pngBytes)) {
                    std::string filename = NewUuidV1Filename();
                    UploadPng(pngBytes, filename);
                }
            }
        } catch (...) {
            // Keep HUD alive even if capture APIs fail on this Windows build.
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
        std::fwprintf(stderr, L"sub error: invalid URL\n");
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
        std::fwprintf(stderr, L"sub error: WinHttpOpen failed\n");
        return;
    }
    WinHttpSetTimeouts(session, 5000, 5000, 5000, 30000);

    while (!state->stopSub.load()) {
        HINTERNET connect = WinHttpConnect(session, hostName.c_str(), parts.nPort, 0);
        if (!connect) {
            std::fwprintf(stderr, L"sub reconnecting\n");
            Sleep(1000);
            continue;
        }

        HINTERNET request = WinHttpOpenRequest(connect, L"GET", pathName.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (!request) {
            WinHttpCloseHandle(connect);
            std::fwprintf(stderr, L"sub reconnecting\n");
            Sleep(1000);
            continue;
        }

        WinHttpAddRequestHeaders(request, L"Accept: text/event-stream\r\nCache-Control: no-cache\r\n", -1, WINHTTP_ADDREQ_FLAG_ADD);
        BOOL ok = WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
        if (ok) ok = WinHttpReceiveResponse(request, nullptr);
        if (!ok) {
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connect);
            std::fwprintf(stderr, L"sub reconnecting\n");
            Sleep(1000);
            continue;
        }

        std::string buf;
        std::string line;
        std::string eventId;
        std::string eventText;
        bool hasText = false;
        auto flushEvent = [&]() {
            if (!hasText) {
                eventId.clear();
                return;
            }
            std::wstring hhmmss = HumanTimeFromUnixText(eventId);
            AppendLiveLogLine(*state, hhmmss, Utf8ToWide(eventText));
            eventId.clear();
            eventText.clear();
            hasText = false;
        };
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
                    flushEvent();
                    continue;
                }
                if (line.rfind("id:", 0) == 0) {
                    flushEvent();
                    eventId = TrimAscii(line.substr(3));
                    continue;
                }
                if (line.rfind("data:", 0) != 0) {
                    continue;
                }
                std::string payload = TrimAscii(line.substr(5));
                size_t colon = payload.find(':');
                if (colon == std::string::npos) {
                    continue;
                }
                std::string key = TrimAscii(payload.substr(0, colon));
                std::string value = TrimAscii(payload.substr(colon + 1));
                if (key == "text") {
                    size_t pos = 0;
                    while ((pos = value.find("\\n", pos)) != std::string::npos) {
                        value.replace(pos, 2, "\n");
                        pos += 1;
                    }
                    eventText = value;
                    hasText = true;
                    flushEvent();
                }
            }
        }
        flushEvent();

        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        if (!state->stopSub.load()) {
            Sleep(500);
        }
    }

    WinHttpCloseHandle(session);
}

void RefreshTextIfChanged(HWND hwnd, AppState& s) {
    std::wstring latest;
    uint64_t version = 0;
    {
        std::lock_guard<std::mutex> lock(s.textMutex);
        latest = s.latestText;
        version = s.dataVersion;
    }
    if (latest.empty()) {
        latest = L"Recording active.";
    }
    if (s.singlePlayerMode) {
        latest = Trim(latest);
        if (latest.empty()) {
            latest = L"Recording active.";
        }
        if (latest != s.currentText) {
            s.currentText = latest;
        }
    }
    if (version != s.renderedVersion) {
        s.renderedVersion = version;
        SpeakLatestText(s, latest);
        InvalidateRect(hwnd, nullptr, FALSE);
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    AppState* s = reinterpret_cast<AppState*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));

    switch (msg) {
        case WM_NCHITTEST:
            // Make the overlay ignore mouse hit-testing so clicks pass through.
            return HTTRANSPARENT;
        case WM_NCCREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCT*>(lParam);
            SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            return DefWindowProc(hwnd, msg, wParam, lParam);
        }
        case WM_CREATE: {
            SetTimer(hwnd, kPollTimerId, kPollMs, nullptr);
            if (s) {
                AddTrayIcon(hwnd, s->appIcon);
                if (!s->inputBgBrush) {
                    s->inputBgBrush = CreateSolidBrush(kInputBgColor);
                }
                s->inputEdit = CreateWindowExW(
                    0,
                    L"EDIT",
                    L"",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                    0,
                    0,
                    0,
                    0,
                    hwnd,
                    nullptr,
                    reinterpret_cast<HINSTANCE>(GetWindowLongPtr(hwnd, GWLP_HINSTANCE)),
                    nullptr);
                if (s->inputEdit) {
                    ApplyInputFont(*s);
                    SendMessageW(s->inputEdit, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"ask:"));
                    SetWindowLongPtrW(s->inputEdit, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(s));
                    s->inputEditOldProc = reinterpret_cast<WNDPROC>(
                        SetWindowLongPtrW(s->inputEdit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(InputEditProc)));
                    LayoutInputControl(hwnd, *s);
                }
            }
            return 0;
        }
        case WM_CTLCOLOREDIT: {
            if (s && s->inputEdit && reinterpret_cast<HWND>(lParam) == s->inputEdit) {
                HDC dc = reinterpret_cast<HDC>(wParam);
                SetTextColor(dc, kInputTextColor);
                SetBkColor(dc, kInputBgColor);
                return reinterpret_cast<LRESULT>(s->inputBgBrush ? s->inputBgBrush : GetStockObject(BLACK_BRUSH));
            }
            return DefWindowProc(hwnd, msg, wParam, lParam);
        }
        case kTrayCallbackMsg: {
            if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) {
                ShowTrayMenu(hwnd, s);
            } else if (lParam == WM_LBUTTONDBLCLK) {
                if (IsWindowVisible(hwnd)) {
                    ShowWindow(hwnd, SW_HIDE);
                } else {
                    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
                    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                }
            }
            return 0;
        }
        case WM_COMMAND: {
            if (LOWORD(wParam) == kTrayToggleVisibilityCommand) {
                if (IsWindowVisible(hwnd)) {
                    ShowWindow(hwnd, SW_HIDE);
                } else {
                    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
                    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                }
                return 0;
            }
            if (LOWORD(wParam) == kTrayToggleSpeechCommand && s) {
                s->speechEnabled = !s->speechEnabled;
                if (!s->speechEnabled) {
                    StopSpeaking(*s);
                }
                SetProcessAudioMuted(!s->speechEnabled);
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
        case WM_SETFOCUS: {
            if (s && s->inputEdit) {
                SetFocus(s->inputEdit);
            }
            return 0;
        }
        case WM_ACTIVATE: {
            if (s && s->inputEdit && LOWORD(wParam) != WA_INACTIVE) {
                SetFocus(s->inputEdit);
            }
            return DefWindowProc(hwnd, msg, wParam, lParam);
        }
        case kSubmitInputMsg: {
            if (s) {
                SubmitInput(*s);
            }
            return 0;
        }
        case kAdjustFontMsg: {
            if (s) {
                int delta = static_cast<int>(wParam);
                AdjustFontSizes(*s, delta, hwnd);
            }
            return 0;
        }
        case WM_KEYDOWN: {
            if (s && (GetKeyState(VK_CONTROL) & 0x8000) != 0) {
                int delta = FontDeltaForKey(wParam);
                if (delta != 0) {
                    AdjustFontSizes(*s, delta, hwnd);
                    return 0;
                }
            }
            return DefWindowProc(hwnd, msg, wParam, lParam);
        }
        case WM_MOUSEWHEEL: {
            if (s && !s->singlePlayerMode) {
                s->wheelRemainder += GET_WHEEL_DELTA_WPARAM(wParam);
                int steps = s->wheelRemainder / WHEEL_DELTA;
                s->wheelRemainder = s->wheelRemainder % WHEEL_DELTA;
                if (steps != 0) {
                    s->scrollOffsetPx += static_cast<float>(steps) * kWheelStepPx;
                    if (s->scrollOffsetPx < 0.0f) {
                        s->scrollOffsetPx = 0.0f;
                    }
                    if (s->scrollOffsetPx > s->maxScrollOffsetPx) {
                        s->scrollOffsetPx = s->maxScrollOffsetPx;
                    }
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
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
                if (s->singlePlayerMode) {
                    PollInputFile(*s);
                }
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
            if (s) {
                LayoutInputControl(hwnd, *s);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_DPICHANGED: {
            if (s) {
                RECT* suggested = reinterpret_cast<RECT*>(lParam);
                if (suggested) {
                    SetWindowPos(
                        hwnd,
                        nullptr,
                        suggested->left,
                        suggested->top,
                        suggested->right - suggested->left,
                        suggested->bottom - suggested->top,
                        SWP_NOZORDER | SWP_NOACTIVATE);
                }
                ApplyInputFont(*s);
                LayoutInputControl(hwnd, *s);
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
            const D2D1_COLOR_F whiteText = D2D1::ColorF(1, 1, 1, 0.95f);
            float left = kPadding;
            float right = bounds.right - kPadding;
            float top = kPadding;
            float inputHeight = static_cast<float>(ComputeInputHeightPx(*s));
            float bottom = bounds.bottom - (kInputBottomPadding + inputHeight + kInputGap);
            if (bottom <= top + 4.0f) {
                bottom = top + 4.0f;
            }
            float textAreaWidth = max(20.0f, right - left);
            float textAreaHeight = max(20.0f, bottom - top);

            if (s->singlePlayerMode) {
                s->maxScrollOffsetPx = 0.0f;
                s->scrollOffsetPx = 0.0f;
                std::wstring drawText;
                {
                    std::lock_guard<std::mutex> lock(s->textMutex);
                    drawText = s->latestText;
                }
                drawText = Trim(drawText);
                if (drawText.empty()) {
                    drawText = L"Recording active.";
                }
                IDWriteTextLayout* layout = nullptr;
                HRESULT lhr = s->dwriteFactory->CreateTextLayout(
                    drawText.c_str(),
                    static_cast<UINT32>(drawText.size()),
                    s->mainFormat,
                    textAreaWidth,
                    textAreaHeight,
                    &layout);
                if (SUCCEEDED(lhr) && layout) {
                    s->fgBrush->SetColor(whiteText);
                    s->rt->DrawTextLayout(
                        D2D1::Point2F(left, top),
                        layout,
                        s->fgBrush,
                        D2D1_DRAW_TEXT_OPTIONS_CLIP);
                    SafeRelease(layout);
                }
            } else {
                std::deque<LogLine> lines;
                {
                    std::lock_guard<std::mutex> lock(s->textMutex);
                    lines = s->logLines;
                }
                if (lines.empty()) {
                    lines.push_back(LogLine{L"--:--:--", L"Recording active."});
                }

                float tsColumnWidth = 0.0f;
                {
                    const std::wstring sampleTs = L"[00:00:00] ";
                    IDWriteTextLayout* sampleLayout = nullptr;
                    HRESULT thr = s->dwriteFactory->CreateTextLayout(
                        sampleTs.c_str(),
                        static_cast<UINT32>(sampleTs.size()),
                        s->tsFormat ? s->tsFormat : s->mainFormat,
                        4096.0f,
                        200.0f,
                        &sampleLayout);
                    if (SUCCEEDED(thr) && sampleLayout) {
                        DWRITE_TEXT_METRICS tm{};
                        sampleLayout->GetMetrics(&tm);
                        tsColumnWidth = tm.widthIncludingTrailingWhitespace + 2.0f;
                    }
                    SafeRelease(sampleLayout);
                }
                if (tsColumnWidth < 70.0f) {
                    tsColumnWidth = 70.0f;
                }
                if (tsColumnWidth > textAreaWidth - 60.0f) {
                    tsColumnWidth = max(40.0f, textAreaWidth * 0.45f);
                }

                struct DrawItem {
                    IDWriteTextLayout* ts = nullptr;
                    IDWriteTextLayout* msg = nullptr;
                    float h = 0.0f;
                    float tsWidth = 0.0f;
                    D2D1_COLOR_F tsColor = D2D1::ColorF(1, 1, 1, 0.95f);
                };
                std::vector<DrawItem> items;
                float totalHeight = 0.0f;
                for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
                    std::wstring stamp = it->hhmmss.empty() ? L"--:--:--" : it->hhmmss;
                    std::wstring prefix = L"[" + stamp + L"] ";
                    IDWriteTextLayout* tsLayout = nullptr;
                    IDWriteTextLayout* msgLayout = nullptr;
                    HRESULT thr = s->dwriteFactory->CreateTextLayout(
                        prefix.c_str(),
                        static_cast<UINT32>(prefix.size()),
                        s->tsFormat ? s->tsFormat : s->mainFormat,
                        4096.0f,
                        200.0f,
                        &tsLayout);
                    if (FAILED(thr) || !tsLayout) {
                        SafeRelease(tsLayout);
                        SafeRelease(msgLayout);
                        continue;
                    }
                    DWRITE_TEXT_METRICS tm{};
                    tsLayout->GetMetrics(&tm);
                    float msgWidth = max(60.0f, textAreaWidth - tsColumnWidth);
                    std::wstring body = it->text.empty() ? L"Recording active." : it->text;
                    HRESULT mhr = s->dwriteFactory->CreateTextLayout(
                        body.c_str(),
                        static_cast<UINT32>(body.size()),
                        s->mainFormat,
                        msgWidth,
                        1000.0f,
                        &msgLayout);
                    if (FAILED(mhr) || !msgLayout) {
                        SafeRelease(tsLayout);
                        SafeRelease(msgLayout);
                        continue;
                    }
                    DWRITE_TEXT_METRICS mm{};
                    msgLayout->GetMetrics(&mm);
                    float h = max(tm.height, mm.height) + 2.0f;
                    totalHeight += h;
                    DrawItem item;
                    item.ts = tsLayout;
                    item.msg = msgLayout;
                    item.h = h;
                    item.tsWidth = tsColumnWidth;
                    item.tsColor = TimestampColor(stamp);
                    items.push_back(item);
                }

                float viewportHeight = max(1.0f, bottom - top);
                s->maxScrollOffsetPx = max(0.0f, totalHeight - viewportHeight);
                if (s->scrollOffsetPx < 0.0f) {
                    s->scrollOffsetPx = 0.0f;
                }
                if (s->scrollOffsetPx > s->maxScrollOffsetPx) {
                    s->scrollOffsetPx = s->maxScrollOffsetPx;
                }

                float y = bottom + s->scrollOffsetPx;
                for (const auto& item : items) {
                    y -= item.h;
                    if (y + item.h < top) {
                        SafeRelease(item.ts);
                        SafeRelease(item.msg);
                        continue;
                    }
                    if (y > bottom) {
                        SafeRelease(item.ts);
                        SafeRelease(item.msg);
                        continue;
                    }
                    s->fgBrush->SetColor(item.tsColor);
                    s->rt->DrawTextLayout(
                        D2D1::Point2F(left, y),
                        item.ts,
                        s->fgBrush,
                        D2D1_DRAW_TEXT_OPTIONS_CLIP);
                    s->fgBrush->SetColor(whiteText);
                    s->rt->DrawTextLayout(
                        D2D1::Point2F(left + item.tsWidth, y),
                        item.msg,
                        s->fgBrush,
                        D2D1_DRAW_TEXT_OPTIONS_CLIP);
                    SafeRelease(item.ts);
                    SafeRelease(item.msg);
                }
                s->fgBrush->SetColor(whiteText);
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
            if (s && s->inputBgBrush) {
                DeleteObject(s->inputBgBrush);
                s->inputBgBrush = nullptr;
            }
            if (s && s->ownsInputFont && s->inputFont) {
                DeleteObject(s->inputFont);
                s->inputFont = nullptr;
                s->ownsInputFont = false;
            }
            PostQuitMessage(0);
            return 0;
        }
        default:
            return DefWindowProc(hwnd, msg, wParam, lParam);
    }
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int) {
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
        auto setDpiCtx = reinterpret_cast<BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT)>(
            GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
        if (setDpiCtx) {
            setDpiCtx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        }
    }
    HRESULT comHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    AppState state;
    ParseArgsResult parseResult = ParseArgs(state);
    if (parseResult == ParseArgsResult::Help) {
        if (SUCCEEDED(comHr)) {
            CoUninitialize();
        }
        return 0;
    }
    if (parseResult == ParseArgsResult::Error) {
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

    if (!RebuildTextFormats(state)) {
        SafeRelease(state.metaFormat);
        SafeRelease(state.mainFormat);
        SafeRelease(state.tsFormat);
        SafeRelease(state.dwriteFactory);
        SafeRelease(state.d2dFactory);
        return 1;
    }
    CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_ALL, IID_ISpVoice, reinterpret_cast<void**>(&state.voice));
    if (state.voice) {
        state.voice->SetRate(5);
    }
    SetProcessAudioMuted(!state.speechEnabled);

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

    DWORD exStyle = WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_APPWINDOW | WS_EX_TRANSPARENT;
    DWORD style = WS_POPUP;

    float initialWidth = kBaseWindowWidth;
    float initialHeight = kBaseWindowHeight;

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
        SafeRelease(state.tsFormat);
        SafeRelease(state.dwriteFactory);
        SafeRelease(state.d2dFactory);
        return 1;
    }

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

    if (state.singlePlayerMode) {
        std::wprintf(
            L"Faeton HUD started in single-player mode. I read overlay text from %ls. Screenshot uploads are disabled.\n",
            state.inputFilePath.c_str());
    } else {
        state.borderlessCaptureAllowed = RequestBorderlessCaptureAccess();
        std::wprintf(
            L"Faeton HUD started in multiplayer mode. I subscribe to live text updates from %ls and upload screenshots every %d seconds.\n",
            state.subUrl.c_str(),
            kCaptureIntervalMs / 1000);
        state.subThread = std::thread(SubscribeLoop, &state);
        state.captureThread = std::thread(CaptureLoop, &state);
    }
    std::fflush(stdout);

    if (state.singlePlayerMode) {
        PollInputFile(state);
    }
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
    SafeRelease(state.tsFormat);
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
