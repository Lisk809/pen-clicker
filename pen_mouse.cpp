// ============================================================
//  pen_mouse.cpp  —  翻页笔鼠标控制器  v1.2
// ============================================================
//
//  v1.2 改动：Per-Monitor DPI 感知，彻底消除白板高分屏模糊
//
//  编译 (MSVC):
//    cl pen_mouse.cpp user32.lib gdi32.lib shell32.lib advapi32.lib
//       /O2 /W3 /utf-8 /MT
//       /Fe:pen_mouse.exe
//       /link /SUBSYSTEM:WINDOWS /ENTRY:wWinMainCRTStartup
//
//  编译 (MinGW / GCC):
//    g++ -O2 -o pen_mouse.exe pen_mouse.cpp
//        -lgdi32 -luser32 -lshell32 -ladvapi32
//        -mwindows -municode
//
//  需要管理员权限（WH_KEYBOARD_LL 低级键盘钩子）
// ============================================================

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <algorithm>
#include <atomic>
#include <string>

using std::min;
using std::max;

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")

// ════════════════════════════════════════════════════════════
//  DPI 辅助
//  g_dpi 在 wWinMain 中初始化，WM_DPICHANGED 时更新
//  S(x)  将 96-DPI 基准像素值缩放到实际 DPI
// ════════════════════════════════════════════════════════════
static UINT g_dpi = 96;
static inline int S(int x) { return MulDiv(x, (int)g_dpi, 96); }

// 动态加载 SetProcessDpiAwarenessContext（Win10 1607+）
static void SetDpiAwareness() {
    // 尝试 Per-Monitor V2（Win10 1703+）= (HANDLE)-4
    // 退化到 Per-Monitor V1              = (HANDLE)-3
    // 最低回退到旧 API SetProcessDPIAware
    typedef BOOL(WINAPI* FnCtx)(HANDLE);
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    FnCtx fn = u32 ? (FnCtx)GetProcAddress(u32, "SetProcessDpiAwarenessContext") : nullptr;
    if (fn) {
        if (!fn((HANDLE)-4))   // PER_MONITOR_AWARE_V2
            fn((HANDLE)-3);    // PER_MONITOR_AWARE
    } else {
        // Vista/7 回退
        typedef BOOL(WINAPI* FnOld)();
        FnOld old = u32 ? (FnOld)GetProcAddress(u32, "SetProcessDPIAware") : nullptr;
        if (old) old();
    }
}

// 获取窗口实际 DPI（Win10 1607+ 有 GetDpiForWindow，否则用 GetDeviceCaps）
static UINT GetWndDpi(HWND hw) {
    typedef UINT(WINAPI* FnGDFW)(HWND);
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    FnGDFW fn = u32 ? (FnGDFW)GetProcAddress(u32, "GetDpiForWindow") : nullptr;
    if (fn && hw) return fn(hw);
    // 回退：通过 DC
    HDC dc = GetDC(hw);
    UINT dpi = (UINT)GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(hw, dc);
    return dpi ? dpi : 96;
}

// ════════════════════════════════════════════════════════════
//  布局基准值（96 DPI 下的逻辑像素，运行时经 S() 放大）
// ════════════════════════════════════════════════════════════
static const int BASE_W  = 380;   // 客户区宽
static const int BASE_H  = 568;   // 客户区高

// ════════════════════════════════════════════════════════════
//  颜色
// ════════════════════════════════════════════════════════════
#define C_BG     RGB( 13,  17,  23)
#define C_PANEL  RGB( 22,  27,  34)
#define C_BORD   RGB( 33,  38,  45)
#define C_TEXT   RGB(230, 237, 243)
#define C_SUB    RGB(110, 118, 129)
#define C_GREEN  RGB( 63, 185,  80)
#define C_RED    RGB(248,  81,  73)
#define C_BLUE   RGB( 88, 166, 255)
#define C_AMBER  RGB(210, 153,  34)
#define C_DIM    RGB( 48,  54,  61)
#define C_TEAL   RGB( 57, 211,  83)
#define C_DARK   RGB( 30,  36,  44)

// ════════════════════════════════════════════════════════════
//  控件 ID / 消息 / 定时器
// ════════════════════════════════════════════════════════════
#define IDC_TOGGLE   101
#define IDC_DET_UP   102
#define IDC_DET_DN   103
#define IDC_DET_ACT  104

#define WM_REFRESH     (WM_USER + 1)
#define TID_CLICK        1
#define TID_CLR_MSG      2

// ════════════════════════════════════════════════════════════
//  移动参数
// ════════════════════════════════════════════════════════════
static const int   STEP_MIN  = 18;
static const int   STEP_MAX  = 75;
static const DWORD ACCEL_MS  = 500;
static const DWORD TICK_MS   = 16;

// ════════════════════════════════════════════════════════════
//  方框键多功能判定
// ════════════════════════════════════════════════════════════
static const DWORD LONG_PRESS = 600;
static const DWORD DBL_GAP    = 350;

// ════════════════════════════════════════════════════════════
//  全局状态
// ════════════════════════════════════════════════════════════
static HWND  g_hwnd = nullptr;
static HHOOK g_hook = nullptr;

static std::atomic<bool>  g_enabled  {false};
static std::atomic<bool>  g_horiz    {false};
static std::atomic<bool>  g_up_held  {false};
static std::atomic<bool>  g_dn_held  {false};
static std::atomic<DWORD> g_up_t     {0};
static std::atomic<DWORD> g_dn_t     {0};
static std::atomic<bool>  g_act_held {false};
static std::atomic<DWORD> g_act_dn_t {0};
static std::atomic<DWORD> g_act_rel_t{0};
static std::atomic<bool>  g_pending  {false};

static std::atomic<DWORD> g_k_up  {VK_PRIOR};
static std::atomic<DWORD> g_k_dn  {VK_NEXT};
static std::atomic<DWORD> g_k_act {VK_OEM_PERIOD};

static int    g_det_target = 0;
static wchar_t g_det_msg[128] = {};

static bool g_ctrl_dn  = false;
static bool g_shift_dn = false;

// ════════════════════════════════════════════════════════════
//  GDI 资源（WM_DPICHANGED 时重新创建）
// ════════════════════════════════════════════════════════════
static HBRUSH g_br_bg  = nullptr;
static HBRUSH g_br_pan = nullptr;
static HBRUSH g_br_dim = nullptr;
static HFONT  g_f_lrg  = nullptr;
static HFONT  g_f_med  = nullptr;
static HFONT  g_f_reg  = nullptr;
static HFONT  g_f_mono = nullptr;
static HFONT  g_f_tiny = nullptr;

// ── 字体工厂（pt 为设计点数，按当前 g_dpi 换算像素高度）
static HFONT MakeFont(const wchar_t* face, int pt, int weight) {
    // CreateFontW height 参数为负值时表示字符高度（em height），
    // 正确做法：-MulDiv(pt, dpi, 72)  即 点数 → 像素
    return CreateFontW(
        -MulDiv(pt, (int)g_dpi, 72),
        0, 0, 0, weight, 0, 0, 0,
        DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,           // ClearType 消除锯齿
        DEFAULT_PITCH | FF_DONTCARE,
        face
    );
}

static void CreateFonts() {
    auto D = [](HGDIOBJ o) { if (o) DeleteObject(o); };
    D(g_f_lrg); D(g_f_med); D(g_f_reg); D(g_f_mono); D(g_f_tiny);
    g_f_lrg  = MakeFont(L"Microsoft YaHei UI", 13, FW_BOLD);
    g_f_med  = MakeFont(L"Microsoft YaHei UI", 10, FW_BOLD);
    g_f_reg  = MakeFont(L"Microsoft YaHei UI",  9, FW_NORMAL);
    g_f_mono = MakeFont(L"Consolas",             9, FW_NORMAL);
    g_f_tiny = MakeFont(L"Microsoft YaHei UI",   8, FW_NORMAL);
}

// ── 子控件位置（DPI 变化时重新布置）
static void PositionControls(HWND hw) {
    // 开关按钮
    SetWindowPos(GetDlgItem(hw, IDC_TOGGLE), nullptr,
        S(20), S(128), S(340), S(42), SWP_NOZORDER);
    // 三个检测按钮
    int bx = S(116), bw = S(52), bh = S(22);
    SetWindowPos(GetDlgItem(hw, IDC_DET_UP),  nullptr, bx, S(414), bw, bh, SWP_NOZORDER);
    SetWindowPos(GetDlgItem(hw, IDC_DET_DN),  nullptr, bx, S(442), bw, bh, SWP_NOZORDER);
    SetWindowPos(GetDlgItem(hw, IDC_DET_ACT), nullptr, bx, S(470), bw, bh, SWP_NOZORDER);
}

// ════════════════════════════════════════════════════════════
//  管理员检测
// ════════════════════════════════════════════════════════════
static bool IsAdmin() {
    BOOL ok = FALSE;
    SID_IDENTIFIER_AUTHORITY auth = SECURITY_NT_AUTHORITY;
    PSID sid = nullptr;
    if (AllocateAndInitializeSid(&auth, 2,
            SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
            0, 0, 0, 0, 0, 0, &sid)) {
        CheckTokenMembership(nullptr, sid, &ok);
        FreeSid(sid);
    }
    return !!ok;
}

// ════════════════════════════════════════════════════════════
//  VK 码 → 可读名称
// ════════════════════════════════════════════════════════════
static std::wstring VkName(DWORD vk) {
    UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    LONG lp = (LONG)(sc << 16);
    if ((vk >= VK_PRIOR && vk <= VK_DOWN) ||
        vk == VK_INSERT || vk == VK_DELETE)
        lp |= (1 << 24);
    wchar_t buf[64] = {};
    if (GetKeyNameTextW(lp, buf, 64) > 0) return buf;
    switch (vk) {
        case VK_OEM_PERIOD: return L".";
        case VK_OEM_COMMA:  return L",";
        case VK_OEM_MINUS:  return L"-";
        case VK_OEM_PLUS:   return L"=";
        case VK_ESCAPE:     return L"Escape";
        case VK_SPACE:      return L"Space";
        case VK_BACK:       return L"Backspace";
        case VK_TAB:        return L"Tab";
        case VK_RETURN:     return L"Enter";
        default: {
            wchar_t s[16];
            swprintf_s(s, 16, L"VK 0x%02X", vk);
            return s;
        }
    }
}

// ════════════════════════════════════════════════════════════
//  鼠标操作
// ════════════════════════════════════════════════════════════
static void MoveMouse(int dx, int dy) {
    POINT p;
    GetCursorPos(&p);
    SetCursorPos(
        max(0, min(GetSystemMetrics(SM_CXSCREEN) - 1, (int)p.x + dx)),
        max(0, min(GetSystemMetrics(SM_CYSCREEN) - 1, (int)p.y + dy))
    );
}

// ════════════════════════════════════════════════════════════
//  光标可见性管理（智能白板触摸模式下会隐藏系统光标）
// ════════════════════════════════════════════════════════════
static std::atomic<int> g_cursor_adj {0};

static void ForceCursorVisible() {
    int adj = 0, cnt;
    do { cnt = ShowCursor(TRUE); ++adj; } while (cnt < 0 && adj < 16);
    g_cursor_adj.store(adj);
}

static void RestoreCursorVisible() {
    int adj = g_cursor_adj.exchange(0);
    for (int i = 0; i < adj; ++i) ShowCursor(FALSE);
}

static void EnsureCursorVisible() {
    CURSORINFO ci = {};
    ci.cbSize = sizeof(ci);
    if (GetCursorInfo(&ci) && !(ci.flags & CURSOR_SHOWING)) {
        ShowCursor(TRUE);
        g_cursor_adj.fetch_add(1);
    }
}

// ════════════════════════════════════════════════════════════
//  移动线程
// ════════════════════════════════════════════════════════════
static DWORD WINAPI MoveThread(LPVOID) {
    int visCheck = 0;
    const int VIS_INTERVAL = 1000 / (int)TICK_MS;
    for (;;) {
        if (g_enabled.load()) {
            if (++visCheck >= VIS_INTERVAL) { visCheck = 0; EnsureCursorVisible(); }
            if (g_up_held.load() || g_dn_held.load()) {
                DWORD now = GetTickCount();
                int dx = 0, dy = 0;
                auto step = [&](DWORD t0) -> int {
                    float t = min(1.0f, (float)(now - t0) / (float)ACCEL_MS);
                    return (int)(STEP_MIN + t * (STEP_MAX - STEP_MIN));
                };
                if (g_up_held.load()) { int s=step(g_up_t.load()); if(g_horiz.load()) dx-=s; else dy-=s; }
                if (g_dn_held.load()) { int s=step(g_dn_t.load()); if(g_horiz.load()) dx+=s; else dy+=s; }
                if (dx || dy) MoveMouse(dx, dy);
            }
        } else { visCheck = 0; }
        Sleep(TICK_MS);
    }
    return 0;
}

// ════════════════════════════════════════════════════════════
//  方框键多功能处理
// ════════════════════════════════════════════════════════════
static void OnActRelease() {
    DWORD now  = GetTickCount();
    DWORD held = now - g_act_dn_t.load();
    if (held >= LONG_PRESS) {
        if (g_pending.load()) { KillTimer(g_hwnd, TID_CLICK); g_pending = false; }
        mouse_event(MOUSEEVENTF_RIGHTDOWN, 0, 0, 0, 0);
        mouse_event(MOUSEEVENTF_RIGHTUP,   0, 0, 0, 0);
    } else if (g_pending.load() && (now - g_act_rel_t.load()) < DBL_GAP) {
        KillTimer(g_hwnd, TID_CLICK);
        g_pending = false;
        g_horiz   = !g_horiz.load();
        PostMessage(g_hwnd, WM_REFRESH, 0, 0);
    } else {
        if (g_pending.load()) KillTimer(g_hwnd, TID_CLICK);
        g_pending    = true;
        g_act_rel_t  = now;
        SetTimer(g_hwnd, TID_CLICK, DBL_GAP, nullptr);
    }
}

// ════════════════════════════════════════════════════════════
//  低级键盘钩子
// ════════════════════════════════════════════════════════════
static LRESULT CALLBACK KbHook(int code, WPARAM wp, LPARAM lp) {
    if (code < 0) return CallNextHookEx(g_hook, code, wp, lp);
    auto* kb = reinterpret_cast<KBDLLHOOKSTRUCT*>(lp);
    DWORD vk = kb->vkCode;
    bool  dn = (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN);

    if (vk==VK_LCONTROL||vk==VK_RCONTROL||vk==VK_CONTROL) { g_ctrl_dn=dn; return CallNextHookEx(g_hook,code,wp,lp); }
    if (vk==VK_LSHIFT  ||vk==VK_RSHIFT  ||vk==VK_SHIFT  ) { g_shift_dn=dn;return CallNextHookEx(g_hook,code,wp,lp); }

    if (vk==VK_F8 && g_ctrl_dn && g_shift_dn && dn) {
        PostMessage(g_hwnd, WM_COMMAND, IDC_TOGGLE, 0);
        return 1;
    }
    if (g_det_target != 0 && dn) {
        int tgt = g_det_target; g_det_target = 0;
        switch (tgt) { case 1: g_k_up=vk; break; case 2: g_k_dn=vk; break; case 3: g_k_act=vk; break; }
        PostMessage(g_hwnd, WM_REFRESH, 1, (LPARAM)vk);
        return CallNextHookEx(g_hook, code, wp, lp);
    }
    if (!g_enabled.load()) return CallNextHookEx(g_hook, code, wp, lp);

    DWORD ku=g_k_up.load(), kd=g_k_dn.load(), ka=g_k_act.load();
    if (vk==ku) { if(dn&&!g_up_held.load()){g_up_held=true;g_up_t=GetTickCount();} if(!dn)g_up_held=false; return 1; }
    if (vk==kd) { if(dn&&!g_dn_held.load()){g_dn_held=true;g_dn_t=GetTickCount();} if(!dn)g_dn_held=false; return 1; }
    if (vk==ka) { if(dn&&!g_act_held.load()){g_act_held=true;g_act_dn_t=GetTickCount();} if(!dn&&g_act_held.load()){g_act_held=false;OnActRelease();} return 1; }

    return CallNextHookEx(g_hook, code, wp, lp);
}

// ════════════════════════════════════════════════════════════
//  绘制辅助
// ════════════════════════════════════════════════════════════
static void FillR(HDC dc, int x, int y, int w, int h, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    RECT   r = {x, y, x+w, y+h};
    FillRect(dc, &r, b);
    DeleteObject(b);
}

static void DTxt(HDC dc, const wchar_t* s, int x, int y, int w, int h,
                 COLORREF fg, HFONT f,
                 UINT fmt = DT_LEFT | DT_VCENTER | DT_SINGLELINE) {
    SetTextColor(dc, fg);
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, f);
    RECT r = {x, y, x+w, y+h};
    DrawTextW(dc, s, -1, &r, fmt);
}

static void DrawCard(HDC dc, int x, int y, int w, int h) {
    FillR(dc, x,   y,   w,   h,   C_BORD);
    FillR(dc, x+1, y+1, w-2, h-2, C_PANEL);
}

// ════════════════════════════════════════════════════════════
//  owner-drawn 按钮
// ════════════════════════════════════════════════════════════
static void DrawButton(DRAWITEMSTRUCT* di) {
    HDC  dc  = di->hDC;
    RECT rc  = di->rcItem;
    int  id  = (int)di->CtlID;
    int  bw  = rc.right - rc.left;
    int  bh  = rc.bottom - rc.top;
    bool dwn = !!(di->itemState & ODS_SELECTED);

    if (id == IDC_TOGGLE) {
        bool on = g_enabled.load();
        COLORREF bg = dwn ? (on ? RGB(200,55,50) : RGB(40,150,55))
                          : (on ? C_RED           : C_GREEN);
        FillR(dc, rc.left, rc.top, bw, bh, bg);
        const wchar_t* txt = on ? L"■   关闭控制" : L"▶   开启控制";
        DTxt(dc, txt, rc.left, rc.top, bw, bh, C_BG, g_f_med,
             DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    } else {
        FillR(dc, rc.left, rc.top, bw, bh, dwn ? C_DIM : C_DARK);
        HPEN pen = CreatePen(PS_SOLID, 1, C_BORD);
        HPEN old = (HPEN)SelectObject(dc, pen);
        SelectObject(dc, GetStockObject(NULL_BRUSH));
        Rectangle(dc, rc.left, rc.top, rc.right, rc.bottom);
        SelectObject(dc, old); DeleteObject(pen);
        wchar_t buf[32] = {};
        GetWindowTextW(di->hwndItem, buf, 32);
        DTxt(dc, buf, rc.left, rc.top, bw, bh, C_SUB, g_f_tiny,
             DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
}

// ════════════════════════════════════════════════════════════
//  窗口过程
// ════════════════════════════════════════════════════════════
static LRESULT CALLBACK WndProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {

    case WM_CREATE: {
        g_br_bg  = CreateSolidBrush(C_BG);
        g_br_pan = CreateSolidBrush(C_PANEL);
        g_br_dim = CreateSolidBrush(C_DIM);
        CreateFonts();

        HINSTANCE hi = GetModuleHandleW(nullptr);
        CreateWindowW(L"BUTTON", L"",
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            S(20), S(128), S(340), S(42),
            hw, (HMENU)(UINT_PTR)IDC_TOGGLE, hi, nullptr);
        CreateWindowW(L"BUTTON", L"检 测",
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            S(116), S(414), S(52), S(22),
            hw, (HMENU)(UINT_PTR)IDC_DET_UP, hi, nullptr);
        CreateWindowW(L"BUTTON", L"检 测",
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            S(116), S(442), S(52), S(22),
            hw, (HMENU)(UINT_PTR)IDC_DET_DN, hi, nullptr);
        CreateWindowW(L"BUTTON", L"检 测",
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            S(116), S(470), S(52), S(22),
            hw, (HMENU)(UINT_PTR)IDC_DET_ACT, hi, nullptr);

        g_hook = SetWindowsHookExW(WH_KEYBOARD_LL, KbHook, hi, 0);
        CloseHandle(CreateThread(nullptr, 0, MoveThread, nullptr, 0, nullptr));
        return 0;
    }

    // ── DPI 变化（窗口移到不同 DPI 显示器）─────────────────
    case WM_DPICHANGED: {
        g_dpi = HIWORD(wp);                         // 新 DPI
        CreateFonts();                               // 重建所有字体
        PositionControls(hw);                        // 重新布置子控件

        // 使用系统建议的新窗口矩形直接 resize
        RECT* r = reinterpret_cast<RECT*>(lp);
        SetWindowPos(hw, nullptr,
            r->left, r->top,
            r->right - r->left, r->bottom - r->top,
            SWP_NOZORDER | SWP_NOACTIVATE);
        InvalidateRect(hw, nullptr, TRUE);
        return 0;
    }

    case WM_ERASEBKGND: {
        RECT r; GetClientRect(hw, &r);
        FillRect((HDC)wp, &r, g_br_bg);
        return 1;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hw, &ps);

        // ─ 标题 ─────────────────────────────────────────────
        DTxt(dc, L"⌨  翻页笔鼠标控制器",
             S(20), S(14), S(300), S(28), C_TEXT, g_f_lrg);
        DTxt(dc, L"Clicker → Mouse Bridge  v1.2",
             S(24), S(40), S(320), S(18), C_SUB, g_f_mono);

        // ─ 状态卡片  y=62 h=60 ──────────────────────────────
        DrawCard(dc, S(20), S(62), S(340), S(60));

        COLORREF dotC = g_enabled.load() ? C_GREEN : C_RED;
        {
            HBRUSH db = CreateSolidBrush(dotC);
            HBRUSH ob = (HBRUSH)SelectObject(dc, db);
            SelectObject(dc, GetStockObject(NULL_PEN));
            Ellipse(dc, S(34), S(75), S(46), S(87));
            SelectObject(dc, ob); DeleteObject(db);
        }

        DTxt(dc, g_enabled.load() ? L"已开启" : L"已关闭",
             S(54), S(66), S(180), S(26),
             g_enabled.load() ? C_GREEN : C_TEXT, g_f_med);
        {
            wchar_t ax[64];
            swprintf_s(ax, L"移动方向  %s", g_horiz.load() ? L"↔  水平" : L"↕  垂直");
            DTxt(dc, ax, S(54), S(92), S(240), S(22), C_SUB, g_f_reg);
        }

        // ─ 操作说明卡片  y=178 h=178 ────────────────────────
        DrawCard(dc, S(20), S(178), S(340), S(178));
        DTxt(dc, L"操作说明", S(34), S(190), S(200), S(22), C_TEXT, g_f_med);

        struct { const wchar_t* k; const wchar_t* a; } G[] = {
            { L"上键",              L"向上 / 向左移动（按住自动加速）" },
            { L"下键",              L"向下 / 向右移动（按住自动加速）" },
            { L"方框键  × 1",      L"鼠标左键单击"                    },
            { L"方框键  × 2",      L"切换移动轴  ↕ / ↔"               },
            { L"方框键  长按",      L"鼠标右键单击"                    },
            { L"Ctrl+Shift+F8",    L"全局开启 / 关闭"                  },
        };
        for (int i = 0; i < 6; ++i) {
            int y = S(218 + i * 22);
            DTxt(dc, G[i].k, S(34),  y, S(112), S(20), C_BLUE, g_f_mono);
            DTxt(dc, G[i].a, S(150), y, S(196), S(20), C_SUB,  g_f_reg);
        }

        // ─ 绑定检测卡片  y=362 h=172 ────────────────────────
        DrawCard(dc, S(20), S(362), S(340), S(172));
        DTxt(dc, L"按键绑定 / 按键检测", S(34), S(374), S(300), S(22), C_TEXT, g_f_med);
        DTxt(dc, L"不知道翻页笔发哪个键？点击\"检测\"后按一下翻页笔对应键",
             S(34), S(395), S(310), S(18), C_SUB, g_f_tiny);

        struct { const wchar_t* lbl; DWORD vk; } B[] = {
            { L"上键",   g_k_up.load()  },
            { L"下键",   g_k_dn.load()  },
            { L"方框键", g_k_act.load() },
        };
        int by[] = { S(414), S(442), S(470) };
        for (int i = 0; i < 3; ++i) {
            DTxt(dc, B[i].lbl, S(34), by[i], S(76), S(22), C_SUB, g_f_reg);
            std::wstring kn = VkName(B[i].vk);
            DTxt(dc, kn.c_str(), S(178), by[i], S(160), S(22), C_AMBER, g_f_mono);
        }

        if (g_det_msg[0])
            DTxt(dc, g_det_msg, S(34), S(496), S(310), S(22), C_TEAL, g_f_reg);
        else if (g_det_target != 0)
            DTxt(dc, L"⋯  请按下翻页笔的对应键", S(34), S(496), S(310), S(22), C_AMBER, g_f_reg);

        DTxt(dc, L"WH_KEYBOARD_LL 低级钩子  ·  需以管理员身份运行",
             S(20), S(548), S(340), S(18), C_DIM, g_f_tiny,
             DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        EndPaint(hw, &ps);
        return 0;
    }

    case WM_DRAWITEM: {
        auto* di = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
        if (di->CtlType == ODT_BUTTON) DrawButton(di);
        return TRUE;
    }

    case WM_TIMER: {
        if (wp == TID_CLICK) {
            KillTimer(hw, TID_CLICK);
            if (g_pending.load()) {
                g_pending = false;
                mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
                mouse_event(MOUSEEVENTF_LEFTUP,   0, 0, 0, 0);
            }
        } else if (wp == TID_CLR_MSG) {
            KillTimer(hw, TID_CLR_MSG);
            g_det_msg[0] = 0;
            InvalidateRect(hw, nullptr, FALSE);
        }
        return 0;
    }

    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == IDC_TOGGLE) {
            if (g_enabled.load()) {
                g_up_held = g_dn_held = g_act_held = false;
                g_pending = false;
                KillTimer(hw, TID_CLICK);
                RestoreCursorVisible();
            } else {
                ForceCursorVisible();
            }
            g_enabled = !g_enabled.load();
            InvalidateRect(GetDlgItem(hw, IDC_TOGGLE), nullptr, FALSE);
            InvalidateRect(hw, nullptr, FALSE);
        } else if (id == IDC_DET_UP || id == IDC_DET_DN || id == IDC_DET_ACT) {
            g_det_target = (id == IDC_DET_UP) ? 1 : (id == IDC_DET_DN) ? 2 : 3;
            g_det_msg[0] = 0;
            KillTimer(hw, TID_CLR_MSG);
            InvalidateRect(hw, nullptr, FALSE);
        }
        return 0;
    }

    case WM_REFRESH: {
        if (wp == 1) {
            std::wstring kn = VkName((DWORD)lp);
            swprintf_s(g_det_msg, 128, L"✓  已更新  →  %s", kn.c_str());
            SetTimer(hw, TID_CLR_MSG, 3000, nullptr);
        }
        InvalidateRect(GetDlgItem(hw, IDC_TOGGLE), nullptr, FALSE);
        InvalidateRect(hw, nullptr, FALSE);
        return 0;
    }

    case WM_DESTROY: {
        if (g_hook) { UnhookWindowsHookEx(g_hook); g_hook = nullptr; }
        auto D = [](HGDIOBJ o) { if (o) DeleteObject(o); };
        D(g_br_bg); D(g_br_pan); D(g_br_dim);
        D(g_f_lrg); D(g_f_med);  D(g_f_reg); D(g_f_mono); D(g_f_tiny);
        PostQuitMessage(0);
        return 0;
    }

    } // switch
    return DefWindowProcW(hw, msg, wp, lp);
}

// ════════════════════════════════════════════════════════════
//  程序入口
// ════════════════════════════════════════════════════════════
int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, LPWSTR, int) {

    // ── 1. DPI 感知（必须在任何窗口创建之前）────────────────
    SetDpiAwareness();

    // ── 2. 管理员权限检查 ─────────────────────────────────
    if (!IsAdmin()) {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        ShellExecuteW(nullptr, L"runas", path, nullptr, nullptr, SW_NORMAL);
        return 0;
    }

    // ── 3. 临时 DC 取当前系统 DPI（用于创建窗口时的尺寸计算）
    {
        HDC dc = GetDC(nullptr);
        g_dpi = (UINT)GetDeviceCaps(dc, LOGPIXELSX);
        ReleaseDC(nullptr, dc);
        if (g_dpi == 0) g_dpi = 96;
    }

    // ── 4. 注册窗口类 ─────────────────────────────────────
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hi;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(C_BG);
    wc.lpszClassName = L"PenMouseCtrl";
    wc.hIcon         = LoadIconW(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);

    // ── 5. 创建窗口（尺寸已按 DPI 缩放）──────────────────
    RECT wr = {0, 0, S(BASE_W), S(BASE_H)};
    AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW & ~(WS_MAXIMIZEBOX | WS_THICKFRAME), FALSE);
    int ww = wr.right  - wr.left;
    int wh = wr.bottom - wr.top;
    int wx = (GetSystemMetrics(SM_CXSCREEN) - ww) / 2;
    int wy = (GetSystemMetrics(SM_CYSCREEN) - wh) / 2;

    g_hwnd = CreateWindowExW(0,
        L"PenMouseCtrl", L"翻页笔鼠标控制器",
        WS_OVERLAPPEDWINDOW & ~(WS_MAXIMIZEBOX | WS_THICKFRAME),
        wx, wy, ww, wh,
        nullptr, nullptr, hi, nullptr);

    // ── 6. 拿窗口真实 DPI（Per-Monitor 下可能与系统 DPI 不同）
    UINT wndDpi = GetWndDpi(g_hwnd);
    if (wndDpi != g_dpi) {
        // 若不一致说明窗口落在了不同 DPI 的显示器，触发一次 WM_DPICHANGED 处理
        g_dpi = wndDpi;
        // 重新调整窗口大小
        RECT wr2 = {0, 0, S(BASE_W), S(BASE_H)};
        AdjustWindowRect(&wr2, WS_OVERLAPPEDWINDOW & ~(WS_MAXIMIZEBOX | WS_THICKFRAME), FALSE);
        int ww2 = wr2.right - wr2.left, wh2 = wr2.bottom - wr2.top;
        int wx2 = (GetSystemMetrics(SM_CXSCREEN) - ww2) / 2;
        int wy2 = (GetSystemMetrics(SM_CYSCREEN) - wh2) / 2;
        SetWindowPos(g_hwnd, nullptr, wx2, wy2, ww2, wh2, SWP_NOZORDER);
        CreateFonts();
        PositionControls(g_hwnd);
    }

    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);

    // ── 7. 消息循环 ───────────────────────────────────────
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return (int)m.wParam;
}
