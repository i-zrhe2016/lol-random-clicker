/*
 * 随机鼠标左键点击工具 (Windows版)
 * 编译: x86_64-w64-mingw32-gcc random_clicker_win.c -o random_clicker.exe -lgdi32 -luser32 -mwindows -mconsole
 * 用法:
 *   random_clicker.exe              → 手动拉框选择区域（截图背景，可看到程序内容）
 *   random_clicker.exe x1 y1 x2 y2 → 直接指定坐标
 */

#define UNICODE
#define _UNICODE
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <signal.h>
#include <wchar.h>

/* mingw 可能缺少此声明 */
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

static volatile int running = 1;
static int total_clicks = 0;

void handle_signal(int sig) {
    (void)sig;
    running = 0;
}

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

/* -------- 截取全屏到 HBITMAP -------- */
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

/* -------- 拉框选择（截图背景） -------- */

typedef struct { int x1, y1, x2, y2; int done, cancel; } SelState;
static SelState  g_sel;
static HBITMAP   g_screenshot = NULL;
static int       g_sw, g_sh;

/* 在 HDC 上绘制半透明暗色遮罩（用 GDI AlphaBlend） */
static void draw_dim_overlay(HDC hdc, int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    HDC hdc_dim  = CreateCompatibleDC(hdc);
    HBITMAP bmp  = CreateCompatibleBitmap(hdc, w, h);
    SelectObject(hdc_dim, bmp);
    HBRUSH br = CreateSolidBrush(RGB(0, 0, 0));
    RECT rc = {0, 0, w, h};
    FillRect(hdc_dim, &rc, br);
    DeleteObject(br);

    BLENDFUNCTION bf = {AC_SRC_OVER, 0, 140, 0}; /* 55% 透明度 */
    AlphaBlend(hdc, x, y, w, h, hdc_dim, 0, 0, w, h, bf);

    DeleteDC(hdc_dim);
    DeleteObject(bmp);
}

LRESULT CALLBACK SelWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    static int dragging = 0;
    static int sx, sy, px, py;

    switch (msg) {
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { g_sel.cancel = 1; DestroyWindow(hwnd); }
        break;

    case WM_LBUTTONDOWN:
        dragging = 1;
        sx = px = (short)LOWORD(lp);
        sy = py = (short)HIWORD(lp);
        SetCapture(hwnd);
        InvalidateRect(hwnd, NULL, FALSE);
        break;

    case WM_MOUSEMOVE:
        if (dragging) {
            px = (short)LOWORD(lp);
            py = (short)HIWORD(lp);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        break;

    case WM_LBUTTONUP:
        if (dragging) {
            dragging = 0;
            ReleaseCapture();
            int ex = (short)LOWORD(lp), ey = (short)HIWORD(lp);
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

        /* 1. 绘制截图背景 */
        HDC hdc_bmp = CreateCompatibleDC(hdc);
        SelectObject(hdc_bmp, g_screenshot);
        BitBlt(hdc, 0, 0, g_sw, g_sh, hdc_bmp, 0, 0, SRCCOPY);
        DeleteDC(hdc_bmp);

        /* 2. 计算选择框 */
        int lx = sx < px ? sx : px;
        int ly = sy < py ? sy : py;
        int rw = abs(px - sx);
        int rh = abs(py - sy);

        /* 3. 四周暗色遮罩（选框内保持原始截图） */
        draw_dim_overlay(hdc, 0,       0,       g_sw,       ly);           /* 上 */
        draw_dim_overlay(hdc, 0,       ly+rh,   g_sw,       g_sh-ly-rh);  /* 下 */
        draw_dim_overlay(hdc, 0,       ly,      lx,         rh);           /* 左 */
        draw_dim_overlay(hdc, lx+rw,   ly,      g_sw-lx-rw, rh);          /* 右 */

        /* 4. 提示文字 */
        SetBkMode(hdc, TRANSPARENT);
        HFONT font = CreateFontW(28, 0, 0, 0, FW_BOLD, 0, 0, 0,
            DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Microsoft YaHei");
        HFONT old_font = (HFONT)SelectObject(hdc, font);

        const wchar_t *tip = L"按住鼠标左键拖动选择区域，ESC 取消";
        /* 描边（黑色） */
        SetTextColor(hdc, RGB(0, 0, 0));
        for (int dx = -1; dx <= 1; dx++)
            for (int dy = -1; dy <= 1; dy++)
                if (dx || dy) TextOutW(hdc, 40+dx, 16+dy, tip, (int)wcslen(tip));
        /* 正文（白色） */
        SetTextColor(hdc, RGB(255, 255, 255));
        TextOutW(hdc, 40, 16, tip, (int)wcslen(tip));
        SelectObject(hdc, old_font);
        DeleteObject(font);

        /* 5. 选择框边线 + 坐标 */
        if (dragging && rw > 0 && rh > 0) {
            HPEN pen = CreatePen(PS_SOLID, 2, RGB(0, 200, 255));
            HPEN old_pen = (HPEN)SelectObject(hdc, pen);
            HBRUSH old_br = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
            Rectangle(hdc, lx, ly, lx + rw, ly + rh);
            SelectObject(hdc, old_pen);
            SelectObject(hdc, old_br);
            DeleteObject(pen);

            wchar_t info[64];
            swprintf(info, 64, L"(%d,%d) -> (%d,%d)  %dx%d",
                lx, ly, lx+rw, ly+rh, rw, rh);

            HFONT sf = CreateFontW(18, 0, 0, 0, FW_BOLD, 0, 0, 0,
                DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Microsoft YaHei");
            HFONT old_sf = (HFONT)SelectObject(hdc, sf);
            int ty = ly > 22 ? ly - 22 : ly + rh + 4;
            SetTextColor(hdc, RGB(0, 0, 0));
            for (int dx = -1; dx <= 1; dx++)
                for (int dy = -1; dy <= 1; dy++)
                    if (dx || dy) TextOutW(hdc, lx+dx, ty+dy, info, (int)wcslen(info));
            SetTextColor(hdc, RGB(0, 255, 180));
            TextOutW(hdc, lx, ty, info, (int)wcslen(info));
            SelectObject(hdc, old_sf);
            DeleteObject(sf);
        }

        EndPaint(hwnd, &ps);
        break;
    }

    case WM_DESTROY:
        PostQuitMessage(0);
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int select_region(int *ox1, int *oy1, int *ox2, int *oy2) {
    memset(&g_sel, 0, sizeof(g_sel));

    g_sw = GetSystemMetrics(SM_CXSCREEN);
    g_sh = GetSystemMetrics(SM_CYSCREEN);

    /* 截屏（在窗口显示前） */
    g_screenshot = capture_screen(g_sw, g_sh);

    WNDCLASSW wc = {0};
    wc.lpfnWndProc   = SelWndProc;
    wc.hInstance     = GetModuleHandle(NULL);
    wc.lpszClassName = L"SelWnd";
    wc.hCursor       = LoadCursor(NULL, IDC_CROSS);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(
        WS_EX_TOPMOST,
        L"SelWnd", L"选择区域",
        WS_POPUP,
        0, 0, g_sw, g_sh,
        NULL, NULL, wc.hInstance, NULL);

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

int main(int argc, char *argv[]) {
    /* 禁止系统DPI缩放，使用物理像素坐标 */
    set_dpi_aware();
    SetConsoleOutputCP(65001);

    /* 启动延迟 10 秒 */
    printf("程序将在 10 秒后启动...\n"); fflush(stdout);
    for (int i = 10; i > 0; i--) {
        printf("  %d 秒\r", i); fflush(stdout);
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
        printf("请在屏幕上拖动选择点击区域...\n"); fflush(stdout);
        if (select_region(&x1, &y1, &x2, &y2) != 0) {
            printf("已取消\n");
            return 0;
        }
    }

    if (x1 < 0) x1 = 0; if (y1 < 0) y1 = 0;
    if (x2 >= sw) x2 = sw - 1; if (y2 >= sh) y2 = sh - 1;
    if (x1 > x2 || y1 > y2) { fprintf(stderr, "错误: 无效区域\n"); return 1; }
    if (delay_min > delay_max) { int t = delay_min; delay_min = delay_max; delay_max = t; }

    signal(SIGINT, handle_signal);
    srand((unsigned int)time(NULL));

    printf("\n屏幕分辨率: %dx%d\n", sw, sh);
    printf("点击区域:   (%d,%d) -> (%d,%d)  [%dx%d]\n",
           x1, y1, x2, y2, x2-x1, y2-y1);
    printf("间隔范围:   %dms ~ %dms (%.0f~%.0f秒)\n",
           delay_min, delay_max, delay_min/1000.0, delay_max/1000.0);
    printf("点击次数:   %s\n", count > 0 ? "" : "无限循环");
    if (count > 0) printf("            %d 次\n", count);
    printf("按 Ctrl+C 停止\n");
    printf("----------------------------------------\n");

    for (int i = 3; i > 0; i--) {
        printf("  %d...\n", i); fflush(stdout);
        Sleep(1000);
    }
    printf("开始！\n\n");

    while (running) {
        int rx = x1 + rand() % (x2 - x1 + 1);
        int ry = y1 + rand() % (y2 - y1 + 1);

        click_at(rx, ry);
        total_clicks++;
        printf("[%4d] 点击: (%d, %d)\n", total_clicks, rx, ry);
        fflush(stdout);

        if (count > 0 && total_clicks >= count) {
            printf("\n完成！共点击 %d 次\n", total_clicks);
            break;
        }

        int delay = (delay_min == delay_max)
                    ? delay_min
                    : delay_min + rand() % (delay_max - delay_min + 1);
        sleep_ms(delay);
    }

    if (running == 0)
        printf("\n已停止，共点击 %d 次\n", total_clicks);

    return 0;
}
