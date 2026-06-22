// inputtip_mvp.cpp  -- C++17, Win32 + WIC + IMM32 + Layered Window
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <imm.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <string>
#include <memory>
#include <cstdio>
#include <shellapi.h>
#include <strsafe.h>   // StringCchPrintfW
#include "Resource.h"

#pragma comment(lib, "imm32.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")

using Microsoft::WRL::ComPtr;

// ---- 全局 ----
static HWND        g_hOverlay = nullptr;
static UINT_PTR    g_pollTimer = 1;   // 100ms 轮询 IME 状态
static UINT_PTR    g_hideTimer = 2;   // 显示 N 秒后隐藏
static const UINT  kPollMs = 100;
static const UINT  kShowMs = 2000;
static int         g_lastState = -1;  // 0=EN, 1=CN, -1=未知

struct PngBitmap {
    std::unique_ptr<BYTE[]> bgra;     // 预乘 BGRA
    UINT w = 0, h = 0;
};
static PngBitmap g_imgCN, g_imgEN;

// ---- WIC: 把 PNG 解码成预乘 BGRA ----
bool LoadPngPremultiplied(const wchar_t* path, PngBitmap& out)
{
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&factory)))) return false;

    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromFilename(path, nullptr, GENERIC_READ,
        WICDecodeMetadataCacheOnLoad, &decoder))) return false;

    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) return false;

    ComPtr<IWICFormatConverter> conv;
    if (FAILED(factory->CreateFormatConverter(&conv))) return false;
    // GUID_WICPixelFormat32bppPBGRA = 预乘 BGRA, 正是 UpdateLayeredWindow 需要的
    if (FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
        WICBitmapDitherTypeNone, nullptr, 0.0,
        WICBitmapPaletteTypeCustom))) return false;

    UINT w = 0, h = 0;
    conv->GetSize(&w, &h);
    UINT stride = w * 4;
    UINT size = stride * h;
    auto buf = std::make_unique<BYTE[]>(size);
    if (FAILED(conv->CopyPixels(nullptr, stride, size, buf.get()))) return false;

    out.bgra = std::move(buf);
    out.w = w; out.h = h;
    return true;
}

// ---- 日志:Debug 构建 -> OutputDebugString;错误统一进事件日志 ----
static HANDLE g_hEventLog = nullptr;

void LogInit()  { g_hEventLog = RegisterEventSourceW(nullptr, L"InputTip"); }
void LogShutdown() { if (g_hEventLog) DeregisterEventSource(g_hEventLog); }

static void LogV(WORD type, const wchar_t* fmt, va_list ap)
{
    wchar_t buf[1024];
    StringCchVPrintfW(buf, ARRAYSIZE(buf), fmt, ap);

#ifdef _DEBUG
    OutputDebugStringW(buf);
    OutputDebugStringW(L"\n");
#endif

    // 仅 Warning/Error 真正落事件日志,Info 只在 Debug 下走 DebugView
    if (g_hEventLog && type != EVENTLOG_INFORMATION_TYPE) {
        const wchar_t* msgs[1] = { buf };
        ReportEventW(g_hEventLog, type, 0, 0, nullptr, 1, 0, msgs, nullptr);
    }
}

void LogInfo (const wchar_t* fmt, ...) { va_list a; va_start(a,fmt); LogV(EVENTLOG_INFORMATION_TYPE, fmt, a); va_end(a); }
void LogWarn (const wchar_t* fmt, ...) { va_list a; va_start(a,fmt); LogV(EVENTLOG_WARNING_TYPE,     fmt, a); va_end(a); }
void LogError(const wchar_t* fmt, ...) { va_list a; va_start(a,fmt); LogV(EVENTLOG_ERROR_TYPE,       fmt, a); va_end(a); }


// ---- 用 PngBitmap 更新分层窗口 ----
void DrawLayered(HWND hwnd, const PngBitmap& img, POINT screenPos)
{
    HDC screenDC = GetDC(nullptr);
    HDC memDC = CreateCompatibleDC(screenDC);

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = img.w;
    bi.bmiHeader.biHeight = -(LONG)img.h;   // 负 = 顶到底
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screenDC, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    memcpy(bits, img.bgra.get(), img.w * img.h * 4);

    HGDIOBJ old = SelectObject(memDC, dib);

    SIZE sz{ (LONG)img.w, (LONG)img.h };
    POINT srcPt{ 0, 0 };
    BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };

    UpdateLayeredWindow(hwnd, screenDC, &screenPos, &sz,
        memDC, &srcPt, 0, &bf, ULW_ALPHA);

    SelectObject(memDC, old);
    DeleteObject(dib);
    DeleteDC(memDC);
    ReleaseDC(nullptr, screenDC);
}

// ---- 取前台窗口光标位置(屏幕坐标);失败回退到鼠标 ----
POINT GetCaretScreenPos()
{
    GUITHREADINFO gti{ sizeof(gti) };
    HWND fg = GetForegroundWindow();
    DWORD tid = GetWindowThreadProcessId(fg, nullptr);
    if (GetGUIThreadInfo(tid, &gti) && gti.hwndCaret) {
        POINT p{ gti.rcCaret.left, gti.rcCaret.bottom };
        ClientToScreen(gti.hwndCaret, &p);
        return p;
    }
    POINT p{};
    GetCursorPos(&p);
    return p;
}

// ---- 通过 IMM32 跨进程读取前台窗口的 IME 中/英状态 ----
// 返回 1=中文(NATIVE), 0=英文/关闭, -1=查询失败
int QueryImeState()
{
    HWND fg = GetForegroundWindow();
    wchar_t cls[64] = {};
    if (fg) GetClassNameW(fg, cls, 64);
    LogInfo(L"[poll] fg=%p cls=%s\n", fg, cls);   // 无条件先打

    if (!fg) return -1;
    HWND ime = ImmGetDefaultIMEWnd(fg);
    if (!ime) { LogInfo(L"  -> 无默认IME窗口\n"); return 0; }

    DWORD_PTR openStatus = 0, conv = 0;
    LRESULT r1 = SendMessageTimeoutW(ime, WM_IME_CONTROL, 0x0005, 0,
        SMTO_ABORTIFHUNG, 80, &openStatus);
    LRESULT r2 = SendMessageTimeoutW(ime, WM_IME_CONTROL, 0x0001, 0,
        SMTO_ABORTIFHUNG, 80, &conv);
    LogInfo(L"  -> ime=%p open(r=%lld v=%llu) conv(r=%lld v=%llu)\n",
        ime, (long long)r1, (unsigned long long)openStatus,
        (long long)r2, (unsigned long long)conv);

    if (!r1 || !r2) return -1;
    if (openStatus == 0) return 0;
    return (conv & 0x1) ? 1 : 0;
}



void ShowTip(int state)
{
    const PngBitmap& img = (state == 1) ? g_imgCN : g_imgEN;
    if (!img.bgra) { LogWarn(L"[render] state=%d 但图片未加载\n", state); return; }

    POINT caret = GetCaretScreenPos();
    POINT pos{ caret.x + 4, caret.y - (LONG)img.h - 4 };
    LogInfo(L"[render] state=%d caret=(%ld,%ld) draw=(%ld,%ld) size=%ux%u\n",
        state, caret.x, caret.y, pos.x, pos.y, img.w, img.h);

    DrawLayered(g_hOverlay, img, pos);
    ShowWindow(g_hOverlay, SW_SHOWNOACTIVATE);
    SetTimer(g_hOverlay, g_hideTimer, kShowMs, nullptr);
}


#define WM_TRAYICON   (WM_APP + 1)
#define IDM_EXIT      1001
static NOTIFYICONDATAW g_nid{};

void AddTray(HWND hwnd)
{
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = LoadIcon(nullptr, IDI_APPLICATION);   // 先用系统图标占位
    lstrcpynW(g_nid.szTip, L"InputTip", ARRAYSIZE(g_nid.szTip));
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

void RemoveTray() { Shell_NotifyIconW(NIM_DELETE, &g_nid); }

void ShowTrayMenu(HWND hwnd)
{
    POINT pt; GetCursorPos(&pt);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"退出 InputTip");
    SetForegroundWindow(hwnd);   // 必须,否则点外面菜单不消失
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN,
        pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_TIMER:
        if (wp == g_pollTimer) {
            int s = QueryImeState();
            if (s != g_lastState) {
                LogInfo(L"[state] %d -> %d\n", g_lastState, s);
                if (s >= 0 && g_lastState != -1) ShowTip(s);
                g_lastState = s;
            }
        }
        else if (wp == g_hideTimer) {
            KillTimer(hwnd, g_hideTimer);
            ShowWindow(hwnd, SW_HIDE);
            LogInfo(L"[render] 隐藏\n");
        }
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_TRAYICON:
        if (LOWORD(lp) == WM_RBUTTONUP) ShowTrayMenu(hwnd);
        return 0;

    case WM_COMMAND:
        if (LOWORD(wp) == IDM_EXIT) {
            RemoveTray();
            DestroyWindow(hwnd);
        }
        return 0;


    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}





int APIENTRY wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int)
{
    //AllocConsole();
    //FILE* fp = nullptr;
    //freopen_s(&fp, "CONOUT$", "w", stdout);
    //freopen_s(&fp, "CONOUT$", "w", stderr);
    //SetConsoleOutputCP(CP_UTF8);
    //LogInfo(L"[InputTip] 启动\n");

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // 载入两张 PNG(放在 exe 同目录)
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring dir = exePath;
    dir = dir.substr(0, dir.find_last_of(L"\\/") + 1);
    if (!LoadPngPremultiplied((dir + L"cn.png").c_str(), g_imgCN))
        MessageBoxW(nullptr, L"cn.png 加载失败", L"InputTip", MB_OK);
    if (!LoadPngPremultiplied((dir + L"en.png").c_str(), g_imgEN))
        MessageBoxW(nullptr, L"en.png 加载失败", L"InputTip", MB_OK);

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"InputTipOverlay";
    RegisterClassW(&wc);

    // 关键:WS_EX_LAYERED 透明 / TRANSPARENT 鼠标穿透 / NOACTIVATE 不抢焦点
    g_hOverlay = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST |
        WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        L"InputTipOverlay", L"", WS_POPUP,
        0, 0, 1, 1, nullptr, nullptr, hInst, nullptr);

    AddTray(g_hOverlay);

    SetTimer(g_hOverlay, g_pollTimer, kPollMs, nullptr);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    CoUninitialize();
    return 0;
}
