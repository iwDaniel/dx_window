
//#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objidl.h>   // IStream 定义在这里
#include <propidl.h> 
#include <GdiPlus.h>
#include <map>
#include <string>
#pragma comment(lib, "gdiplus.lib")
using namespace Gdiplus;//如果没有
#define     DX_SIDE __declspec(dllexport)
////////////////////////////////////////////////////////////////////
static HINSTANCE  g_hDllInstance = NULL;   // DLL 自身模块句柄
static ULONG_PTR  g_gdiplusToken = 0;
static bool       g_inited = false;  // 是否已初始化

static std::map<HWND, Bitmap*> g_bitmaps;
static std::map<HWND, bool> g_draggable;
static std::wstring g_title = L"GMPngWindow";

static const wchar_t* CLASS_NAME = L"GMPngWindowClass";
////////////////////////////////////////////////////////////////////////////////////////////////////
// ================================================================
// RenderLayeredWindow：把窗口对应的 Bitmap 用 alpha 混合绘制到屏幕上
// ================================================================
// 这是分层窗口的核心：创建 32 位 DIB → GDI+ 绘制 → UpdateLayeredWindow
// ================================================================
static bool RenderLayeredWindow(HWND hWnd)
{
    auto it = g_bitmaps.find(hWnd);
    if (it == g_bitmaps.end() || !it->second) return false;

    Bitmap* pBmp = it->second;

    // 1. 取客户区尺寸
    RECT rcClient;
    GetClientRect(hWnd, &rcClient);
    int cw = rcClient.right - rcClient.left;
    int ch = rcClient.bottom - rcClient.top;
    if (cw <= 0 || ch <= 0) return false;

    HDC hdcScreen = GetDC(NULL);
    if (!hdcScreen) return false;

    // 2. 创建 32 位 DIB（带 alpha 通道）
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = cw;
    bmi.bmiHeader.biHeight = -ch;   // 负数 = 自上而下
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;    // 32 位，每像素 4 字节（BGRA）
    bmi.bmiHeader.biCompression = BI_RGB;

    void* pBits = NULL;
    HBITMAP hDib = CreateDIBSection(hdcScreen, &bmi, DIB_RGB_COLORS, &pBits, NULL, 0);
    if (!hDib || !pBits)
    {
        ReleaseDC(NULL, hdcScreen);
        return false;
    }

    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    HBITMAP hOld = (HBITMAP)SelectObject(hdcMem, hDib);

    // 3. 用 GDI+ 把源图片画到 DIB 上（保留 alpha）
    {
        // 用 PixelFormat32bppPARGB（预乘 alpha）
        Bitmap bmpDib(cw, ch, cw * 4, PixelFormat32bppPARGB, (BYTE*)pBits);
        Graphics g(&bmpDib);
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        // SourceCopy：直接覆盖像素，不做混合，保留源图 alpha
        g.SetCompositingMode(CompositingModeSourceCopy);
        g.DrawImage(pBmp, 0, 0, cw, ch);
    }

    // 4. 取窗口当前屏幕位置
    RECT rcWin;
    GetWindowRect(hWnd, &rcWin);

    POINT ptSrc = { 0, 0 };
    POINT ptDst = { rcWin.left, rcWin.top };
    SIZE  size = { cw, ch };
    // AC_SRC_OVER + AC_SRC_ALPHA = 按源图 alpha 通道混合
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };

    BOOL ok = UpdateLayeredWindow(
        hWnd,
        hdcScreen,
        &ptDst, &size,
        hdcMem, &ptSrc,
        0, &bf,
        ULW_ALPHA);

    // 5. 清理
    SelectObject(hdcMem, hOld);
    DeleteObject(hDib);
    DeleteDC(hdcMem);
    ReleaseDC(NULL, hdcScreen);

    return ok != FALSE;
}


/*
static HRGN CreateRegionFromBitmap(Bitmap* bmp, BYTE alphaThreshold = 128)
{
    if (!bmp) return NULL;

    int w = (int)bmp->GetWidth();
    int h = (int)bmp->GetHeight();
    if (w <= 0 || h <= 0) return NULL;

    Rect rect(0, 0, w, h);
    BitmapData data;
    if (bmp->LockBits(&rect, ImageLockModeRead, PixelFormat32bppARGB, &data) != Ok)
        return NULL;

    BYTE* scan0 = (BYTE*)data.Scan0;
    int   stride = data.Stride;

    HRGN hTotal = CreateRectRgn(0, 0, 0, 0);

    for (int y = 0; y < h; ++y)
    {
        BYTE* row = scan0 + y * stride;
        int xStart = -1;

        for (int x = 0; x < w; ++x)
        {
            BYTE alpha = row[x * 4 + 3];
            bool opaque = (alpha >= alphaThreshold);

            if (opaque && xStart < 0)
            {
                xStart = x;
            }
            else if (!opaque && xStart >= 0)
            {
                HRGN hRow = CreateRectRgn(xStart, y, x, y + 1);
                CombineRgn(hTotal, hTotal, hRow, RGN_OR);
                DeleteObject(hRow);
                xStart = -1;
            }
        }
        if (xStart >= 0)
        {
            HRGN hRow = CreateRectRgn(xStart, y, w, y + 1);
            CombineRgn(hTotal, hTotal, hRow, RGN_OR);
            DeleteObject(hRow);
        }
    }

    bmp->UnlockBits(&data);
    return hTotal;
}
static HRGN CreateRegionForBorderedWindow(HWND hWnd, Bitmap* bmp, BYTE threshold)
{
    // 1. 取客户区相对窗口的偏移
    POINT ptClient = { 0, 0 };
    ClientToScreen(hWnd, &ptClient);   // 客户区左上角的屏幕坐标
    RECT rcWin;
    GetWindowRect(hWnd, &rcWin);
    int offsetX = ptClient.x - rcWin.left;   // 客户区相对窗口的 X 偏移
    int offsetY = ptClient.y - rcWin.top;    // 客户区相对窗口的 Y 偏移（≈ 标题栏高度）

    // 2. 从图片 alpha 生成区域（客户区坐标）
    HRGN hImgRgn = CreateRegionFromBitmap(bmp, threshold);
    if (!hImgRgn) return NULL;

    // 3. 把图片区域偏移到窗口坐标
    OffsetRgn(hImgRgn, offsetX, offsetY);

    // 4. 构造标题栏 + 边框矩形（覆盖整个非客户区）
    RECT rcClient;
    GetClientRect(hWnd, &rcClient);
    int winW = rcWin.right - rcWin.left;
    int winH = rcWin.bottom - rcWin.top;

    // 标题栏矩形：从窗口顶部到客户区顶部
    HRGN hCaption = CreateRectRgn(0, 0, winW, offsetY);

    // 合并：标题栏 + 偏移后的图片区域
    HRGN hTotal = CreateRectRgn(0, 0, 0, 0);
    CombineRgn(hTotal, hCaption, hImgRgn, RGN_OR);

    DeleteObject(hImgRgn);
    DeleteObject(hCaption);
    return hTotal;
}*/

////////////////////////////////////////////////////////////////////////////////////////////////////////////
// ---------------- 窗口过程 ----------------
static LRESULT CALLBACK PngWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hWnd, &ps);
    DWORD exStyle = (DWORD)GetWindowLongPtr(hWnd, GWL_EXSTYLE);
    if (exStyle & WS_EX_LAYERED)
    {   
        //MessageBoxA(NULL, "Layered window", "Info", MB_OK);
        EndPaint(hWnd, &ps);
        return 0;
    }


    auto it = g_bitmaps.find(hWnd);
    if (it == g_bitmaps.end() || !it->second)
    {
        EndPaint(hWnd, &ps);
        return 0;
    }
    Bitmap* bmp = it->second;
    RECT rcClient;
    GetClientRect(hWnd, &rcClient);
    int cw = rcClient.right - rcClient.left;
    int ch = rcClient.bottom - rcClient.top;

    // 客户区为 0（如最小化）时直接跳过，避免创建 0×0 位图
    if (cw <= 0 || ch <= 0)
    {
        EndPaint(hWnd, &ps);
        return 0;
    }
    // ---- 双缓冲：先画到内存 DC ----
    HDC hdcMem = CreateCompatibleDC(hdc);
    HBITMAP hbmMem = CreateCompatibleBitmap(hdc, cw, ch);
    HBITMAP hbmOld = (HBITMAP)SelectObject(hdcMem, hbmMem);

    {
        Graphics g(hdcMem);
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        // 拉伸到整个客户区
        g.DrawImage(bmp, 0, 0, cw, ch);
    }

    // ---- 一次性贴到屏幕 ----
    BitBlt(hdc, 0, 0, cw, ch, hdcMem, 0, 0, SRCCOPY);

    // ---- 清理 ----
    SelectObject(hdcMem, hbmOld);
    DeleteObject(hbmMem);
    DeleteDC(hdcMem);
    EndPaint(hWnd, &ps);
        return 0;
    }


    case WM_DESTROY:
    {
        auto it = g_bitmaps.find(hWnd);
        if (it != g_bitmaps.end())
        {
            delete it->second;
            g_bitmaps.erase(it);
        }
        return 0;
    }
    case WM_NCHITTEST:
    {
        LRESULT hit = DefWindowProc(hWnd, msg, wParam, lParam);

        // 查询该窗口是否启用了整窗拖动
        auto it = g_draggable.find(hWnd);
        if (it != g_draggable.end() && it->second)
        {
            // 只把【客户区】伪装成标题栏，非客户区（边框、按钮）保持原样
            if (hit == HTCLIENT)
                return HTCAPTION;
        }
        return hit;
    }
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

// ---------------- DllMain：只保存模块句柄 ----------------
// 注意：不在 DllMain 里做 GDI+/窗口类初始化，避免加载器锁死锁。
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_hDllInstance = (HINSTANCE)hModule;
        DisableThreadLibraryCalls(hModule);
    }
    return TRUE;
}
////////////////////////////////////////////////
#ifdef __cplusplus
extern "C" {
#endif
    // ================================================================
// 导出函数 1：DX_INIT（必须最先调用）
// ================================================================
// 无参数
// 返回：1 = 初始化成功；0 = 失败
// ================================================================
    DX_SIDE double __cdecl DX_INIT()
    {   
        if (g_inited) return 1;   // 重复调用直接成功

        // 1. 启动 GDI+
        GdiplusStartupInput input;
        if (GdiplusStartup(&g_gdiplusToken, &input, NULL) != Ok)
            return 0;
        //1.1. 激活上下文，确保使用 DLL 内嵌的 ComCtl32 v6 资源（没啦哈哈哈）
        // 2. 注册窗口类（使用 DLL 自己的 HINSTANCE）
        WNDCLASSEX wc = {};
        wc.cbSize = sizeof(WNDCLASSEX);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = PngWndProc;
        wc.hInstance = g_hDllInstance;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        //wc.hbrBackground = NULL;
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.lpszClassName = CLASS_NAME;

        if (!RegisterClassEx(&wc))
        {
            GdiplusShutdown(g_gdiplusToken);
            g_gdiplusToken = 0;
            return 0;
        }
        g_inited = true;
        return 1.0;
    }
    DX_SIDE double __cdecl DX_UNLOAD()
    {
        if (!g_inited) return 0.0;

        int count = 0;

        // 1. 销毁所有窗口（WM_DESTROY 会 delete Bitmap 并从 map 删除）
        while (!g_bitmaps.empty())
        {
            HWND hWnd = g_bitmaps.begin()->first;

            if (IsWindow(hWnd))
            {
                // DestroyWindow 触发 WM_DESTROY → delete Bitmap + erase
                DestroyWindow(hWnd);
                ++count;
            }
            else
            {
                // 窗口已不存在，手动清理残留 Bitmap
                delete g_bitmaps.begin()->second;
                g_bitmaps.erase(g_bitmaps.begin());
            }
        }

        // 2. 关闭 GDI+
        if (g_gdiplusToken)
        {
            GdiplusShutdown(g_gdiplusToken);
            g_gdiplusToken = 0;
        }

        // 3. 重置状态
        g_inited = false;
        g_title = L"GMPngWindow";
		//3.关闭激活上下文 
        // 注意：窗口类已注册，Windows 没有反注册 API，
        //       进程退出时系统会自动释放，无需处理。

        return (double)count;
    }

    // ================================================================
// 导出函数 2：DX_CREATE_PNG_WINDOW
// ================================================================
// imagePath : a.png 路径（ANSI 字符串）
// x, y      : 初始屏幕坐标
// hasBorder : 1 = 有边框，0 = 无边框
// 返回      : 窗口句柄（HWND）；失败返回 0
// ================================================================
    DX_SIDE double DX_CREATE_PNG_WINDOW(const char* imagePath, double x, double y, double hasBorder)
    {
        //char buf[64];
        //sprintf_s(buf, "x=%d, y=%d", x, y);
        //MessageBoxA(NULL, buf, "params", MB_OK);
        if (!g_inited) return 0.0;
        //0. 记录当前窗口
        HWND hPrevForeground = GetForegroundWindow();
        // 1. 加载 PNG 图片
        int pSize = MultiByteToWideChar(CP_ACP, 0, imagePath, -1, NULL, 0);
        if (pSize <= 0) return 0.0;
        std::wstring wImagePath(pSize, L'\0');
        MultiByteToWideChar(CP_ACP, 0, imagePath, -1, &wImagePath[0], pSize);
        Bitmap* bmp = Bitmap::FromFile(wImagePath.c_str());
        if (!bmp || bmp->GetLastStatus() != Ok)
        {
            //////////////////////////////////
            char buf[128];///////////////////////
            sprintf_s(buf, "Bitmap load failed, status=%d", bmp ? (int)bmp->GetLastStatus() : -1);
            MessageBoxA(NULL, buf, "err", MB_OK);
            ////////////////////////
            delete bmp;
            return 0.0;
        }
        // 2. 创建窗口
        DWORD style = WS_POPUP;
		//char buf[64];
        //sprintf_s(buf, "%d:%.2f", (int)hasBorder, hasBorder);
		//MessageBoxA(NULL, buf, "Info", MB_OK);
        if (hasBorder) style |= WS_CAPTION | WS_MINIMIZEBOX;

        DWORD exStyle = WS_EX_TOPMOST | WS_EX_TOOLWINDOW
            | WS_EX_NOACTIVATE;
        if (hasBorder == 0)
        {
            exStyle |= WS_EX_LAYERED;
        }

        // 3. ★ 用 AdjustWindowRectEx 把图片尺寸换算成外框尺寸
        RECT rc = { 0, 0, (LONG)bmp->GetWidth(), (LONG)bmp->GetHeight() };
        AdjustWindowRectEx(&rc, style, FALSE, exStyle);
        int winW = rc.right - rc.left;
        int winH = rc.bottom - rc.top;
        //DWORD style = WS_POPUP;
        //if (hasBorder) style |= WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
        HWND hWnd = CreateWindowEx(
            exStyle,
            CLASS_NAME,
            g_title.c_str(),
            style,
            (int)x, (int)y,
            winW, winH,
            NULL, NULL,
            g_hDllInstance, NULL);
        if (!hWnd)
        {
            delete bmp;
            return 0.0;
        }
        // 3. 保存 Bitmap/draggable/closeable 指针
        g_bitmaps[hWnd] = bmp;
        g_draggable[hWnd] = false;
        // 6. 显示 + 绘制
        bool useLayered = (hasBorder == 0);
        ShowWindow(hWnd, SW_SHOWNOACTIVATE);
        if (useLayered)
        {        RenderLayeredWindow(hWnd);
    }      // 分层：UpdateLayeredWindow
        else
        {   // 有边框 → 设置二值透明区域
        
            InvalidateRect(hWnd, NULL, FALSE);   
            UpdateWindow(hWnd);
        }
        // 创建后把焦点还给原来的窗口
        if (hPrevForeground && IsWindow(hPrevForeground))
        {
            SetForegroundWindow(hPrevForeground);
            SetFocus(hPrevForeground);
        }

		return (double)(uintptr_t)hWnd; // 返回窗口句柄作为 double
	}
    // ================================================================
// DX_UPDATE：在 GM8.1 的 Step 事件中每帧调用
// 遍历所有窗口，主动把 Bitmap 绘制到各自客户区
// 返回：本次实际绘制的窗口数量
// ================================================================
    DX_SIDE double __cdecl DX_UPDATE()
    {
       if (!g_inited) return 0.0;

        int count = 0;
        for (auto& kv : g_bitmaps)
        {
            HWND hWnd = kv.first;
            if (!IsWindow(hWnd)) continue;
            DWORD exStyle = (DWORD)GetWindowLongPtr(hWnd, GWL_EXSTYLE);
            if (exStyle & WS_EX_LAYERED)
            {
                RenderLayeredWindow(hWnd);
            }
            else
            {
                InvalidateRect(hWnd, NULL, FALSE);
                UpdateWindow(hWnd);
            }   // 立即触发 WM_PAINT
            count++;
        }
        return (double)count;
        //return 0;
    }
    // ================================================================
    // DX_CLOSE_WINDOW：关闭指定窗口
    // ================================================================
    // hwndD : DX_CREATE_PNG_WINDOW 返回的窗口句柄（double）
    // 返回  : 1 = 成功；0 = 失败
    // ================================================================
    DX_SIDE double __cdecl DX_CLOSE_WINDOW(double hwndD)
    {
        HWND hWnd = (HWND)(uintptr_t)hwndD;
        if (!IsWindow(hWnd)) return 0.0;

        // DestroyWindow 会触发 WM_DESTROY
        // 在 WM_DESTROY 里我们已经 delete Bitmap 并从 g_bitmaps 删除
        return DestroyWindow(hWnd) ? 1.0 : 0.0;
    }
    // ================================================================
    // DX_IS_WINDOW_VALID：判断窗口是否存在
    // ================================================================
    // hwndD : DX_CREATE_PNG_WINDOW 返回的窗口句柄（double）
    // 返回  : 1 = 存在且有效；0 = 不存在 / 已销毁 / 不属于本 DLL
    // ================================================================
    DX_SIDE double __cdecl DX_IS_WINDOW_VALID(double hwndD)
    {
        if (!g_inited) return 0.0;

        HWND hWnd = (HWND)(uintptr_t)hwndD;
        if (hWnd == NULL) return 0.0;

        // 条件 1：系统层面是有效窗口
        if (!IsWindow(hWnd)) return 0.0;

        // 条件 2：确实是我们创建并管理着的窗口
        if (g_bitmaps.find(hWnd) == g_bitmaps.end()) return 0.0;

        return 1.0;
    }
    /////////////////////////////////2-窗口调整-/////////////////////////////////////////////////////////
    // ================================================================
// DX_SETTTL：设置窗口标题
// ================================================================
    DX_SIDE double __cdecl DX_SETTTL(double hwndD, const char* titleK)
    {
        if (!titleK) return 0.0;
        HWND hWnd = (HWND)(uintptr_t)hwndD;
        if (!IsWindow(hWnd)) return 0.0;
        if (g_bitmaps.find(hWnd) == g_bitmaps.end()) return 0.0;

        // UTF-8 → 宽字符
        int bufSize = MultiByteToWideChar(CP_UTF8, 0, titleK, -1, NULL, 0);
        if (bufSize <= 0) return 0.0;

        std::wstring wTitle(bufSize, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, titleK, -1, &wTitle[0], bufSize);
        if (!wTitle.empty() && wTitle.back() == L'\0')
            wTitle.pop_back();

        // 真正设置窗口标题
        SetWindowTextW(hWnd, wTitle.c_str());

        // 同时更新全局默认标题（下次创建窗口时使用）
        g_title = wTitle;
        return (double)bufSize;
    }
    // ================================================================
// DX_SET_WINDOW_RECT：修改窗口位置与大小
// ================================================================
// hwndD : DX_CREATE_PNG_WINDOW 返回的窗口句柄（double）
// x, y  : 新的屏幕坐标
// w, h  : 新的窗口宽高（外框尺寸，不是客户区）
// 返回  : 1 = 成功；0 = 失败
// ================================================================
DX_SIDE double __cdecl DX_SET_WINDOW_RECT(double hwndD, double x, double y, double w, double h)
 {
        if (!g_inited) return 0.0;

        HWND hWnd = (HWND)(uintptr_t)hwndD;
        if (!IsWindow(hWnd)) return 0.0;

        if (w <= 0 || h <= 0) return 0.0;

        // 把"客户区尺寸"换算成"窗口外框尺寸"
        DWORD style = (DWORD)GetWindowLongPtr(hWnd, GWL_STYLE);
        DWORD exStyle = (DWORD)GetWindowLongPtr(hWnd, GWL_EXSTYLE);
		//如果不是分层窗口，先清除原来的区域，否则 SetWindowRgn 会失败
        bool isLayered = (exStyle & WS_EX_LAYERED) != 0;
        if (!isLayered)
        {
            SetWindowRgn(hWnd, NULL, FALSE);   // 清除区域，FALSE = 暂不重绘
        }
        RECT rc = { 0, 0, (LONG)w, (LONG)h };
        AdjustWindowRectEx(&rc, style, FALSE, exStyle);
        int winW = rc.right - rc.left;
        int winH = rc.bottom - rc.top;

        BOOL ok = SetWindowPos(
            hWnd, NULL,
            (int)x, (int)y,
            winW, winH,
            SWP_NOZORDER | SWP_NOACTIVATE);

        // 立即触发重绘，让图片按新客户区尺寸拉伸
        InvalidateRect(hWnd, NULL, FALSE);
        UpdateWindow(hWnd);

        return ok ? 1.0 : 0.0;
}
    
// ================================================================
// DX_CHANGE_IMAGE：更换窗口背景图片，窗口大小随之改变
// ================================================================
// hwndD     : DX_CREATE_PNG_WINDOW 返回的窗口句柄
// imagePath : 新图片路径（ANSI 字符串，支持 png/jpg/bmp/gif 等）
// 返回      : 1 = 成功；0 = 失败
// ================================================================
DX_SIDE double __cdecl DX_CHANGE_IMAGE(double hwndD, const char* imagePath)
{
    if (!g_inited) return 0.0;
    if (!imagePath) return 0.0;

    HWND hWnd = (HWND)(uintptr_t)hwndD;
    if (!IsWindow(hWnd)) return 0.0;

    auto it = g_bitmaps.find(hWnd);
    if (it == g_bitmaps.end()) return 0.0;

    // 1. ANSI → 宽字符
    int pSize = MultiByteToWideChar(CP_ACP, 0, imagePath, -1, NULL, 0);
    if (pSize <= 0) return 0.0;
    std::wstring wPath(pSize, L'\0');
    MultiByteToWideChar(CP_ACP, 0, imagePath, -1, &wPath[0], pSize);

    // 2. 先加载新图片（成功后再替换，失败不影响原窗口）
    Bitmap* pNew = Bitmap::FromFile(wPath.c_str());
    if (!pNew || pNew->GetLastStatus() != Ok)
    {
        delete pNew;
        return 0.0;
    }

    // 3. 替换旧图片
    Bitmap* pOld = it->second;
    it->second = pNew;
    delete pOld;

    // 4. 以新图片的【原始尺寸】作为客户区尺寸
    int imgW = (int)pNew->GetWidth();
    int imgH = (int)pNew->GetHeight();

    DWORD style = (DWORD)GetWindowLongPtr(hWnd, GWL_STYLE);
    DWORD exStyle = (DWORD)GetWindowLongPtr(hWnd, GWL_EXSTYLE);

    RECT rc = { 0, 0, imgW, imgH };
    AdjustWindowRectEx(&rc, style, FALSE, exStyle);
    int winW = rc.right - rc.left;
    int winH = rc.bottom - rc.top;

    // 5. 保持左上角坐标不变，只改宽高
    RECT curRect;
    GetWindowRect(hWnd, &curRect);
    int curX = curRect.left;
    int curY = curRect.top;

    SetWindowPos(
        hWnd, NULL,
        curX, curY,
        winW, winH,
        SWP_NOZORDER | SWP_NOACTIVATE);

    //DWORD exStyle = (DWORD)GetWindowLongPtr(hWnd, GWL_EXSTYLE);

    if (exStyle & WS_EX_LAYERED)
    {
        // 分层窗口：用 UpdateLayeredWindow
        RenderLayeredWindow(hWnd);
    }
    else
    {
        

        InvalidateRect(hWnd, NULL, FALSE);
        UpdateWindow(hWnd);
    }
    return 1.0;
    return 1.0;
}// ================================================================
// DX_GET_WINDOW_RECT：获取窗口位置与大小
// ================================================================
// hwndD : DX_CREATE_PNG_WINDOW 返回的窗口句柄（double）
// type: 输出参数(0:x, 1:y, 2:w, 3:h)
// 返回  : type对应的值
// ================================================================
DX_SIDE double __cdecl DX_GET_WINDOW_RECT(double hwndD, double* type)
{
    if (!g_inited) return 0.0;
    if (!type || *type < 0 || *type > 3 || *type != (int)*type) return -1.0;
    
    HWND hWnd = (HWND)(uintptr_t)hwndD;
    if (!IsWindow(hWnd)) return 0.0;

    RECT rc;
    GetWindowRect(hWnd, &rc);
	double result = 0.0;
    switch ((int)*type) {
    case 0:
        result = (double)rc.left;
        break;
    case 1:
        result = (double)rc.top;
        break;
    case 2:
        result = (double)(rc.right - rc.left);
        break;
    case 3:
        result = (double)(rc.bottom - rc.top);
        break;
    default:
        return -1.0;
    }
    return result;
}
// ================================================================
// DX_SET_WINDOW_ZORDER：将窗口插入到指定窗口之后
// ================================================================
// hwndD : DX_CREATE_PNG_WINDOW 返回的窗口句柄（double）
// hwnP: 参考窗口句柄（double）
// 返回  : 操作是否成功
// ================================================================
DX_SIDE double DX_SET_WINDOW_ZORDER(double hwndD,double hwnP)
{
	if (!g_inited) return 0.0;
	HWND hWnd = (HWND)(uintptr_t)hwndD;
    HWND hInsertAfter = NULL; 
    if (hwnP != 0 ) { hInsertAfter = (HWND)(uintptr_t)hwnP;
    
    }
    else {
		return -1.0;
    };

	BOOL ok = SetWindowPos(
		hWnd, hInsertAfter,
		0, 0, 0, 0,
		SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
	return ok ? 1.0 : 0.0;
}
// ================================================================
// DX_SET_DRAGGABLE：设置窗口是否可以用鼠标拖动任意位置
// ================================================================
// hwndD  : 窗口句柄
// enable : 1 = 启用整窗拖动；0 = 只允许拖动标题栏
// 返回   : 1 = 成功；0 = 失败
// ================================================================
DX_SIDE double __cdecl DX_SET_DRAGGABLE(double hwndD, double enable)
{
    if (!g_inited) return 0.0;

    HWND hWnd = (HWND)(uintptr_t)hwndD;
    if (!IsWindow(hWnd)) return 0.0;
    if (g_bitmaps.find(hWnd) == g_bitmaps.end()) return 0.0;

    g_draggable[hWnd] = (enable != 0.0);
    return 1.0;
}

// ================================================================
// DX_GET_DRAGGABLE：查询窗口是否启用整窗拖动
// ================================================================
DX_SIDE double __cdecl DX_GET_DRAGGABLE(double hwndD)
{
    if (!g_inited) return 0.0;

    HWND hWnd = (HWND)(uintptr_t)hwndD;
    if (!IsWindow(hWnd)) return 0.0;

    auto it = g_draggable.find(hWnd);
    if (it == g_draggable.end()) return 0.0;
    return it->second ? 1.0 : 0.0;
}

    









    DX_SIDE double DX_MESSAGE(const char* msg, const char* ttl)
    {
        // 参数顺序：hWnd, 正文, 标题, 类型
        int R = MessageBoxA(NULL, msg, ttl, MB_YESNO | MB_ICONQUESTION);
        return (R == IDYES) ? 1.0 : 0.0;
    }
    DX_SIDE double __cdecl DX_TEST()
    {
        // 用一个全新的窗口类，WndProc 直接用 DefWindowProc
        WNDCLASSEX wc = {};
        wc.cbSize = sizeof(WNDCLASSEX);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = DefWindowProc;                // 系统默认过程
        wc.hInstance = g_hDllInstance;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1); // 白色背景
        wc.lpszClassName = L"DXTestClass";
        RegisterClassEx(&wc);

        HWND h = CreateWindowEx(
            WS_EX_TOPMOST,
            L"DXTestClass", L"TEST WINDOW",
            WS_POPUP | WS_VISIBLE,
            100, 100, 400, 300,
            NULL, NULL, g_hDllInstance, NULL);

        if (h)
        {
            // 主动让系统把白背景画上去
            UpdateWindow(h);
        }
        return (double)(uintptr_t)h;
    }
#ifdef __cplusplus
}
#endif