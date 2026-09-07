// volumecontrol.cpp : Volume control via mouse wheel on taskbar
//

#include "framework.h"
#include "volumecontrol.h"
#pragma comment(lib, "ws2_32.lib")

#define MAX_LOADSTRING 100
#define WM_APP_VOLUME_WHEEL (WM_APP + 1)

#define VOLUME_DELTA        0.02f
#define VOLUME_THROTTLE_MS  50
#define DISPLAY_HIDE_MS     1500
#define AUDIO_CHECK_MS      2000

// Global variables
HINSTANCE hInst;
WCHAR szTitle[MAX_LOADSTRING];
WCHAR szWindowClass[MAX_LOADSTRING];
HWND g_hWnd = NULL;
HWND g_hVolumeWnd = NULL;
NOTIFYICONDATAW g_nid = { 0 };
HHOOK g_hMouseHook = NULL;
IAudioEndpointVolume* g_pEndpointVolume = NULL;
bool g_bPreventScreensaver = false;
UINT_PTR g_nVolumeTimerId = 0;
UINT_PTR g_nScreensaverTimerId = 0;
int g_nCurrentVolume = 0;
DWORD g_dwLastVolumeUpdate = 0;
DWORD g_dwLastAudioCheck = 0;

// SysMonitor globals
HWND g_hSysMonitorWnd = NULL;

// 공유 메모리 구조체 (LHMWrapper C#과 동일한 레이아웃)
#pragma pack(push, 4)
struct HardwareData {
    float CpuUsage;   // CPU 사용률 (%)
    float CpuTemp;    // CPU 온도 (C)
    float GpuMemUsedGB; // GPU 메모리 사용량 (GB)
    float GpuUsage;   // GPU 사용률 (%)
    float GpuTemp;    // GPU 온도 (C)
    float MemUsedGB;  // 메모리 사용량 (GB)
    float NetInMBs;   // 다운로드 속도 (MB/s)
    float NetOutMBs;  // 업로드 속도 (MB/s)
};
#pragma pack(pop)

HANDLE g_hMapFile = NULL;
HardwareData* g_pHWData = NULL;
HANDLE g_hHostProcess = NULL;

bool OpenSharedMemory() {
    if (g_hMapFile && g_pHWData) return true;
    if (!g_hMapFile) {
        g_hMapFile = OpenFileMappingW(FILE_MAP_READ, FALSE, L"VolumeControlHWDataV2");
    }
    if (!g_hMapFile) return false;
    if (!g_pHWData) {
        g_pHWData = (HardwareData*)MapViewOfFile(g_hMapFile, FILE_MAP_READ, 0, 0, sizeof(HardwareData));
    }
    if (!g_pHWData) {
        CloseHandle(g_hMapFile);
        g_hMapFile = NULL;
        return false;
    }
    return true;
}

void CloseSharedMemory() {
    if (g_pHWData) { UnmapViewOfFile(g_pHWData); g_pHWData = NULL; }
    if (g_hMapFile) { CloseHandle(g_hMapFile); g_hMapFile = NULL; }
}

// Sampled taskbar background color
COLORREF g_taskbarBgColor = RGB(243, 243, 243);

bool IsWindowsLightTheme() {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        0, KEY_READ, &hKey) == ERROR_SUCCESS)
    {
        DWORD val = 1;
        DWORD sz = sizeof(val);
        LONG res = RegQueryValueExW(hKey, L"SystemUsesLightTheme", NULL, NULL, (LPBYTE)&val, &sz);
        RegCloseKey(hKey);
        if (res == ERROR_SUCCESS) return (val != 0);
    }
    return false;
}

void UpdateTaskbarColors() {
    bool isLight = IsWindowsLightTheme();
    g_taskbarBgColor = isLight ? RGB(243, 243, 243) : RGB(32, 32, 32);

    HWND hTaskbar = FindWindowW(L"Shell_TrayWnd", NULL);
    if (hTaskbar) {
        RECT rcTaskbar;
        GetWindowRect(hTaskbar, &rcTaskbar);
        int x = rcTaskbar.right - 250;
        int y = rcTaskbar.top + (rcTaskbar.bottom - rcTaskbar.top) / 2;
        HDC hdcScreen = GetDC(NULL);
        if (hdcScreen) {
            COLORREF sampled = GetPixel(hdcScreen, x, y);
            if (sampled != CLR_INVALID && sampled != 0) {
                g_taskbarBgColor = sampled;
            }
            ReleaseDC(NULL, hdcScreen);
        }
    }
}

// Function declarations
ATOM MyRegisterClass(HINSTANCE hInstance);
BOOL InitInstance(HINSTANCE, int);
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK VolumeWndProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK SysMonitorWndProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam);

bool InitializeAudio();
void UninitializeAudio();
bool EnsureAudio();
bool ChangeVolume(int delta);
float GetCurrentVolume();
bool SetVolume(float level);

void CreateTrayIcon(HWND hWnd);
void RemoveTrayIcon();
void ShowTrayMenu(HWND hWnd);

bool IsAutoStartEnabled();
void ToggleAutoStart();

void CreateVolumeWindow(HINSTANCE hInstance);
void ShowVolumeDisplay(int volumePercent);
void HideVolumeDisplay();
void PositionVolumeWindow();

void CreateSysMonitorWindow(HINSTANCE hInstance);
void LaunchSysMonitorHost();
void TerminateSysMonitorHost();

bool IsMouseOnTaskbar(POINT pt);
bool IsTaskbarWindow(HWND hwnd);

void ToggleScreensaverBlock();
void HandleVolumeWheel(int delta);

void LogDebug(const wchar_t* msg) {
    FILE* fp = NULL;
    _wfopen_s(&fp, L"d:\\coding\\visualstudio\\volumecontrol\\vc_debug.log", L"a, ccs=UTF-8");
    if (fp) {
        fwprintf(fp, L"[%lu] %s\n", GetTickCount(), msg);
        fclose(fp);
    }
}

// WinMain
int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_ LPWSTR    lpCmdLine,
    _In_ int       nCmdShow)
{
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);
    UNREFERENCED_PARAMETER(nCmdShow);

    LogDebug(L"wWinMain entered");

    HANDLE hMutex = CreateMutex(NULL, TRUE, L"VolumeControlMutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        LogDebug(L"Mutex already exists! Exiting.");
        CloseHandle(hMutex);
        return 0;
    }
    LogDebug(L"Mutex acquired");

    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        LogDebug(L"CoInitializeEx failed!");
        return FALSE;
    }
    LogDebug(L"CoInitializeEx succeeded");

    LoadStringW(hInstance, IDS_APP_TITLE, szTitle, MAX_LOADSTRING);
    LoadStringW(hInstance, IDC_VOLUMECONTROL, szWindowClass, MAX_LOADSTRING);
    MyRegisterClass(hInstance);
    LogDebug(L"MyRegisterClass done");

    if (!InitInstance(hInstance, SW_HIDE)) {
        LogDebug(L"InitInstance failed!");
        CoUninitialize();
        return FALSE;
    }
    LogDebug(L"InitInstance succeeded");

    if (!InitializeAudio()) {
        LogDebug(L"InitializeAudio failed!");
        MessageBox(NULL, L"Failed to initialize audio device.", L"Error", MB_ICONERROR);
        CoUninitialize();
        return FALSE;
    }
    LogDebug(L"InitializeAudio succeeded");

    CreateVolumeWindow(hInstance);
    LogDebug(L"CreateVolumeWindow done");

    LaunchSysMonitorHost();
    LogDebug(L"LaunchSysMonitorHost done");

    CreateSysMonitorWindow(hInstance);
    LogDebug(L"CreateSysMonitorWindow done");

    CreateTrayIcon(g_hWnd);
    LogDebug(L"CreateTrayIcon done");

    g_hMouseHook = SetWindowsHookEx(WH_MOUSE_LL, LowLevelMouseProc, hInstance, 0);
    if (!g_hMouseHook) {
        LogDebug(L"SetWindowsHookEx failed!");
        MessageBox(NULL, L"Failed to install mouse hook.", L"Error", MB_ICONERROR);
    } else {
        LogDebug(L"SetWindowsHookEx succeeded");
    }

    LogDebug(L"Entering message loop");
    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    LogDebug(L"Message loop exited");

    if (g_hMouseHook) {
        UnhookWindowsHookEx(g_hMouseHook);
    }
    RemoveTrayIcon();
    UninitializeAudio();
    TerminateSysMonitorHost();
    LogDebug(L"Cleanup finished. Exiting wWinMain.");

    if (g_nScreensaverTimerId) {
        KillTimer(g_hWnd, g_nScreensaverTimerId);
    }

    CoUninitialize();
    CloseHandle(hMutex);

    return (int)msg.wParam;
}

ATOM MyRegisterClass(HINSTANCE hInstance)
{
    WNDCLASSEXW wcex = { 0 };
    wcex.cbSize = sizeof(WNDCLASSEX);
    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = WndProc;
    wcex.hInstance = hInstance;
    wcex.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_VOLUMECONTROL));
    wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wcex.lpszClassName = szWindowClass;
    wcex.hIconSm = LoadIcon(wcex.hInstance, MAKEINTRESOURCE(IDI_SMALL));

    return RegisterClassExW(&wcex);
}

void CreateVolumeWindow(HINSTANCE hInstance)
{
    WNDCLASSEXW wcVolume = { 0 };
    wcVolume.cbSize = sizeof(WNDCLASSEX);
    wcVolume.style = CS_HREDRAW | CS_VREDRAW;
    wcVolume.lpfnWndProc = VolumeWndProc;
    wcVolume.hInstance = hInstance;
    wcVolume.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcVolume.hbrBackground = CreateSolidBrush(RGB(43, 43, 43));
    wcVolume.lpszClassName = L"VolumeDisplayClass";
    RegisterClassExW(&wcVolume);

    g_hVolumeWnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
        L"VolumeDisplayClass",
        L"VolumeDisplay",
        WS_POPUP,
        0, 0, 300, 50,
        NULL, NULL, hInstance, NULL
    );

    if (g_hVolumeWnd) {
        SetLayeredWindowAttributes(g_hVolumeWnd, 0, 230, LWA_ALPHA);
        PositionVolumeWindow();
        ShowWindow(g_hVolumeWnd, SW_HIDE);
    }
}

void PositionVolumeWindow()
{
    if (!g_hVolumeWnd) return;

    POINT pt;
    GetCursorPos(&pt);

    HMONITOR hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    if (!GetMonitorInfo(hMon, &mi)) {
        int sw = GetSystemMetrics(SM_CXSCREEN);
        int sh = GetSystemMetrics(SM_CYSCREEN);
        mi.rcWork.right = sw;
        mi.rcWork.bottom = sh;
        mi.rcWork.left = 0;
        mi.rcWork.top = 0;
    }

    const int width = 300;
    const int height = 50;
    // Python: x = screen_width - width - 20, y = screen_height - 110 (physical screen bottom)
    int x = mi.rcMonitor.right - width - 20;
    int y = mi.rcMonitor.bottom - 110;

    SetWindowPos(g_hVolumeWnd, HWND_TOPMOST, x, y, width, height,
        SWP_NOACTIVATE | SWP_NOZORDER);
}

BOOL InitInstance(HINSTANCE hInstance, int nCmdShow)
{
    hInst = hInstance;

    g_hWnd = CreateWindowW(szWindowClass, szTitle, WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, 0, CW_USEDEFAULT, 0, nullptr, nullptr, hInstance, nullptr);

    if (!g_hWnd) {
        return FALSE;
    }

    ShowWindow(g_hWnd, SW_HIDE);
    return TRUE;
}

bool InitializeAudio()
{
    if (g_pEndpointVolume) {
        g_pEndpointVolume->Release();
        g_pEndpointVolume = NULL;
    }

    IMMDeviceEnumerator* pEnumerator = NULL;
    IMMDevice* pDevice = NULL;

    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator),
        NULL,
        CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator),
        (void**)&pEnumerator
    );

    if (FAILED(hr)) return false;

    hr = pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice);
    if (FAILED(hr)) {
        pEnumerator->Release();
        return false;
    }

    hr = pDevice->Activate(
        __uuidof(IAudioEndpointVolume),
        CLSCTX_ALL,
        NULL,
        (void**)&g_pEndpointVolume
    );

    pDevice->Release();
    pEnumerator->Release();

    return SUCCEEDED(hr) && g_pEndpointVolume != NULL;
}

void UninitializeAudio()
{
    if (g_pEndpointVolume) {
        g_pEndpointVolume->Release();
        g_pEndpointVolume = NULL;
    }
}

bool EnsureAudio()
{
    DWORD now = GetTickCount();
    if (g_pEndpointVolume && (now - g_dwLastAudioCheck < AUDIO_CHECK_MS)) {
        return true;
    }
    g_dwLastAudioCheck = now;

    if (g_pEndpointVolume) {
        float level = 0.0f;
        HRESULT hr = g_pEndpointVolume->GetMasterVolumeLevelScalar(&level);
        if (SUCCEEDED(hr)) {
            return true;
        }
        g_pEndpointVolume->Release();
        g_pEndpointVolume = NULL;
    }

    return InitializeAudio();
}

float GetCurrentVolume()
{
    if (!g_pEndpointVolume) return 0.0f;

    float level = 0.0f;
    if (FAILED(g_pEndpointVolume->GetMasterVolumeLevelScalar(&level))) {
        return 0.0f;
    }
    return level;
}

bool SetVolume(float level)
{
    if (!g_pEndpointVolume) return false;

    if (level < 0.0f) level = 0.0f;
    if (level > 1.0f) level = 1.0f;

    return SUCCEEDED(g_pEndpointVolume->SetMasterVolumeLevelScalar(level, NULL));
}

bool ChangeVolume(int delta)
{
    if (!EnsureAudio()) {
        return false;
    }

    DWORD currentTime = GetTickCount();
    bool throttled = (currentTime - g_dwLastVolumeUpdate < VOLUME_THROTTLE_MS);

    float currentLevel = GetCurrentVolume();
    float newLevel = currentLevel;

    if (!throttled) {
        newLevel = currentLevel + (delta > 0 ? VOLUME_DELTA : -VOLUME_DELTA);
        if (newLevel < 0.0f) newLevel = 0.0f;
        if (newLevel > 1.0f) newLevel = 1.0f;

        if (!SetVolume(newLevel)) {
            if (!InitializeAudio()) {
                return false;
            }
            if (!SetVolume(newLevel)) {
                return false;
            }
        }
        g_dwLastVolumeUpdate = currentTime;
    }

    int volumePercent = (int)(newLevel * 100.0f + 0.5f);
    ShowVolumeDisplay(volumePercent);
    return !throttled;
}

void HandleVolumeWheel(int delta)
{
    POINT pt;
    GetCursorPos(&pt);
    if (!IsMouseOnTaskbar(pt)) {
        return;
    }

    ChangeVolume(delta);
}

void CreateTrayIcon(HWND hWnd)
{
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(NOTIFYICONDATAW);
    g_nid.hWnd = hWnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = LoadIcon(hInst, MAKEINTRESOURCE(IDI_VOLUMECONTROL));
    wcscpy_s(g_nid.szTip, L"Volume Control");

    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

void RemoveTrayIcon()
{
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
}

void ShowTrayMenu(HWND hWnd)
{
    HMENU hMenu = CreatePopupMenu();

    UINT autoStartFlags = MF_STRING;
    if (IsAutoStartEnabled()) {
        autoStartFlags |= MF_CHECKED;
    }
    AppendMenu(hMenu, autoStartFlags, ID_TRAY_AUTOSTART, L"Auto Start");

    UINT screensaverFlags = MF_STRING;
    if (g_bPreventScreensaver) {
        screensaverFlags |= MF_CHECKED;
    }
    AppendMenu(hMenu, screensaverFlags, ID_TRAY_SCREENSAVER, L"Block Screensaver");

    AppendMenu(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenu(hMenu, MF_STRING, ID_TRAY_EXIT, L"Exit");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hWnd);
    TrackPopupMenu(hMenu, TPM_LEFTALIGN | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hWnd, NULL);
    PostMessage(hWnd, WM_NULL, 0, 0);

    DestroyMenu(hMenu);
}

bool IsAutoStartEnabled()
{
    HKEY hKey;
    if (RegOpenKeyEx(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        0, KEY_READ, &hKey) == ERROR_SUCCESS)
    {
        WCHAR szPath[MAX_PATH];
        DWORD dwSize = sizeof(szPath);
        LONG result = RegQueryValueEx(hKey, L"VolumeControl", NULL, NULL, (LPBYTE)szPath, &dwSize);
        RegCloseKey(hKey);
        return (result == ERROR_SUCCESS);
    }
    return false;
}

void ToggleAutoStart()
{
    HKEY hKey;
    if (RegOpenKeyEx(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        0, KEY_ALL_ACCESS, &hKey) == ERROR_SUCCESS)
    {
        if (IsAutoStartEnabled()) {
            RegDeleteValue(hKey, L"VolumeControl");
        }
        else {
            WCHAR szPath[MAX_PATH];
            GetModuleFileName(NULL, szPath, MAX_PATH);
            RegSetValueEx(hKey, L"VolumeControl", 0, REG_SZ,
                (LPBYTE)szPath, (DWORD)(wcslen(szPath) + 1) * sizeof(WCHAR));
        }
        RegCloseKey(hKey);
    }
}

void ToggleScreensaverBlock()
{
    g_bPreventScreensaver = !g_bPreventScreensaver;

    if (g_bPreventScreensaver) {
        g_nScreensaverTimerId = SetTimer(g_hWnd, 2, 30000, NULL);
        SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED);
    }
    else {
        if (g_nScreensaverTimerId) {
            KillTimer(g_hWnd, g_nScreensaverTimerId);
            g_nScreensaverTimerId = 0;
        }
        SetThreadExecutionState(ES_CONTINUOUS);
    }
}

void ShowVolumeDisplay(int volumePercent)
{
    if (!g_hVolumeWnd || !IsWindow(g_hVolumeWnd)) {
        return;
    }

    g_nCurrentVolume = volumePercent;

    PositionVolumeWindow();

    ShowWindow(g_hVolumeWnd, SW_SHOWNOACTIVATE);
    SetWindowPos(g_hVolumeWnd, HWND_TOPMOST, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);

    InvalidateRect(g_hVolumeWnd, NULL, TRUE);
    UpdateWindow(g_hVolumeWnd);

    if (g_nVolumeTimerId) {
        KillTimer(g_hVolumeWnd, g_nVolumeTimerId);
        g_nVolumeTimerId = 0;
    }
    g_nVolumeTimerId = SetTimer(g_hVolumeWnd, 1, DISPLAY_HIDE_MS, NULL);
}

void HideVolumeDisplay()
{
    if (g_hVolumeWnd && IsWindow(g_hVolumeWnd)) {
        ShowWindow(g_hVolumeWnd, SW_HIDE);
    }
}

bool IsTaskbarWindow(HWND hwnd)
{
    if (!hwnd) return false;

    while (hwnd) {
        WCHAR className[64] = { 0 };
        if (GetClassNameW(hwnd, className, 64) == 0) {
            break;
        }

        if (wcscmp(className, L"Shell_TrayWnd") == 0 ||
            wcscmp(className, L"Shell_SecondaryTrayWnd") == 0) {
            return true;
        }

        hwnd = GetParent(hwnd);
    }
    return false;
}

bool IsMouseOnTaskbar(POINT pt)
{
    HWND hwnd = WindowFromPoint(pt);
    if (IsTaskbarWindow(hwnd)) {
        return true;
    }

    HWND hTray = FindWindowW(L"Shell_TrayWnd", NULL);
    if (hTray) {
        RECT rc = { 0 };
        GetWindowRect(hTray, &rc);
        if (PtInRect(&rc, pt)) {
            return true;
        }
    }

    HWND hSecondary = NULL;
    while ((hSecondary = FindWindowExW(NULL, hSecondary, L"Shell_SecondaryTrayWnd", NULL)) != NULL) {
        RECT rc = { 0 };
        GetWindowRect(hSecondary, &rc);
        if (PtInRect(&rc, pt)) {
            return true;
        }
    }

    return false;
}

LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION && wParam == WM_MOUSEWHEEL) {
        MSLLHOOKSTRUCT* pMouseStruct = (MSLLHOOKSTRUCT*)lParam;
        POINT pt = pMouseStruct->pt;

        if (!IsMouseOnTaskbar(pt)) {
            return CallNextHookEx(g_hMouseHook, nCode, wParam, lParam);
        }

        short zDelta = (short)HIWORD(pMouseStruct->mouseData);
        int delta = (zDelta > 0) ? 1 : -1;

        if (g_hWnd && IsWindow(g_hWnd)) {
            PostMessageW(g_hWnd, WM_APP_VOLUME_WHEEL, (WPARAM)delta, 0);
            return 1;
        }
    }

    return CallNextHookEx(g_hMouseHook, nCode, wParam, lParam);
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_APP_VOLUME_WHEEL:
        HandleVolumeWheel((int)wParam);
        return 0;

    case WM_TRAYICON:
        if (lParam == WM_RBUTTONUP) {
            ShowTrayMenu(hWnd);
        }
        break;

    case WM_COMMAND:
    {
        int wmId = LOWORD(wParam);
        switch (wmId)
        {
        case ID_TRAY_EXIT:
            DestroyWindow(hWnd);
            break;
        case ID_TRAY_AUTOSTART:
            ToggleAutoStart();
            break;
        case ID_TRAY_SCREENSAVER:
            ToggleScreensaverBlock();
            break;
        default:
            return DefWindowProc(hWnd, message, wParam, lParam);
        }
    }
    break;

    case WM_TIMER:
        if (wParam == 2 && g_bPreventScreensaver) {
            INPUT input = { 0 };
            input.type = INPUT_MOUSE;
            input.mi.dwFlags = MOUSEEVENTF_MOVE;
            SendInput(1, &input, sizeof(INPUT));
        }
        break;

    case WM_DESTROY:
        LogDebug(L"WndProc WM_DESTROY called!");
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

LRESULT CALLBACK VolumeWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);

        RECT rcClient;
        GetClientRect(hWnd, &rcClient);

        HBRUSH hBrushBg = CreateSolidBrush(RGB(43, 43, 43));
        FillRect(hdc, &rcClient, hBrushBg);
        DeleteObject(hBrushBg);

        RECT rcBarBg = { 20, 15, rcClient.right - 20, 25 };
        HBRUSH hBrushBarBg = CreateSolidBrush(RGB(64, 64, 64));
        FillRect(hdc, &rcBarBg, hBrushBarBg);
        DeleteObject(hBrushBarBg);

        int barWidth = (int)((rcClient.right - 40) * g_nCurrentVolume / 100.0);
        if (barWidth > 0) {
            RECT rcBar = { 20, 15, 20 + barWidth, 25 };
            HBRUSH hBrushBar = CreateSolidBrush(RGB(255, 255, 255));
            FillRect(hdc, &rcBar, hBrushBar);
            DeleteObject(hBrushBar);
        }

        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(255, 255, 255));

        HFONT hFont = CreateFont(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        HFONT hOldFont = (HFONT)SelectObject(hdc, hFont);

        WCHAR szVolume[32];
        swprintf_s(szVolume, L"Volume: %d%%", g_nCurrentVolume);

        RECT rcText = { 0, 30, rcClient.right, 45 };
        DrawText(hdc, szVolume, -1, &rcText, DT_CENTER | DT_SINGLELINE);

        SelectObject(hdc, hOldFont);
        DeleteObject(hFont);

        EndPaint(hWnd, &ps);
    }
    break;

    case WM_TIMER:
        if (wParam == 1) {
            KillTimer(hWnd, 1);
            g_nVolumeTimerId = 0;
            HideVolumeDisplay();
        }
        break;

    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

LRESULT CALLBACK SysMonitorWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;

    case WM_MOUSEWHEEL:
    {
        short zDelta = (short)HIWORD(wParam);
        int delta = (zDelta > 0) ? 1 : -1;
        ChangeVolume(delta);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1; // Prevent background erasing to eliminate flicker

    case WM_TIMER:
        if (wParam == 3) {
            if (!g_pHWData) OpenSharedMemory();
            InvalidateRect(hWnd, NULL, FALSE);
        }
        break;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT rc;
        GetClientRect(hWnd, &rc);

        // Double buffering: memory DC & bitmap
        HDC memDC = CreateCompatibleDC(hdc);
        HBITMAP memBmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
        HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, memBmp);

        // Fill background with taskbar color
        HBRUSH hBgBrush = CreateSolidBrush(g_taskbarBgColor);
        FillRect(memDC, &rc, hBgBrush);
        DeleteObject(hBgBrush);

        SetBkMode(memDC, TRANSPARENT);

        // Font: Malgun Gothic (맑은 고딕) - clean, readable, crisp glyphs
        HFONT hFont = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Malgun Gothic");
        HFONT oldFont = (HFONT)SelectObject(memDC, hFont);

        // Calculate text metrics for perfect vertical centering
        TEXTMETRICW tm;
        GetTextMetricsW(memDC, &tm);
        int lineH = tm.tmHeight + 2;
        int totalTextH = lineH * 2;
        int yStart = (rc.bottom - totalTextH) / 2;
        if (yStart < 1) yStart = 1;

        int r1 = yStart;
        int r2 = yStart + lineH;

        bool isLight = IsWindowsLightTheme();
        COLORREF clrText  = isLight ? RGB(20, 20, 20)  : RGB(245, 245, 245);
        COLORREF clrAlert = isLight ? RGB(225, 10, 10) : RGB(255, 50, 50);

        MEMORYSTATUSEX ms = { sizeof(ms) };
        GlobalMemoryStatusEx(&ms);
        DWORD memLoad = ms.dwMemoryLoad;

        if (g_pHWData && g_pHWData->MemUsedGB > 0.0f) {
            HardwareData d = *g_pHWData;

            // Col 1: Network (x: 4 .. 100)
            WCHAR szNet1[64], szNet2[64];
            if (d.NetOutMBs < 1.0f)
                swprintf_s(szNet1, L"\u2191: %.0f KB/s", d.NetOutMBs * 1024.0f);
            else
                swprintf_s(szNet1, L"\u2191: %.2f MB/s", d.NetOutMBs);

            if (d.NetInMBs < 1.0f)
                swprintf_s(szNet2, L"\u2193: %.0f KB/s", d.NetInMBs * 1024.0f);
            else
                swprintf_s(szNet2, L"\u2193: %.2f MB/s", d.NetInMBs);

            RECT rcN1 = { 4, r1, 100, r1 + lineH };
            RECT rcN2 = { 4, r2, 100, r2 + lineH };

            // Network speed >= 20 MB/s turns RED
            SetTextColor(memDC, (d.NetOutMBs >= 20.0f) ? clrAlert : clrText);
            DrawTextW(memDC, szNet1, -1, &rcN1, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);

            SetTextColor(memDC, (d.NetInMBs >= 20.0f) ? clrAlert : clrText);
            DrawTextW(memDC, szNet2, -1, &rcN2, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);

            // Col 2: CPU & GPU Usage (x: 104 .. 175)
            WCHAR szU1[64], szU2[64];
            swprintf_s(szU1, L"CPU: %d %%", (int)(d.CpuUsage + 0.5f));
            swprintf_s(szU2, L"GPU: %d %%", (int)(d.GpuUsage + 0.5f));

            RECT rcU1 = { 104, r1, 175, r1 + lineH };
            RECT rcU2 = { 104, r2, 175, r2 + lineH };

            // Usage >= 70% (or 80%) turns RED
            SetTextColor(memDC, (d.CpuUsage >= 70.0f) ? clrAlert : clrText);
            DrawTextW(memDC, szU1, -1, &rcU1, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);

            SetTextColor(memDC, (d.GpuUsage >= 70.0f) ? clrAlert : clrText);
            DrawTextW(memDC, szU2, -1, &rcU2, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);

            // Col 3: Mem Used & GPU Mem Used (x: 178 .. 275)
            WCHAR szC1[64], szC2[64];
            swprintf_s(szC1, L"Mem: %.2f GB", d.MemUsedGB);
            swprintf_s(szC2, L"VRAM: %.2f GB", d.GpuMemUsedGB);

            RECT rcC1 = { 178, r1, 275, r1 + lineH };
            RECT rcC2 = { 178, r2, 275, r2 + lineH };

            // Memory >= 60% turns RED
            SetTextColor(memDC, (memLoad >= 60) ? clrAlert : clrText);
            DrawTextW(memDC, szC1, -1, &rcC1, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);

            // VRAM (GPU Memory)
            SetTextColor(memDC, clrText);
            DrawTextW(memDC, szC2, -1, &rcC2, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);

            // Col 4: Temperatures (x: 278 .. 365)
            WCHAR szT1[64], szT2[64];
            swprintf_s(szT1, L"CPU: %d \u00B0C", (int)(d.CpuTemp + 0.5f));
            swprintf_s(szT2, L"GPU: %d \u00B0C", (int)(d.GpuTemp + 0.5f));

            RECT rcT1 = { 278, r1, 365, r1 + lineH };
            RECT rcT2 = { 278, r2, 365, r2 + lineH };

            // Temperature >= 90°C turns RED, otherwise normal text color
            SetTextColor(memDC, (d.CpuTemp >= 90.0f) ? clrAlert : clrText);
            DrawTextW(memDC, szT1, -1, &rcT1, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);

            SetTextColor(memDC, (d.GpuTemp >= 90.0f) ? clrAlert : clrText);
            DrawTextW(memDC, szT2, -1, &rcT2, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
        }
        else {
            SetTextColor(memDC, isLight ? RGB(100, 100, 100) : RGB(160, 160, 160));
            RECT rcWait = { 6, r1, rc.right, r1 + lineH };
            DrawTextW(memDC, L"Waiting for hardware data...", -1, &rcWait, DT_LEFT | DT_SINGLELINE);
        }

        BitBlt(hdc, 0, 0, rc.right, rc.bottom, memDC, 0, 0, SRCCOPY);

        SelectObject(memDC, oldFont);
        DeleteObject(hFont);
        SelectObject(memDC, oldBmp);
        DeleteObject(memBmp);
        DeleteDC(memDC);

        EndPaint(hWnd, &ps);
    }
    break;

    case WM_DESTROY:
        if (hWnd == g_hSysMonitorWnd) {
            KillTimer(hWnd, 3);
            CloseSharedMemory();
        }
        break;

    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

void CreateSysMonitorWindow(HINSTANCE hInstance)
{
    HWND hTaskbar = FindWindowW(L"Shell_TrayWnd", NULL);

    int width = 365;
    int height = 40;
    int xPos = 0, yPos = 0;

    if (hTaskbar) {
        RECT rcTaskbar;
        GetWindowRect(hTaskbar, &rcTaskbar);
        height = rcTaskbar.bottom - rcTaskbar.top;
        if (height < 36) height = 36;
        yPos = rcTaskbar.top;

        // Position moved further right (directly left of the tray icons)
        xPos = rcTaskbar.right - width - 240;

        HWND hTrayNotify = FindWindowExW(hTaskbar, NULL, L"TrayNotifyWnd", NULL);
        if (hTrayNotify) {
            RECT rcTrayNotify = { 0 };
            GetWindowRect(hTrayNotify, &rcTrayNotify);
            if (rcTrayNotify.left > rcTaskbar.left + width) {
                xPos = rcTrayNotify.left - width - 8;
            }
        }

        if (xPos < 0) xPos = 0;
    }

    UpdateTaskbarColors();

    WNDCLASSEXW wcMon = { 0 };
    wcMon.cbSize        = sizeof(WNDCLASSEX);
    wcMon.style         = CS_HREDRAW | CS_VREDRAW;
    wcMon.lpfnWndProc   = SysMonitorWndProc;
    wcMon.hInstance     = hInstance;
    wcMon.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wcMon.hbrBackground = CreateSolidBrush(g_taskbarBgColor);
    wcMon.lpszClassName = L"SysMonitorClass";
    RegisterClassExW(&wcMon);

    g_hSysMonitorWnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        L"SysMonitorClass",
        L"",
        WS_POPUP | WS_CLIPSIBLINGS,
        xPos, yPos, width, height,
        hTaskbar ? hTaskbar : NULL,
        NULL, hInstance, NULL
    );

    if (g_hSysMonitorWnd) {
        SetWindowPos(g_hSysMonitorWnd, HWND_TOPMOST, xPos, yPos, width, height,
            SWP_NOACTIVATE | SWP_SHOWWINDOW);
        OpenSharedMemory();
        SetTimer(g_hSysMonitorWnd, 3, 1000, NULL);
    }
}

void LaunchSysMonitorHost()
{
    WCHAR exePath[MAX_PATH];
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    WCHAR* lastSlash = wcsrchr(exePath, L'\\');
    if (!lastSlash) return;

    wcscpy_s(lastSlash + 1, MAX_PATH - (lastSlash - exePath) - 1, L"SysMonitorHost.exe");

    if (GetFileAttributesW(exePath) == INVALID_FILE_ATTRIBUTES) {
        return;
    }

    // Check if already running
    HANDLE hMutex = OpenMutexW(MUTEX_ALL_ACCESS, FALSE, L"VolumeControlHWHostMutexV2");
    if (hMutex) {
        CloseHandle(hMutex);
        return;
    }

    WCHAR cmdLine[MAX_PATH + 64];
    swprintf_s(cmdLine, L"\"%s\" --parent %lu", exePath, GetCurrentProcessId());

    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = { 0 };

    if (CreateProcessW(NULL, cmdLine, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        g_hHostProcess = pi.hProcess;
        CloseHandle(pi.hThread);
    }
}

void TerminateSysMonitorHost()
{
    if (g_hHostProcess) {
        TerminateProcess(g_hHostProcess, 0);
        CloseHandle(g_hHostProcess);
        g_hHostProcess = NULL;
    }
}
