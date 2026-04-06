/*
 * 随机鼠标左键点击工具 (Windows版)
 * 编译: x86_64-w64-mingw32-gcc random_clicker_win.c random_clicker_res.o
 *         -o random_clicker.exe -lgdi32 -luser32 -lmsimg32 -lshell32
 *         -mwindows -mconsole
 * 用法:
 *   random_clicker.exe              → 手动拉框选择区域
 *   random_clicker.exe x1 y1 x2 y2 → 直接指定坐标
 */

#define _WIN32_IE 0x0600
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <signal.h>
#include <wchar.h>
#include <stdarg.h>

/* -------- DPI -------- */
#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((HANDLE)-4)
#endif
typedef BOOL (WINAPI *PFN_SetProcessDpiAwarenessContext)(HANDLE);
static void set_dpi_aware(void) {
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    if (!u32) return;
    PFN_SetProcessDpiAwarenessContext fn =
        (PFN_SetProcessDpiAwarenessContext)GetProcAddress(u32, "SetProcessDpiAwarenessContext");
    if (fn) fn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
}

/* -------- 托盘常量 -------- */
#define WM_TRAYICON     (WM_USER + 1)
#define ID_TRAY_PAUSE   1001
#define ID_TRAY_STOP    1002
#define ID_TRAY_CONSOLE 1003

/* -------- 颜色属性 -------- */
#define CON_WHITE   (FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE)
#define CON_CYAN    (FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY)
#define CON_YELLOW  (FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY)
#define CON_GREEN   (FOREGROUND_GREEN | FOREGROUND_INTENSITY)
#define CON_RED     (FOREGROUND_RED | FOREGROUND_INTENSITY)
#define CON_LBLUE   (FOREGROUND_BLUE | FOREGROUND_INTENSITY)

/* -------- 全局状态 -------- */
static volatile int  running      = 1;
static volatile LONG g_paused     = 0;
static int           total_clicks = 0;

/* 控制台 */
static HANDLE g_hCon       = INVALID_HANDLE_VALUE;
static BOOL   g_con_is_tty = FALSE;
static COORD  g_status_pos = {0, 0};
static BOOL   g_status_pos_set = FALSE;

/* 托盘 */
static HWND             g_tray_hwnd    = NULL;
static HANDLE           g_tray_thread  = NULL;
static NOTIFYICONDATAW  g_nid;
static HWND             g_console_hwnd = NULL;
static UINT             WM_TASKBARCREATED = 0;

/* 选区 */
typedef struct { int x1, y1, x2, y2; int done, cancel; } SelState;
static SelState  g_sel;
static HBITMAP   g_screenshot = NULL;
static int       g_sw, g_sh;

/* ================================================================
 * 控制台颜色辅助
 * ================================================================ */
static void con_color(WORD attr) {
    if (g_con_is_tty) SetConsoleTextAttribute(g_hCon, attr);
}
static void con_reset(void) {
    con_color(CON_WHITE);
}
static void con_printf(WORD attr, const char *fmt, ...) {
    con_color(attr);
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    con_reset();
    fflush(stdout);
}

/* 在固定行原地刷新状态（点击循环中调用） */
static void update_status_line(int x1, int y1, int x2, int y2,
                                int dmin, int dmax, int clicks) {
    if (!g_con_is_tty) return;

    CONSOLE_SCREEN_BUFFER_INFO csbi;
    GetConsoleScreenBufferInfo(g_hCon, &csbi);
    COORD cur = csbi.dwCursorPosition;

    if (!g_status_pos_set) return;

    SetConsoleCursorPosition(g_hCon, g_status_pos);
    con_color(CON_CYAN);
    printf("  区域: (%d,%d)->(%d,%d) [%dx%d]  间隔: %.1f~%.1fs  点击: ",
           x1, y1, x2, y2, x2-x1, y2-y1,
           dmin/1000.0, dmax/1000.0);
    con_color(CON_YELLOW);
    printf("%-6d", clicks);
    con_reset();
    /* 清除行尾残留 */
    printf("          \r");
    fflush(stdout);

    SetConsoleCursorPosition(g_hCon, cur);
}

/* ================================================================
 * 信号 & 工具
 * ================================================================ */
void handle_signal(int sig) { (void)sig; running = 0; }
void sleep_ms(int ms) { Sleep(ms); }

void click_at(int x, int y) {
    SetCursorPos(x, y);
    Sleep(10);
    INPUT input[2] = {0};
    input[0].type       = INPUT_MOUSE;
    input[0].mi.dwFlags = MOUSEEVENTF_RIGHTDOWN;
    input[1].type       = INPUT_MOUSE;
    input[1].mi.dwFlags = MOUSEEVENTF_RIGHTUP;
    SendInput(2, input, sizeof(INPUT));
}

/* ================================================================
 * 截屏
 * ================================================================ */
static HBITMAP capture_screen(int sw, int sh) {
    HDC hdc_screen = GetDC(NULL);
    HDC hdc_mem    = CreateCompatibleDC(hdc_screen);
    HBITMAP bmp    = CreateCompatibleBitmap(hdc_screen, sw, sh);
    SelectObject(hdc_mem, bmp);
    BitBlt(hdc_mem, 0, 0, sw, sh, hdc_screen, 0, 0, SRCCOPY);
    DeleteDC(hdc_mem);
    ReleaseDC(NULL, hdc_screen);
    return bmp;
}

/* ================================================================
 * 拉框选区
 * ================================================================ */
static void draw_dim_overlay(HDC hdc, int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    HDC hdc_dim  = CreateCompatibleDC(hdc);
    HBITMAP bmp  = CreateCompatibleBitmap(hdc, w, h);
    SelectObject(hdc_dim, bmp);
    HBRUSH br = CreateSolidBrush(RGB(0, 0, 0));
    RECT rc = {0, 0, w, h};
    FillRect(hdc_dim, &rc, br);
    DeleteObject(br);
    BLENDFUNCTION bf = {AC_SRC_OVER, 0, 140, 0};
    AlphaBlend(hdc, x, y, w, h, hdc_dim, 0, 0, w, h, bf);
    DeleteDC(hdc_dim);
    DeleteObject(bmp);
}

LRESULT CALLBACK SelWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    static int dragging  = 0;
    static int sx, sy, px, py;
    static int cur_x = 0, cur_y = 0;
    static int anim_phase = 0;

    switch (msg) {
    case WM_CREATE:
        cur_x = g_sw / 2;
        cur_y = g_sh / 2;
        SetTimer(hwnd, 1, 120, NULL);
        break;

    case WM_TIMER:
        anim_phase ^= 1;
        InvalidateRect(hwnd, NULL, FALSE);
        break;

    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { g_sel.cancel = 1; DestroyWindow(hwnd); }
        break;

    case WM_LBUTTONDOWN:
        dragging = 1;
        sx = px = cur_x;
        sy = py = cur_y;
        SetCapture(hwnd);
        InvalidateRect(hwnd, NULL, FALSE);
        break;

    case WM_MOUSEMOVE:
        cur_x = (short)LOWORD(lp);
        cur_y = (short)HIWORD(lp);
        if (dragging) { px = cur_x; py = cur_y; }
        InvalidateRect(hwnd, NULL, FALSE);
        break;

    case WM_LBUTTONUP:
        if (dragging) {
            dragging = 0;
            ReleaseCapture();
            int ex = cur_x, ey = cur_y;
            if (abs(ex - sx) > 5 && abs(ey - sy) > 5) {
                g_sel.x1   = sx < ex ? sx : ex;
                g_sel.y1   = sy < ey ? sy : ey;
                g_sel.x2   = sx > ex ? sx : ex;
                g_sel.y2   = sy > ey ? sy : ey;
                g_sel.done = 1;
                DestroyWindow(hwnd);
            } else {
                InvalidateRect(hwnd, NULL, FALSE);
            }
        }
        break;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        /* 1. 截图背景 */
        HDC hdc_bmp = CreateCompatibleDC(hdc);
        SelectObject(hdc_bmp, g_screenshot);
        BitBlt(hdc, 0, 0, g_sw, g_sh, hdc_bmp, 0, 0, SRCCOPY);

        /* 2. 计算选择框 */
        int lx = sx < px ? sx : px;
        int ly = sy < py ? sy : py;
        int rw = abs(px - sx);
        int rh = abs(py - sy);

        /* 3. 暗色遮罩（选框内保持清晰） */
        if (dragging && rw > 0 && rh > 0) {
            draw_dim_overlay(hdc, 0,     0,      g_sw,       ly);
            draw_dim_overlay(hdc, 0,     ly+rh,  g_sw,       g_sh-ly-rh);
            draw_dim_overlay(hdc, 0,     ly,     lx,         rh);
            draw_dim_overlay(hdc, lx+rw, ly,     g_sw-lx-rw, rh);
        } else {
            draw_dim_overlay(hdc, 0, 0, g_sw, g_sh);
        }

        /* 4. 提示文字 */
        SetBkMode(hdc, TRANSPARENT);
        HFONT font = CreateFontW(26, 0, 0, 0, FW_BOLD, 0, 0, 0,
            DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Microsoft YaHei");
        HFONT old_font = (HFONT)SelectObject(hdc, font);
        const wchar_t *tip = L"按住鼠标左键拖动选择区域，ESC 取消";
        SetTextColor(hdc, RGB(0, 0, 0));
        for (int dx = -1; dx <= 1; dx++)
            for (int dy = -1; dy <= 1; dy++)
                if (dx || dy) TextOutW(hdc, 40+dx, 14+dy, tip, (int)wcslen(tip));
        SetTextColor(hdc, RGB(255, 255, 100));
        TextOutW(hdc, 40, 14, tip, (int)wcslen(tip));
        SelectObject(hdc, old_font);
        DeleteObject(font);

        /* 5. 十字准线 */
        HPEN cross_pen = CreatePen(PS_SOLID, 1, RGB(0, 200, 255));
        HPEN old_pen = (HPEN)SelectObject(hdc, cross_pen);
        MoveToEx(hdc, 0, cur_y, NULL);    LineTo(hdc, g_sw, cur_y);
        MoveToEx(hdc, cur_x, 0, NULL);    LineTo(hdc, cur_x, g_sh);
        SelectObject(hdc, old_pen);
        DeleteObject(cross_pen);

        /* 6. 坐标标注（靠近边缘时翻转） */
        wchar_t coord_str[32];
        swprintf(coord_str, 32, L" %d, %d ", cur_x, cur_y);
        HFONT cf = CreateFontW(16, 0, 0, 0, FW_BOLD, 0, 0, 0,
            DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Consolas");
        HFONT old_cf = (HFONT)SelectObject(hdc, cf);
        int cx_off = (cur_x + 120 < g_sw) ? cur_x + 14 : cur_x - 120;
        int cy_off = (cur_y - 20 > 0) ? cur_y - 20 : cur_y + 6;
        SetTextColor(hdc, RGB(0, 0, 0));
        for (int dx = -1; dx <= 1; dx++)
            for (int dy = -1; dy <= 1; dy++)
                if (dx || dy) TextOutW(hdc, cx_off+dx, cy_off+dy, coord_str, (int)wcslen(coord_str));
        SetTextColor(hdc, RGB(0, 255, 180));
        TextOutW(hdc, cx_off, cy_off, coord_str, (int)wcslen(coord_str));
        SelectObject(hdc, old_cf);
        DeleteObject(cf);

        /* 7. 选择框：动画边线 + 角标 */
        if (dragging && rw > 0 && rh > 0) {
            COLORREF border_col = anim_phase ? RGB(0, 220, 255) : RGB(255, 255, 255);
            HPEN sel_pen = CreatePen(PS_DASH, 1, border_col);
            HPEN old_sp  = (HPEN)SelectObject(hdc, sel_pen);
            HBRUSH old_br = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
            Rectangle(hdc, lx, ly, lx + rw, ly + rh);
            SelectObject(hdc, old_sp);
            SelectObject(hdc, old_br);
            DeleteObject(sel_pen);

            /* 角标：8x8 实心白色方块 */
            HBRUSH corner_br = CreateSolidBrush(RGB(255, 255, 255));
            int pts[4][2] = { {lx,lx+rw-8}, {lx,ly+rh-8} };
            /* 四角 */
            int cx[4] = {lx, lx+rw-8, lx, lx+rw-8};
            int cy[4] = {ly, ly, ly+rh-8, ly+rh-8};
            for (int i = 0; i < 4; i++) {
                RECT cr = {cx[i], cy[i], cx[i]+8, cy[i]+8};
                FillRect(hdc, &cr, corner_br);
            }
            DeleteObject(corner_br);
            (void)pts;

            /* 尺寸信息 */
            wchar_t info[64];
            swprintf(info, 64, L"  %dx%d  ", rw, rh);
            HFONT sf = CreateFontW(16, 0, 0, 0, FW_BOLD, 0, 0, 0,
                DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Consolas");
            HFONT old_sf = (HFONT)SelectObject(hdc, sf);
            int ty = ly > 24 ? ly - 22 : ly + rh + 4;
            SetTextColor(hdc, RGB(0, 0, 0));
            for (int dx = -1; dx <= 1; dx++)
                for (int dy = -1; dy <= 1; dy++)
                    if (dx || dy) TextOutW(hdc, lx+dx, ty+dy, info, (int)wcslen(info));
            SetTextColor(hdc, RGB(0, 255, 180));
            TextOutW(hdc, lx, ty, info, (int)wcslen(info));
            SelectObject(hdc, old_sf);
            DeleteObject(sf);
        }

        /* 8. 放大镜（右下角固定，120x120，3x 缩放） */
        {
            int zoom_w = 120, zoom_h = 120;
            int zoom_x = g_sw - zoom_w - 16;
            int zoom_y = g_sh - zoom_h - 16;
            int src_w = 40, src_h = 40;
            int src_x = cur_x - src_w/2;
            int src_y = cur_y - src_h/2;
            /* clamp */
            if (src_x < 0) src_x = 0;
            if (src_y < 0) src_y = 0;
            if (src_x + src_w > g_sw) src_x = g_sw - src_w;
            if (src_y + src_h > g_sh) src_y = g_sh - src_h;

            SetStretchBltMode(hdc_bmp, COLORONCOLOR);
            StretchBlt(hdc, zoom_x, zoom_y, zoom_w, zoom_h,
                       hdc_bmp, src_x, src_y, src_w, src_h, SRCCOPY);

            /* 像素网格线（每 3px = 1 源像素） */
            HPEN grid_pen = CreatePen(PS_SOLID, 1, RGB(60, 60, 60));
            HPEN old_gp = (HPEN)SelectObject(hdc, grid_pen);
            for (int gx = 0; gx <= zoom_w; gx += 3) {
                MoveToEx(hdc, zoom_x + gx, zoom_y, NULL);
                LineTo(hdc, zoom_x + gx, zoom_y + zoom_h);
            }
            for (int gy = 0; gy <= zoom_h; gy += 3) {
                MoveToEx(hdc, zoom_x, zoom_y + gy, NULL);
                LineTo(hdc, zoom_x + zoom_w, zoom_y + gy);
            }
            SelectObject(hdc, old_gp);
            DeleteObject(grid_pen);

            /* 中心十字 */
            HPEN cp = CreatePen(PS_SOLID, 1, RGB(255, 80, 80));
            HPEN old_cp = (HPEN)SelectObject(hdc, cp);
            int mx = zoom_x + zoom_w/2, my = zoom_y + zoom_h/2;
            MoveToEx(hdc, zoom_x, my, NULL); LineTo(hdc, zoom_x + zoom_w, my);
            MoveToEx(hdc, mx, zoom_y, NULL); LineTo(hdc, mx, zoom_y + zoom_h);
            SelectObject(hdc, old_cp);
            DeleteObject(cp);

            /* 外框 */
            HPEN bp = CreatePen(PS_SOLID, 2, RGB(0, 200, 255));
            HPEN old_bp = (HPEN)SelectObject(hdc, bp);
            HBRUSH old_nb = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
            Rectangle(hdc, zoom_x-1, zoom_y-1, zoom_x+zoom_w+1, zoom_y+zoom_h+1);
            SelectObject(hdc, old_bp);
            SelectObject(hdc, old_nb);
            DeleteObject(bp);

            /* 标签 */
            HFONT lf = CreateFontW(14, 0, 0, 0, FW_BOLD, 0, 0, 0,
                DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Consolas");
            HFONT old_lf = (HFONT)SelectObject(hdc, lf);
            SetTextColor(hdc, RGB(0, 200, 255));
            TextOutW(hdc, zoom_x, zoom_y - 18, L"3x", 2);
            SelectObject(hdc, old_lf);
            DeleteObject(lf);
        }

        DeleteDC(hdc_bmp);
        EndPaint(hwnd, &ps);
        break;
    }

    case WM_DESTROY:
        KillTimer(hwnd, 1);
        PostQuitMessage(0);
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int select_region(int *ox1, int *oy1, int *ox2, int *oy2) {
    memset(&g_sel, 0, sizeof(g_sel));
    g_sw = GetSystemMetrics(SM_CXSCREEN);
    g_sh = GetSystemMetrics(SM_CYSCREEN);
    g_screenshot = capture_screen(g_sw, g_sh);

    WNDCLASSW wc = {0};
    wc.lpfnWndProc   = SelWndProc;
    wc.hInstance     = GetModuleHandle(NULL);
    wc.lpszClassName = L"SelWnd";
    wc.hCursor       = LoadCursor(NULL, IDC_CROSS);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(
        WS_EX_TOPMOST, L"SelWnd", L"选择区域", WS_POPUP,
        0, 0, g_sw, g_sh, NULL, NULL, wc.hInstance, NULL);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    SetForegroundWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    UnregisterClassW(L"SelWnd", wc.hInstance);
    DeleteObject(g_screenshot);
    g_screenshot = NULL;

    if (g_sel.done) {
        *ox1 = g_sel.x1; *oy1 = g_sel.y1;
        *ox2 = g_sel.x2; *oy2 = g_sel.y2;
        return 0;
    }
    return -1;
}

/* ================================================================
 * 托盘图标
 * ================================================================ */
static HICON create_tray_icon(void) {
    /* 16x16 纯色图标（绿色方块），程序化创建，无需 .ico 文件 */
    BYTE color_bits[16 * 16 * 4];
    for (int i = 0; i < 16 * 16; i++) {
        color_bits[i*4 + 0] = 80;   /* B */
        color_bits[i*4 + 1] = 200;  /* G */
        color_bits[i*4 + 2] = 40;   /* R */
        color_bits[i*4 + 3] = 255;  /* A */
    }
    /* 加一圈深色边框 */
    for (int i = 0; i < 16; i++) {
        int edge[4] = {i, i*16, i + 15*16, i*16 + 15};
        for (int j = 0; j < 4; j++) {
            int idx = edge[j];
            color_bits[idx*4+0] = 20;
            color_bits[idx*4+1] = 100;
            color_bits[idx*4+2] = 10;
        }
    }

    BITMAPV5HEADER bi = {0};
    bi.bV5Size        = sizeof(BITMAPV5HEADER);
    bi.bV5Width       = 16;
    bi.bV5Height      = -16; /* top-down */
    bi.bV5Planes      = 1;
    bi.bV5BitCount    = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask     = 0x00FF0000;
    bi.bV5GreenMask   = 0x0000FF00;
    bi.bV5BlueMask    = 0x000000FF;
    bi.bV5AlphaMask   = 0xFF000000;

    HDC hdc = GetDC(NULL);
    void *bits = NULL;
    HBITMAP hbm_color = CreateDIBSection(hdc, (BITMAPINFO*)&bi,
                                          DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, hdc);
    if (!hbm_color) return NULL;
    memcpy(bits, color_bits, sizeof(color_bits));

    /* 1bpp 全零遮罩 = 完全不透明 */
    BYTE mask_bits[16 * 2]; /* 16 rows × 2 bytes (16 bits) */
    memset(mask_bits, 0, sizeof(mask_bits));
    HBITMAP hbm_mask = CreateBitmap(16, 16, 1, 1, mask_bits);

    ICONINFO ii = {0};
    ii.fIcon    = TRUE;
    ii.hbmColor = hbm_color;
    ii.hbmMask  = hbm_mask;
    HICON icon = CreateIconIndirect(&ii);

    DeleteObject(hbm_color);
    DeleteObject(hbm_mask);
    return icon;
}

LRESULT CALLBACK TrayWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_TASKBARCREATED) {
        Shell_NotifyIconW(NIM_ADD, &g_nid);
        return 0;
    }
    switch (msg) {
    case WM_TRAYICON:
        if (LOWORD(lp) == WM_RBUTTONUP) {
            POINT pt;
            GetCursorPos(&pt);
            HMENU menu = CreatePopupMenu();
            BOOL paused = (InterlockedCompareExchange(&g_paused, 0, 0) != 0);
            AppendMenuW(menu, MF_STRING, ID_TRAY_PAUSE,
                        paused ? L"继续 (Resume)" : L"暂停 (Pause)");
            AppendMenuW(menu, MF_STRING, ID_TRAY_STOP,    L"停止 (Stop)");
            AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
            AppendMenuW(menu, MF_STRING, ID_TRAY_CONSOLE, L"显示控制台");
            SetForegroundWindow(hwnd);
            TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
            PostMessage(hwnd, WM_NULL, 0, 0);
            DestroyMenu(menu);
        }
        break;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_TRAY_PAUSE:
            InterlockedExchange(&g_paused, g_paused ? 0L : 1L);
            break;
        case ID_TRAY_STOP:
            running = 0;
            break;
        case ID_TRAY_CONSOLE:
            if (g_console_hwnd) {
                ShowWindow(g_console_hwnd, SW_RESTORE);
                SetForegroundWindow(g_console_hwnd);
            }
            break;
        }
        break;

    case WM_DESTROY:
        PostQuitMessage(0);
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static DWORD WINAPI tray_thread_proc(LPVOID param) {
    (void)param;

    WM_TASKBARCREATED = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSW wc = {0};
    wc.lpfnWndProc   = TrayWndProc;
    wc.hInstance     = GetModuleHandle(NULL);
    wc.lpszClassName = L"TrayMsgWnd";
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(0, L"TrayMsgWnd", NULL, 0,
        0, 0, 0, 0, HWND_MESSAGE, NULL, wc.hInstance, NULL);
    if (!hwnd) return 1;

    HICON icon = create_tray_icon();

    memset(&g_nid, 0, sizeof(g_nid));
    g_nid.cbSize           = sizeof(NOTIFYICONDATAW);
    g_nid.hWnd             = hwnd;
    g_nid.uID              = 1;
    g_nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon            = icon;
    wcscpy(g_nid.szTip, L"RandomClicker - 运行中");

    Shell_NotifyIconW(NIM_ADD, &g_nid);
    g_tray_hwnd = hwnd;

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    if (icon) DestroyIcon(icon);
    UnregisterClassW(L"TrayMsgWnd", wc.hInstance);
    return 0;
}

/* ================================================================
 * main
 * ================================================================ */
int main(int argc, char *argv[]) {
    set_dpi_aware();
    SetConsoleOutputCP(65001);

    g_hCon = GetStdHandle(STD_OUTPUT_HANDLE);
    g_con_is_tty = (g_hCon != INVALID_HANDLE_VALUE &&
                    GetFileType(g_hCon) == FILE_TYPE_CHAR);

    /* 标题横幅 */
    con_printf(CON_CYAN,
        "+------------------------------------------+\n"
        "|         Random Clicker  v2.0             |\n"
        "|     随机鼠标点击工具 (Windows)           |\n"
        "+------------------------------------------+\n");

    /* 启动延迟 */
    con_printf(CON_WHITE, "程序将在 ");
    con_printf(CON_YELLOW, "10");
    con_printf(CON_WHITE, " 秒后启动...\n");
    for (int i = 10; i > 0; i--) {
        con_printf(CON_YELLOW, "  %d...\r", i);
        Sleep(1000);
    }
    printf("                \n"); fflush(stdout);

    int x1, y1, x2, y2;
    int count     = 0;
    int delay_min = 15000;
    int delay_max = 30000;

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);

    if (argc >= 5) {
        x1 = atoi(argv[1]); y1 = atoi(argv[2]);
        x2 = atoi(argv[3]); y2 = atoi(argv[4]);
        if (argc > 5) count     = atoi(argv[5]);
        if (argc > 6) delay_min = atoi(argv[6]);
        if (argc > 7) delay_max = atoi(argv[7]);
    } else {
        con_printf(CON_WHITE, "请在屏幕上拖动选择点击区域...\n");
        if (select_region(&x1, &y1, &x2, &y2) != 0) {
            con_printf(CON_YELLOW, "已取消\n");
            return 0;
        }
    }

    if (x1 < 0) x1 = 0; if (y1 < 0) y1 = 0;
    if (x2 >= sw) x2 = sw - 1; if (y2 >= sh) y2 = sh - 1;
    if (x1 > x2 || y1 > y2) {
        con_printf(CON_RED, "错误: 无效区域\n");
        return 1;
    }
    if (delay_min > delay_max) { int t = delay_min; delay_min = delay_max; delay_max = t; }

    signal(SIGINT, handle_signal);
    srand((unsigned int)time(NULL));

    con_printf(CON_CYAN,  "\n屏幕分辨率: ");
    con_printf(CON_WHITE, "%dx%d\n", sw, sh);
    con_printf(CON_CYAN,  "点击区域:   ");
    con_printf(CON_WHITE, "(%d,%d) -> (%d,%d)  [%dx%d]\n",
               x1, y1, x2, y2, x2-x1, y2-y1);
    con_printf(CON_CYAN,  "间隔范围:   ");
    con_printf(CON_WHITE, "%.1f ~ %.1f 秒\n", delay_min/1000.0, delay_max/1000.0);
    con_printf(CON_CYAN,  "点击次数:   ");
    con_printf(CON_WHITE, "%s\n", count > 0 ? "" : "无限循环");
    if (count > 0) con_printf(CON_WHITE, "            %d 次\n", count);
    con_printf(CON_WHITE, "按 ");
    con_printf(CON_YELLOW, "Ctrl+C");
    con_printf(CON_WHITE,  " 停止 | 托盘图标右键菜单可暂停/停止\n");
    con_printf(CON_CYAN,   "----------------------------------------\n");

    for (int i = 3; i > 0; i--) {
        con_printf(CON_YELLOW, "  %d...\n", i);
        Sleep(1000);
    }
    con_printf(CON_GREEN, "开始！\n\n");

    /* 状态行占位 */
    if (g_con_is_tty) {
        CONSOLE_SCREEN_BUFFER_INFO csbi;
        GetConsoleScreenBufferInfo(g_hCon, &csbi);
        g_status_pos = csbi.dwCursorPosition;
        g_status_pos_set = TRUE;
        con_printf(CON_CYAN, "  [状态行]\n\n");
    }

    /* 托盘 + 最小化控制台 */
    g_console_hwnd = GetConsoleWindow();
    ShowWindow(g_console_hwnd, SW_MINIMIZE);
    g_tray_thread = CreateThread(NULL, 0, tray_thread_proc, NULL, 0, NULL);
    Sleep(150);

    /* 点击主循环 */
    while (running) {
        /* 暂停检测 */
        while (InterlockedCompareExchange(&g_paused, 0, 0) && running)
            Sleep(200);
        if (!running) break;

        int rx = x1 + rand() % (x2 - x1 + 1);
        int ry = y1 + rand() % (y2 - y1 + 1);
        click_at(rx, ry);
        total_clicks++;

        con_printf(CON_GREEN, "[%4d] 点击: (%d, %d)\n", total_clicks, rx, ry);
        update_status_line(x1, y1, x2, y2, delay_min, delay_max, total_clicks);

        /* 更新托盘 tooltip */
        if (g_tray_hwnd) {
            swprintf(g_nid.szTip, 128, L"RandomClicker - 已点击 %d 次", total_clicks);
            Shell_NotifyIconW(NIM_MODIFY, &g_nid);
        }

        if (count > 0 && total_clicks >= count) {
            con_printf(CON_GREEN, "\n完成！共点击 %d 次\n", total_clicks);
            break;
        }

        /* 间隔睡眠（支持中途停止/暂停） */
        int delay = (delay_min == delay_max)
                    ? delay_min
                    : delay_min + rand() % (delay_max - delay_min + 1);
        int elapsed = 0;
        while (elapsed < delay && running) {
            while (InterlockedCompareExchange(&g_paused, 0, 0) && running)
                Sleep(200);
            Sleep(100);
            elapsed += 100;
        }
    }

    if (running == 0 && !(count > 0 && total_clicks >= count))
        con_printf(CON_YELLOW, "\n已停止，共点击 %d 次\n", total_clicks);

    /* 退出清理 */
    if (g_tray_hwnd) PostMessage(g_tray_hwnd, WM_QUIT, 0, 0);
    if (g_tray_thread) {
        WaitForSingleObject(g_tray_thread, 3000);
        CloseHandle(g_tray_thread);
    }
    if (g_console_hwnd) ShowWindow(g_console_hwnd, SW_RESTORE);

    return 0;
}
