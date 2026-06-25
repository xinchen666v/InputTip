// inputtip_v3.cpp -- C++17, Win32 + WIC + IMM32 + WinEvent + UIA + Layered Window
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <imm.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <uiautomation.h>
#include <string>
#include <memory>
#include <shellapi.h>
#include <strsafe.h>

#pragma comment(lib, "imm32.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "oleaut32.lib")

using Microsoft::WRL::ComPtr;

// ════════════════════════════════════════════════════
//  全局
// ════════════════════════════════════════════════════
static HWND       g_hOverlay = nullptr;
static UINT_PTR   g_pollTimer = 1;   // 100ms 轮询
static UINT_PTR   g_hideTimer = 2;   // 显示后自动隐藏
static const UINT kPollMs = 100;
static const UINT kShowMs = 2000;
static int        g_lastState = -1;  // 0=EN  1=CN  -1=未知

static HWINEVENTHOOK g_hookForeground = nullptr;

#define WM_TRAYICON   (WM_APP + 1)
#define IDM_EXIT      1001

// ════════════════════════════════════════════════════
//  PNG 位图
// ════════════════════════════════════════════════════
struct PngBitmap { std::unique_ptr<BYTE[]> bgra; UINT w = 0, h = 0; };
static PngBitmap g_imgCN, g_imgEN;

bool LoadPngPremultiplied(const wchar_t* path, PngBitmap& out)
{
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) return false;
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromFilename(path, nullptr, GENERIC_READ,
        WICDecodeMetadataCacheOnLoad, &decoder))) return false;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) return false;
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(factory->CreateFormatConverter(&conv))) return false;
    if (FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
        WICBitmapDitherTypeNone, nullptr, 0.0,
        WICBitmapPaletteTypeCustom))) return false;
    UINT w = 0, h = 0; conv->GetSize(&w, &h);
    UINT stride = w * 4, size = stride * h;
    auto buf = std::make_unique<BYTE[]>(size);
    if (FAILED(conv->CopyPixels(nullptr, stride, size, buf.get()))) return false;
    out.bgra = std::move(buf); out.w = w; out.h = h;
    return true;
}

// ════════════════════════════════════════════════════
//  IME 状态查询（IMM32 跨进程）
// ════════════════════════════════════════════════════
int QueryImeState()
{
    HWND fg = GetForegroundWindow();
    if (!fg) return -1;

    // 跳过自己的 overlay 窗口
    DWORD myPid = 0, fgPid = 0;
    GetWindowThreadProcessId(g_hOverlay, &myPid);
    GetWindowThreadProcessId(fg, &fgPid);
    if (fgPid == myPid) return g_lastState;

    HWND ime = ImmGetDefaultIMEWnd(fg);
    if (!ime) return 0;   // 无 IME 窗口 = 纯英文/不支持输入法

    DWORD_PTR openStatus = 0, conv = 0;
    LRESULT r1 = SendMessageTimeoutW(ime, WM_IME_CONTROL, 0x0005, 0,
        SMTO_ABORTIFHUNG, 80, &openStatus);
    LRESULT r2 = SendMessageTimeoutW(ime, WM_IME_CONTROL, 0x0001, 0,
        SMTO_ABORTIFHUNG, 80, &conv);
    if (!r1 || !r2) return -1;
    if (openStatus == 0) return 0;
    return (conv & 0x1) ? 1 : 0;
}

// ════════════════════════════════════════════════════
//  Caret 位置：Win32 → UIA → 鼠标
// ════════════════════════════════════════════════════
bool TryUiaCaret(POINT& out)
{
    ComPtr<IUIAutomation> ua;
    if (FAILED(CoCreateInstance(__uuidof(CUIAutomation8), nullptr,
        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&ua)))) return false;
    ComPtr<IUIAutomationElement> el;
    if (FAILED(ua->GetFocusedElement(&el)) || !el) return false;
    ComPtr<IUIAutomationTextPattern> tp;
    if (FAILED(el->GetCurrentPatternAs(UIA_TextPatternId,
        IID_PPV_ARGS(&tp))) || !tp) return false;
    ComPtr<IUIAutomationTextRangeArray> ranges;
    if (FAILED(tp->GetSelection(&ranges)) || !ranges) return false;
    int count = 0; ranges->get_Length(&count);
    if (count == 0) return false;
    ComPtr<IUIAutomationTextRange> r;
    if (FAILED(ranges->GetElement(0, &r)) || !r) return false;
    SAFEARRAY* sa = nullptr;
    if (FAILED(r->GetBoundingRectangles(&sa)) || !sa) return false;
    LONG lb = 0, ub = 0;
    SafeArrayGetLBound(sa, 1, &lb); SafeArrayGetUBound(sa, 1, &ub);
    bool ok = false;
    if (ub - lb >= 3) {
        double left = 0, top = 0, width = 0, height = 0; LONG idx = lb;
        SafeArrayGetElement(sa, &idx, &left);  idx++;
        SafeArrayGetElement(sa, &idx, &top);   idx++;
        SafeArrayGetElement(sa, &idx, &width); idx++;
        SafeArrayGetElement(sa, &idx, &height);
        out.x = (LONG)left;
        out.y = (LONG)(top + height);
        ok = true;
    }
    SafeArrayDestroy(sa);
    return ok;
}

POINT GetCaretScreenPos()
{
    HWND fg = GetForegroundWindow();
    if (fg) {
        DWORD tid = GetWindowThreadProcessId(fg, nullptr);
        GUITHREADINFO gti{ sizeof(gti) };
        if (GetGUIThreadInfo(tid, &gti) && gti.hwndCaret) {
            POINT p{ gti.rcCaret.left, gti.rcCaret.top };  // 改这里：bottom → top
            ClientToScreen(gti.hwndCaret, &p);
            return p;
        }
    }
    POINT p{};
    if (TryUiaCaret(p)) return p;
    GetCursorPos(&p);
    return p;
}


// ════════════════════════════════════════════════════
//  分层窗口渲染
// ════════════════════════════════════════════════════
void DrawLayered(HWND hwnd, const PngBitmap& img, POINT pos)
{
    HMONITOR hm = MonitorFromPoint(pos, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(hm, &mi);
    if (pos.x + (LONG)img.w > mi.rcWork.right)  pos.x = mi.rcWork.right - (LONG)img.w;
    if (pos.x < mi.rcWork.left)                  pos.x = mi.rcWork.left;
    if (pos.y < mi.rcWork.top)                   pos.y = mi.rcWork.top;

    HDC screenDC = GetDC(nullptr);
    HDC memDC = CreateCompatibleDC(screenDC);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = img.w; bi.bmiHeader.biHeight = -(LONG)img.h;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screenDC, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib) { DeleteDC(memDC); ReleaseDC(nullptr, screenDC); return; }
    memcpy(bits, img.bgra.get(), img.w * img.h * 4);
    HGDIOBJ old = SelectObject(memDC, dib);
    SIZE sz{ (LONG)img.w,(LONG)img.h }; POINT src{ 0,0 };
    BLENDFUNCTION bf{ AC_SRC_OVER,0,255,AC_SRC_ALPHA };
    UpdateLayeredWindow(hwnd, screenDC, &pos, &sz, memDC, &src, 0, &bf, ULW_ALPHA);
    SelectObject(memDC, old); DeleteObject(dib);
    DeleteDC(memDC); ReleaseDC(nullptr, screenDC);
}

void ShowTip(int state)
{
    const PngBitmap& img = (state == 1) ? g_imgCN : g_imgEN;
    if (!img.bgra) return;
    POINT caret = GetCaretScreenPos();
    POINT pos{ caret.x + 4, caret.y - (LONG)img.h - 20 };//x+越多越向光标右边偏离，y-越多越向光标上边偏离
    DrawLayered(g_hOverlay, img, pos);
    ShowWindow(g_hOverlay, SW_SHOWNOACTIVATE);
    SetTimer(g_hOverlay, g_hideTimer, kShowMs, nullptr);
}

// ════════════════════════════════════════════════════
//  WinEvent：前台窗口切换时立刻同步一次状态基准
//  （只同步 g_lastState，不弹图标，避免切窗口也弹）
// ════════════════════════════════════════════════════
void CALLBACK OnForegroundChanged(
    HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD)
{
    // 延迟 50ms 再读，等目标进程 IME 状态稳定
    if (g_hOverlay) SetTimer(g_hOverlay, 4, 50, nullptr);
}

// ════════════════════════════════════════════════════
//  托盘
// ════════════════════════════════════════════════════
static NOTIFYICONDATAW g_nid{};
void AddTray(HWND hwnd) {
    g_nid.cbSize = sizeof(g_nid); g_nid.hWnd = hwnd; g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    lstrcpynW(g_nid.szTip, L"InputTip", ARRAYSIZE(g_nid.szTip));
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}
void RemoveTray() { Shell_NotifyIconW(NIM_DELETE, &g_nid); }
void ShowTrayMenu(HWND hwnd) {
    POINT pt; GetCursorPos(&pt);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"退出 InputTip");
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
}

// ════════════════════════════════════════════════════
//  消息循环
// ════════════════════════════════════════════════════
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_TIMER:
        if (wp == g_hideTimer) {
            KillTimer(hwnd, g_hideTimer);
            ShowWindow(hwnd, SW_HIDE);
            return 0;
        }
        if (wp == 4) {
            // 前台切换后的单次延迟读取：只更新基准，不弹图标
            KillTimer(hwnd, 4);
            int s = QueryImeState();
            if (s != -1) g_lastState = s;
            return 0;
        }
        if (wp == g_pollTimer) {
            int s = QueryImeState();
            if (s != -1 && s != g_lastState) {
                // 首次冷启动（g_lastState==-1）时只建立基准，不弹
                if (g_lastState != -1) ShowTip(s);
                g_lastState = s;
            }
            return 0;
        }
        return 0;

    case WM_TRAYICON:
        if (LOWORD(lp) == WM_RBUTTONUP) ShowTrayMenu(hwnd);
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == IDM_EXIT) { RemoveTray(); DestroyWindow(hwnd); }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ════════════════════════════════════════════════════
//  入口
// ════════════════════════════════════════════════════
int APIENTRY wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring dir = exePath;
    dir = dir.substr(0, dir.find_last_of(L"\\/") + 1);
    if (!LoadPngPremultiplied((dir + L"cn.png").c_str(), g_imgCN))
        MessageBoxW(nullptr, L"cn.png 加载失败", L"InputTip", MB_OK);
    if (!LoadPngPremultiplied((dir + L"en.png").c_str(), g_imgEN))
        MessageBoxW(nullptr, L"en.png 加载失败", L"InputTip", MB_OK);

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc; wc.hInstance = hInst;
    wc.lpszClassName = L"InputTipOverlay";
    RegisterClassW(&wc);

    g_hOverlay = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST |
        WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        L"InputTipOverlay", L"", WS_POPUP,
        0, 0, 1, 1, nullptr, nullptr, hInst, nullptr);

    AddTray(g_hOverlay);

    // 前台窗口切换钩子（只用来同步基准，不弹图标）
    g_hookForeground = SetWinEventHook(
        EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
        nullptr, OnForegroundChanged,
        0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    // 100ms 轮询：同一窗口内切换中英文靠这个捕获
    SetTimer(g_hOverlay, g_pollTimer, kPollMs, nullptr);

    // 冷启动建立基准（不弹图标）
    g_lastState = QueryImeState();

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    if (g_hookForeground) UnhookWinEvent(g_hookForeground);
    KillTimer(g_hOverlay, g_pollTimer);
    CoUninitialize();
    return 0;
}
