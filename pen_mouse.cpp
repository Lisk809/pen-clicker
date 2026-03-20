// ============================================================
//  pen_mouse.cpp  —  翻页笔鼠标控制器  v1.0
// ============================================================
//
//  编译 (MSVC):
//    cl pen_mouse.cpp user32.lib gdi32.lib shell32.lib
//       /link /SUBSYSTEM:WINDOWS /ENTRY:wWinMainCRTStartup
//
//  编译 (MinGW / GCC):
//    g++ -O2 -o pen_mouse.exe pen_mouse.cpp
//        -lgdi32 -luser32 -lshell32 -mwindows -municode
//
//  需要管理员权限（WH_KEYBOARD_LL 低级钩子）
//  程序启动时若未以管理员运行，自动弹出 UAC 提升请求。
//
// ────────────────────────────────────────────────────────────
//  默认按键映射（可在程序内"按键检测"区域修改）:
//    上键   →  Page Up
//    下键   →  Page Down
//    方框键 →  .（句点）
//
//  操作（模式开启后）:
//    上键 / 下键 按住  → 移动鼠标（持续加速）
//    方框键 单击       → 鼠标左键
//    方框键 双击       → 切换移动轴 ↕垂直 / ↔水平
//    方框键 长按       → 鼠标右键
//    Ctrl+Shift+F8    → 全局开启 / 关闭
// ============================================================

#define NOMINMAX        // 禁止 windows.h 定义 min/max 宏
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

// ════════════════════════════════════════════════════════════
//  布局常量  (客户区像素)
// ════════════════════════════════════════════════════════════
static const int CW = 380;   // 客户区宽
static const int CH = 568;   // 客户区高

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
//  控件 ID
// ════════════════════════════════════════════════════════════
#define IDC_TOGGLE   101
#define IDC_DET_UP   102
#define IDC_DET_DN   103
#define IDC_DET_ACT  104

// ════════════════════════════════════════════════════════════
//  自定义消息 / 定时器 ID
// ════════════════════════════════════════════════════════════
#define WM_REFRESH     (WM_USER + 1)   // 刷新界面（从钩子 PostMessage）
#define TID_CLICK        1              // 等待双击 → 单击触发
#define TID_CLR_MSG      2              // 清除检测提示文字

// ════════════════════════════════════════════════════════════
//  移动参数
// ════════════════════════════════════════════════════════════
static const int   STEP_MIN  = 18;     // 初始步长 px
static const int   STEP_MAX  = 75;     // 加速后最大步长 px
static const DWORD ACCEL_MS  = 500;    // 加速时间 ms
static const DWORD TICK_MS   = 16;     // 移动线程刷新间隔 (~60 fps)

// ════════════════════════════════════════════════════════════
//  方框键多功能判定参数
// ════════════════════════════════════════════════════════════
static const DWORD LONG_PRESS = 600;   // 长按判定时间 ms
static const DWORD DBL_GAP    = 350;   // 双击最大间隔 ms

// ════════════════════════════════════════════════════════════
//  全局状态
// ════════════════════════════════════════════════════════════
static HWND  g_hwnd = nullptr;
static HHOOK g_hook = nullptr;

// 鼠标控制状态（移动线程 + 钩子线程 共享）
static std::atomic<bool>  g_enabled  {false};
static std::atomic<bool>  g_horiz    {false};   // false=垂直, true=水平
static std::atomic<bool>  g_up_held  {false};
static std::atomic<bool>  g_dn_held  {false};
static std::atomic<DWORD> g_up_t     {0};       // 上键按下时刻
static std::atomic<DWORD> g_dn_t     {0};       // 下键按下时刻
static std::atomic<bool>  g_act_held {false};
static std::atomic<DWORD> g_act_dn_t {0};       // 方框键按下时刻
static std::atomic<DWORD> g_act_rel_t{0};       // 方框键上次释放时刻
static std::atomic<bool>  g_pending  {false};   // 等待双击确认中

// 按键绑定（钩子线程 write；移动线程 read）
static std::atomic<DWORD> g_k_up  {VK_PRIOR};        // Page Up
static std::atomic<DWORD> g_k_dn  {VK_NEXT};          // Page Down
static std::atomic<DWORD> g_k_act {VK_OEM_PERIOD};    // .

// 按键检测模式（仅主线程访问，无需 atomic）
static int    g_det_target = 0;         // 1=上键 2=下键 3=方框键
static wchar_t g_det_msg[128] = {};     // 检测结果提示

// 钩子侧修饰键追踪（WH_KEYBOARD_LL 回调运行在主线程）
static bool g_ctrl_dn  = false;
static bool g_shift_dn = false;

// ════════════════════════════════════════════════════════════
//  GDI 资源
// ════════════════════════════════════════════════════════════
static HBRUSH g_br_bg  = nullptr;
static HBRUSH g_br_pan = nullptr;
static HBRUSH g_br_dim = nullptr;
static HFONT  g_f_lrg  = nullptr;   // 13pt Bold 标题
static HFONT  g_f_med  = nullptr;   // 10pt Bold 卡片标题/按钮
static HFONT  g_f_reg  = nullptr;   // 9pt  正文
static HFONT  g_f_mono = nullptr;   // 9pt  Consolas
static HFONT  g_f_tiny = nullptr;   // 8pt  辅助文字

// ════════════════════════════════════════════════════════════
//  辅助：管理员检测
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
//  辅助：VK 码 → 可读名称
// ════════════════════════════════════════════════════════════
static std::wstring VkName(DWORD vk) {
    UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    LONG lp = (LONG)(sc << 16);
    // 扩展键需要设置 extended bit
    if ((vk >= VK_PRIOR && vk <= VK_DOWN) ||
        vk == VK_INSERT || vk == VK_DELETE)
        lp |= (1 << 24);
    wchar_t buf[64] = {};
    if (GetKeyNameTextW(lp, buf, 64) > 0)
        return buf;
    // 回退
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
//  移动线程
// ════════════════════════════════════════════════════════════
static DWORD WINAPI MoveThread(LPVOID) {
    for (;;) {
        if (g_enabled.load() && (g_up_held.load() || g_dn_held.load())) {
            DWORD now = GetTickCount();
            int dx = 0, dy = 0;

            auto calcStep = [&](DWORD t0) -> int {
                float t = min(1.0f, (float)(now - t0) / (float)ACCEL_MS);
                return (int)(STEP_MIN + t * (STEP_MAX - STEP_MIN));
            };

            if (g_up_held.load()) {
                int s = calcStep(g_up_t.load());
                if (g_horiz.load()) dx -= s; else dy -= s;
            }
            if (g_dn_held.load()) {
                int s = calcStep(g_dn_t.load());
                if (g_horiz.load()) dx += s; else dy += s;
            }
            if (dx || dy) MoveMouse(dx, dy);
        }
        Sleep(TICK_MS);
    }
    return 0;
}

// ════════════════════════════════════════════════════════════
//  方框键释放处理
//  (WH_KEYBOARD_LL 回调运行在主线程，可直接调用 SetTimer)
// ════════════════════════════════════════════════════════════
static void OnActRelease() {
    DWORD now  = GetTickCount();
    DWORD held = now - g_act_dn_t.load();

    if (held >= LONG_PRESS) {
        // 长按 → 右键
        if (g_pending.load()) { KillTimer(g_hwnd, TID_CLICK); g_pending = false; }
        mouse_event(MOUSEEVENTF_RIGHTDOWN, 0, 0, 0, 0);
        mouse_event(MOUSEEVENTF_RIGHTUP,   0, 0, 0, 0);

    } else if (g_pending.load() && (now - g_act_rel_t.load()) < DBL_GAP) {
        // 双击 → 切换轴
        KillTimer(g_hwnd, TID_CLICK);
        g_pending = false;
        g_horiz   = !g_horiz.load();
        PostMessage(g_hwnd, WM_REFRESH, 0, 0);

    } else {
        // 可能是单击，等双击窗口
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
    if (code < 0)
        return CallNextHookEx(g_hook, code, wp, lp);

    auto* kb  = reinterpret_cast<KBDLLHOOKSTRUCT*>(lp);
    DWORD vk  = kb->vkCode;
    bool  dn  = (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN);

    // ── 修饰键追踪（始终放行）──────────────────────────────
    if (vk==VK_LCONTROL||vk==VK_RCONTROL||vk==VK_CONTROL) {
        g_ctrl_dn  = dn;
        return CallNextHookEx(g_hook, code, wp, lp);
    }
    if (vk==VK_LSHIFT||vk==VK_RSHIFT||vk==VK_SHIFT) {
        g_shift_dn = dn;
        return CallNextHookEx(g_hook, code, wp, lp);
    }

    // ── 全局开关 Ctrl+Shift+F8（始终有效，拦截）────────────
    if (vk==VK_F8 && g_ctrl_dn && g_shift_dn && dn) {
        PostMessage(g_hwnd, WM_COMMAND, IDC_TOGGLE, 0);
        return 1;
    }

    // ── 按键检测模式：捕获下一个非修饰按键 ────────────────
    if (g_det_target != 0 && dn) {
        int tgt    = g_det_target;
        g_det_target = 0;
        switch (tgt) {
            case 1: g_k_up  = vk; break;
            case 2: g_k_dn  = vk; break;
            case 3: g_k_act = vk; break;
        }
        // 通知主线程更新 UI（wParam=1 表示检测完成，lParam=vk）
        PostMessage(g_hwnd, WM_REFRESH, 1, (LPARAM)vk);
        return CallNextHookEx(g_hook, code, wp, lp);  // 放行
    }

    // ── 模式未开启：一律放行 ──────────────────────────────
    if (!g_enabled.load())
        return CallNextHookEx(g_hook, code, wp, lp);

    DWORD ku = g_k_up.load(), kd = g_k_dn.load(), ka = g_k_act.load();

    // ── 上键 ─────────────────────────────────────────────
    if (vk == ku) {
        if  (dn && !g_up_held.load()) { g_up_held=true;  g_up_t=GetTickCount(); }
        if  (!dn)                     { g_up_held=false; }
        return 1;   // 拦截
    }
    // ── 下键 ─────────────────────────────────────────────
    if (vk == kd) {
        if  (dn && !g_dn_held.load()) { g_dn_held=true;  g_dn_t=GetTickCount(); }
        if  (!dn)                     { g_dn_held=false; }
        return 1;
    }
    // ── 方框键 ───────────────────────────────────────────
    if (vk == ka) {
        if  (dn && !g_act_held.load()) { g_act_held=true; g_act_dn_t=GetTickCount(); }
        if  (!dn && g_act_held.load()) { g_act_held=false; OnActRelease(); }
        return 1;
    }

    return CallNextHookEx(g_hook, code, wp, lp);
}

// ════════════════════════════════════════════════════════════
//  绘制辅助函数
// ════════════════════════════════════════════════════════════
static void FillR(HDC dc, int x, int y, int w, int h, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    RECT   r = {x, y, x+w, y+h};
    FillRect(dc, &r, b);
    DeleteObject(b);
}

// 文字绘制（背景透明）
static void DTxt(HDC dc, const wchar_t* s, int x, int y, int w, int h,
                 COLORREF fg, HFONT f,
                 UINT fmt = DT_LEFT | DT_VCENTER | DT_SINGLELINE) {
    SetTextColor(dc, fg);
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, f);
    RECT r = {x, y, x+w, y+h};
    DrawTextW(dc, s, -1, &r, fmt);
}

// 卡片（带边框的面板）
static void DrawCard(HDC dc, int x, int y, int w, int h) {
    FillR(dc, x,   y,   w,   h,   C_BORD);
    FillR(dc, x+1, y+1, w-2, h-2, C_PANEL);
}

// ════════════════════════════════════════════════════════════
//  绘制 owner-drawn 按钮（WM_DRAWITEM）
// ════════════════════════════════════════════════════════════
static void DrawButton(DRAWITEMSTRUCT* di) {
    HDC   dc  = di->hDC;
    RECT  rc  = di->rcItem;
    int   id  = (int)di->CtlID;
    int   bw  = rc.right  - rc.left;
    int   bh  = rc.bottom - rc.top;
    bool  dwn = !!(di->itemState & ODS_SELECTED);

    if (id == IDC_TOGGLE) {
        bool on  = g_enabled.load();
        COLORREF bg = dwn ? (on ? RGB(200,55,50) : RGB(40,150,55))
                          : (on ? C_RED           : C_GREEN);
        FillR(dc, rc.left, rc.top, bw, bh, bg);
        const wchar_t* txt = on ? L"■   关闭控制" : L"▶   开启控制";
        DTxt(dc, txt, rc.left, rc.top, bw, bh, C_BG, g_f_med,
             DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    } else {
        // 检测按钮
        COLORREF bg = dwn ? C_DIM : C_DARK;
        FillR(dc, rc.left, rc.top, bw, bh, bg);
        // 边框
        HPEN pen = CreatePen(PS_SOLID, 1, C_BORD);
        HPEN old = (HPEN)SelectObject(dc, pen);
        SelectObject(dc, GetStockObject(NULL_BRUSH));
        Rectangle(dc, rc.left, rc.top, rc.right, rc.bottom);
        SelectObject(dc, old);
        DeleteObject(pen);
        // 文字
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

    // ── 初始化 ────────────────────────────────────────────
    case WM_CREATE: {
        // GDI 刷子
        g_br_bg  = CreateSolidBrush(C_BG);
        g_br_pan = CreateSolidBrush(C_PANEL);
        g_br_dim = CreateSolidBrush(C_DIM);

        // 字体工厂
        auto MF = [](const wchar_t* face, int pt, int wt) -> HFONT {
            return CreateFontW(-pt, 0, 0, 0, wt, 0, 0, 0,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
        };
        g_f_lrg  = MF(L"Microsoft YaHei UI", 13, FW_BOLD);
        g_f_med  = MF(L"Microsoft YaHei UI", 10, FW_BOLD);
        g_f_reg  = MF(L"Microsoft YaHei UI",  9, FW_NORMAL);
        g_f_mono = MF(L"Consolas",             9, FW_NORMAL);
        g_f_tiny = MF(L"Microsoft YaHei UI",   8, FW_NORMAL);

        HINSTANCE hi = GetModuleHandleW(nullptr);

        // 开关按钮
        CreateWindowW(L"BUTTON", L"",
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            20, 128, 340, 42, hw, (HMENU)(UINT_PTR)IDC_TOGGLE, hi, nullptr);

        // 检测按钮（在绑定卡片中）
        // 上键检测  y=414
        CreateWindowW(L"BUTTON", L"检 测",
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            116, 414, 52, 22, hw, (HMENU)(UINT_PTR)IDC_DET_UP, hi, nullptr);
        // 下键检测  y=442
        CreateWindowW(L"BUTTON", L"检 测",
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            116, 442, 52, 22, hw, (HMENU)(UINT_PTR)IDC_DET_DN, hi, nullptr);
        // 方框键检测 y=470
        CreateWindowW(L"BUTTON", L"检 测",
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            116, 470, 52, 22, hw, (HMENU)(UINT_PTR)IDC_DET_ACT, hi, nullptr);

        // 安装低级键盘钩子
        g_hook = SetWindowsHookExW(WH_KEYBOARD_LL, KbHook, hi, 0);

        // 启动移动线程（设为守护线程，随进程退出）
        CloseHandle(CreateThread(nullptr, 0, MoveThread, nullptr, 0, nullptr));
        return 0;
    }

    // ── 背景擦除 ──────────────────────────────────────────
    case WM_ERASEBKGND: {
        RECT r;
        GetClientRect(hw, &r);
        FillRect((HDC)wp, &r, g_br_bg);
        return 1;
    }

    // ── 主绘制 ────────────────────────────────────────────
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hw, &ps);

        // ─ 标题 ─────────────────────────────────────────
        DTxt(dc, L"⌨  翻页笔鼠标控制器",
             20, 14, 300, 28, C_TEXT, g_f_lrg);
        DTxt(dc, L"Clicker → Mouse Bridge  v1.0",
             24, 40, 320, 18, C_SUB, g_f_mono);

        // ─ 状态卡片  y=62 h=60 ──────────────────────────
        DrawCard(dc, 20, 62, 340, 60);

        // 状态指示圆点
        {
            COLORREF dotC = g_enabled.load() ? C_GREEN : C_RED;
            HBRUSH db = CreateSolidBrush(dotC);
            HBRUSH ob = (HBRUSH)SelectObject(dc, db);
            SelectObject(dc, GetStockObject(NULL_PEN));
            Ellipse(dc, 34, 75, 46, 87);
            SelectObject(dc, ob);
            DeleteObject(db);
        }

        DTxt(dc, g_enabled.load() ? L"已开启" : L"已关闭",
             54, 66, 180, 26,
             g_enabled.load() ? C_GREEN : C_TEXT, g_f_med);

        {
            wchar_t ax[64];
            swprintf_s(ax, L"移动方向  %s",
                       g_horiz.load() ? L"↔  水平" : L"↕  垂直");
            DTxt(dc, ax, 54, 92, 240, 22, C_SUB, g_f_reg);
        }

        // ─ 操作说明卡片  y=178 h=178 ────────────────────
        DrawCard(dc, 20, 178, 340, 178);
        DTxt(dc, L"操作说明", 34, 190, 200, 22, C_TEXT, g_f_med);

        struct { const wchar_t* k; const wchar_t* a; } G[] = {
            { L"上键",              L"向上 / 向左移动（按住自动加速）" },
            { L"下键",              L"向下 / 向右移动（按住自动加速）" },
            { L"方框键  × 1",      L"鼠标左键单击"                    },
            { L"方框键  × 2",      L"切换移动轴  ↕ / ↔"               },
            { L"方框键  长按",      L"鼠标右键单击"                    },
            { L"Ctrl+Shift+F8",    L"全局开启 / 关闭"                  },
        };
        for (int i = 0; i < 6; ++i) {
            int y = 218 + i * 22;
            DTxt(dc, G[i].k, 34,  y, 112, 20, C_BLUE, g_f_mono);
            DTxt(dc, G[i].a, 150, y, 196, 20, C_SUB,  g_f_reg);
        }

        // ─ 绑定检测卡片  y=362 h=172 ────────────────────
        DrawCard(dc, 20, 362, 340, 172);
        DTxt(dc, L"按键绑定 / 按键检测", 34, 374, 300, 22, C_TEXT, g_f_med);
        DTxt(dc, L"不知道翻页笔发哪个键？点击\"检测\"后按一下翻页笔对应键",
             34, 395, 310, 18, C_SUB, g_f_tiny);

        // 三行绑定（标签 | 检测按钮(控件) | 当前键名）
        struct { const wchar_t* lbl; DWORD vk; } B[] = {
            { L"上键",    g_k_up.load()  },
            { L"下键",    g_k_dn.load()  },
            { L"方框键",  g_k_act.load() },
        };
        int by[] = { 414, 442, 470 };
        for (int i = 0; i < 3; ++i) {
            DTxt(dc, B[i].lbl, 34,  by[i], 76, 22, C_SUB, g_f_reg);
            std::wstring kn = VkName(B[i].vk);
            DTxt(dc, kn.c_str(), 178, by[i], 160, 22, C_AMBER, g_f_mono);
        }

        // 检测反馈 / 等待提示
        if (g_det_msg[0]) {
            DTxt(dc, g_det_msg, 34, 496, 310, 22, C_TEAL, g_f_reg);
        } else if (g_det_target != 0) {
            DTxt(dc, L"⋯  请按下翻页笔的对应键",
                 34, 496, 310, 22, C_AMBER, g_f_reg);
        }

        // ─ 底部说明 ──────────────────────────────────────
        DTxt(dc, L"WH_KEYBOARD_LL 低级钩子  ·  需以管理员身份运行",
             20, 548, 340, 18, C_DIM, g_f_tiny,
             DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        EndPaint(hw, &ps);
        return 0;
    }

    // ── owner-drawn 按钮 ──────────────────────────────────
    case WM_DRAWITEM: {
        auto* di = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
        if (di->CtlType == ODT_BUTTON)
            DrawButton(di);
        return TRUE;
    }

    // ── 定时器 ────────────────────────────────────────────
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

    // ── 按钮命令 ──────────────────────────────────────────
    case WM_COMMAND: {
        int id = LOWORD(wp);

        if (id == IDC_TOGGLE) {
            // 关闭时重置所有按键状态
            if (g_enabled.load()) {
                g_up_held = g_dn_held = g_act_held = false;
                g_pending = false;
                KillTimer(hw, TID_CLICK);
            }
            g_enabled = !g_enabled.load();
            // 刷新按钮和状态
            InvalidateRect(GetDlgItem(hw, IDC_TOGGLE), nullptr, FALSE);
            InvalidateRect(hw, nullptr, FALSE);

        } else if (id == IDC_DET_UP || id == IDC_DET_DN || id == IDC_DET_ACT) {
            // 进入检测模式
            g_det_target = (id == IDC_DET_UP)  ? 1 :
                           (id == IDC_DET_DN)  ? 2 : 3;
            g_det_msg[0] = 0;
            KillTimer(hw, TID_CLR_MSG);
            InvalidateRect(hw, nullptr, FALSE);
        }
        return 0;
    }

    // ── 刷新 UI（从钩子 PostMessage 触发）────────────────
    case WM_REFRESH: {
        if (wp == 1) {
            // 按键检测完成，lp = 被检测到的 VK 码
            std::wstring kn = VkName((DWORD)lp);
            swprintf_s(g_det_msg, 128, L"✓  已更新  →  %s", kn.c_str());
            // 3 秒后自动清除提示
            SetTimer(hw, TID_CLR_MSG, 3000, nullptr);
        }
        // 刷新开关按钮和整个窗口
        InvalidateRect(GetDlgItem(hw, IDC_TOGGLE), nullptr, FALSE);
        InvalidateRect(hw, nullptr, FALSE);
        return 0;
    }

    // ── 销毁 ──────────────────────────────────────────────
    case WM_DESTROY: {
        if (g_hook) {
            UnhookWindowsHookEx(g_hook);
            g_hook = nullptr;
        }
        // 释放 GDI 资源
        auto D = [](HGDIOBJ o){ if (o) DeleteObject(o); };
        D(g_br_bg); D(g_br_pan); D(g_br_dim);
        D(g_f_lrg); D(g_f_med);  D(g_f_reg);
        D(g_f_mono); D(g_f_tiny);
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

    // ── 管理员权限检查 ────────────────────────────────────
    if (!IsAdmin()) {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        // 以管理员身份重新启动
        ShellExecuteW(nullptr, L"runas", path, nullptr, nullptr, SW_NORMAL);
        return 0;
    }

    // ── 注册窗口类 ────────────────────────────────────────
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

    // ── 计算居中窗口位置 ──────────────────────────────────
    RECT wr = {0, 0, CW, CH};
    AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);
    int ww = wr.right  - wr.left;
    int wh = wr.bottom - wr.top;
    int wx = (GetSystemMetrics(SM_CXSCREEN) - ww) / 2;
    int wy = (GetSystemMetrics(SM_CYSCREEN) - wh) / 2;

    // ── 创建窗口（不可缩放 / 无最大化）──────────────────
    g_hwnd = CreateWindowExW(0,
        L"PenMouseCtrl",
        L"翻页笔鼠标控制器",
        (WS_OVERLAPPEDWINDOW & ~(WS_MAXIMIZEBOX | WS_THICKFRAME)),
        wx, wy, ww, wh,
        nullptr, nullptr, hi, nullptr);

    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);

    // ── 消息循环 ──────────────────────────────────────────
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return (int)m.wParam;
}
