
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
static std::wstring g_title = L"GMPngWindow";
static const wchar_t* CLASS_NAME = L"GMPngWindowClass";

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
        char buf[64];
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
		DWORD style = WS_POPUP | WS_VISIBLE;
		if (hasBorder) style |= WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
		HWND hWnd = CreateWindowEx(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
			CLASS_NAME,
			g_title.c_str(),
			style,
			x, y,
			bmp->GetWidth(),
			bmp->GetHeight(),
			NULL,
			NULL,
			g_hDllInstance,
			NULL);
		if (!hWnd)
		{
			delete bmp;
			return 0.0;
		}
		// 3. 保存 Bitmap 指针
		g_bitmaps[hWnd] = bmp;
        ShowWindow(hWnd, SW_SHOW);           // 再显示
        UpdateWindow(hWnd);

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
            //if (!IsWindow(hWnd)) continue;
            InvalidateRect(hWnd, NULL, FALSE);
            UpdateWindow(hWnd);   // 立即触发 WM_PAINT
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
    DX_SIDE double __cdecl DX_SETTTL(const char* titleK)
    {
        if (!titleK) return 0.0;
        int bufSize = MultiByteToWideChar(CP_UTF8, 0, titleK, -1, NULL, 0);
        if (bufSize <= 0) return 0.0;
        g_title.resize(bufSize);
        MultiByteToWideChar(CP_UTF8, 0, titleK, -1, &g_title[0], bufSize);
        // 去掉末尾 '\0'
        if (!g_title.empty() && g_title.back() == L'\0')
            g_title.pop_back();
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

    // 6. 立即刷新：此时客户区尺寸 == 新图原始尺寸，图片显示为 1:1
    InvalidateRect(hWnd, NULL, FALSE);
    UpdateWindow(hWnd);

    return 1.0;
    return 1.0;
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