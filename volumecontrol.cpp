// volumecontrol.cpp : Volume control via mouse wheel on taskbar
//

#include "framework.h"
#include "volumecontrol.h"
#pragma comment(lib, "ws2_32.lib")
#pragma comment(linker,"\"/manifestdependency:type='win32' \
name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#include <TlHelp32.h>

#define MAX_LOADSTRING 100
#define WM_APP_VOLUME_WHEEL   (WM_APP + 1)
#define WM_APP_DEVICE_CHANGED (WM_APP + 2)

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
IMMDeviceEnumerator* g_pEnumerator = NULL;
class CAudioNotificationClient;
CAudioNotificationClient* g_pNotificationClient = NULL;
volatile LONG g_lAudioDeviceChanged = 0;
std::wstring g_strCurrentDeviceId;
#define WM_APP_REORDER_METRIC (WM_APP + 10)

enum MetricType {
    METRIC_NET_OUT = 0,   // 업로드 속도 (↑)
    METRIC_NET_IN = 1,    // 다운로드 속도 (↓)
    METRIC_CPU_USAGE = 2, // CPU 사용률
    METRIC_GPU_USAGE = 3, // GPU 사용률
    METRIC_RAM_USED = 4,  // 메모리 사용량 (RAM)
    METRIC_GPU_MEM = 5,   // GPU 메모리 (VRAM)
    METRIC_CPU_TEMP = 6,  // CPU 온도
    METRIC_GPU_TEMP = 7,  // GPU 온도
    METRIC_COUNT = 8
};

struct MetricItem {
    MetricType type;
    bool enabled;
};

struct MetricInfo {
    const wchar_t* name;
    const wchar_t* label;
    int defaultWidth;
};

const MetricInfo g_metricMetadata[METRIC_COUNT] = {
    { L"↑ 업로드",   L"\u2191:", 78 },
    { L"↓ 다운로드", L"\u2193:", 78 },
    { L"CPU 사용률", L"CPU:",    56 },
    { L"GPU 사용률", L"GPU:",    56 },
    { L"RAM 메모리", L"Mem:",    86 },
    { L"VRAM",       L"VRAM:",   86 },
    { L"CPU 온도",   L"CPU:",    62 },
    { L"GPU 온도",   L"GPU:",    62 }
};

MetricItem g_metricItems[METRIC_COUNT] = {
    { METRIC_NET_OUT, true },
    { METRIC_NET_IN, true },
    { METRIC_CPU_USAGE, true },
    { METRIC_GPU_USAGE, true },
    { METRIC_RAM_USED, true },
    { METRIC_GPU_MEM, true },
    { METRIC_CPU_TEMP, true },
    { METRIC_GPU_TEMP, true }
};

bool g_bPreventScreensaver = false;
bool g_bDisableEfficiency = true;
UINT_PTR g_nVolumeTimerId = 0;
UINT_PTR g_nScreensaverTimerId = 0;
int g_nCurrentVolume = 0;
DWORD g_dwLastVolumeUpdate = 0;
DWORD g_dwLastAudioCheck = 0;

// Shutdown timer globals
bool g_bShutdownTimerActive = false;
int g_shutdownTimerSecondsLeft = 0;
UINT_PTR g_nShutdownTimerId = 0;
#define SHUTDOWN_TIMER_ID 4

// SysMonitor globals
HWND g_hSysMonitorWnd = NULL;
bool g_bMenuOpen = false;

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
    COLORREF defaultColor = isLight ? RGB(243, 243, 243) : RGB(32, 32, 32);
    g_taskbarBgColor = defaultColor;

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
                int r = GetRValue(sampled);
                int g = GetGValue(sampled);
                int b = GetBValue(sampled);
                int brightness = (r * 299 + g * 587 + b * 114) / 1000;
                int maxDiff = abs(r - g);
                if (abs(g - b) > maxDiff) maxDiff = abs(g - b);
                if (abs(b - r) > maxDiff) maxDiff = abs(b - r);

                // 원색 계열(아이콘/알림 등)이 아니고 테마 밝기와 부합할 때만 샘플링 색상 채택
                if (maxDiff < 40) {
                    if (isLight && brightness >= 160) {
                        g_taskbarBgColor = sampled;
                    } else if (!isLight && brightness <= 90) {
                        g_taskbarBgColor = sampled;
                    }
                }
            }
            ReleaseDC(NULL, hdcScreen);
        }
    }

    // 모니터링 창이 이미 생성되어 있다면 ColorKey를 동기화하여 경계선 찌꺼기 방지
    extern HWND g_hSysMonitorWnd;
    if (g_hSysMonitorWnd && IsWindow(g_hSysMonitorWnd)) {
        SetLayeredWindowAttributes(g_hSysMonitorWnd, g_taskbarBgColor, 0, LWA_COLORKEY);
    }
}

// DPI 헬퍼 함수 선언 및 구현
void InitDpiAwareness()
{
    // 1. Windows 10 1703 (Creators Update) 이상: Per-Monitor V2
    HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
    if (hUser32) {
        typedef BOOL (WINAPI *SetProcessDpiAwarenessContextProc)(HANDLE);
        SetProcessDpiAwarenessContextProc setContext = 
            (SetProcessDpiAwarenessContextProc)GetProcAddress(hUser32, "SetProcessDpiAwarenessContext");
        if (setContext) {
            // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 = ((HANDLE)-4)
            if (setContext((HANDLE)-4)) {
                return;
            }
        }
    }

    // 2. Windows 8.1 이상: PROCESS_PER_MONITOR_DPI_AWARE
    HMODULE hShcore = LoadLibraryW(L"shcore.dll");
    if (hShcore) {
        typedef enum {
            PROCESS_DPI_UNAWARE = 0,
            PROCESS_SYSTEM_DPI_AWARE = 1,
            PROCESS_PER_MONITOR_DPI_AWARE = 2
        } PROCESS_DPI_AWARENESS;
        typedef HRESULT (WINAPI *SetProcessDpiAwarenessProc)(PROCESS_DPI_AWARENESS);
        SetProcessDpiAwarenessProc setDpiAwareness = 
            (SetProcessDpiAwarenessProc)GetProcAddress(hShcore, "SetProcessDpiAwareness");
        if (setDpiAwareness) {
            if (SUCCEEDED(setDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE))) {
                FreeLibrary(hShcore);
                return;
            }
        }
        FreeLibrary(hShcore);
    }

    // 3. Windows Vista 이상: System DPI Aware fallback
    SetProcessDPIAware();
}

UINT GetWindowDpi(HWND hWnd)
{
    typedef UINT (WINAPI *GetDpiForWindowProc)(HWND);
    static GetDpiForWindowProc pGetDpiForWindow = (GetDpiForWindowProc)GetProcAddress(
        GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
    if (pGetDpiForWindow && hWnd && IsWindow(hWnd)) {
        UINT dpi = pGetDpiForWindow(hWnd);
        if (dpi > 0) return dpi;
    }

    HDC hdc = GetDC(hWnd);
    if (hdc) {
        int dpi = GetDeviceCaps(hdc, LOGPIXELSY);
        ReleaseDC(hWnd, hdc);
        if (dpi > 0) return (UINT)dpi;
    }
    return 96;
}

// Function declarations
ATOM MyRegisterClass(HINSTANCE hInstance);
BOOL InitInstance(HINSTANCE, int);
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK VolumeWndProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK SysMonitorWndProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam);
INT_PTR CALLBACK SettingsDlgProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam);
INT_PTR CALLBACK ShutdownTimerDlgProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK MetricGridWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

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
bool IsFullscreenAppRunning();

void ToggleScreensaverBlock();
void HandleVolumeWheel(int delta);

void LoadSettings();
void SaveSettings();
INT_PTR CALLBACK SettingsDlgProc(HWND, UINT, WPARAM, LPARAM);
void UpdateSysMonitorLayout();
void DisableEfficiencyMode();
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

    // Per-Monitor V2 DPI 인식을 활성화하여 다중 모니터 및 배율 변경 시 DWM 비트맵 스트레칭 방지
    InitDpiAwareness();

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    // Prevent running in background sessions (e.g., from other users' Scheduled Tasks)
    // to avoid locking WinRing0 driver and hiding the UI from the active user.
    DWORD dwConsoleSessionId = WTSGetActiveConsoleSessionId();
    DWORD dwCurrentSessionId = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &dwCurrentSessionId);
    if (dwConsoleSessionId != 0xFFFFFFFF && dwCurrentSessionId != dwConsoleSessionId) {
        return 0; // Exit silently
    }

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

    WNDCLASSEXW wcGrid = { 0 };
    wcGrid.cbSize = sizeof(WNDCLASSEX);
    wcGrid.style = CS_HREDRAW | CS_VREDRAW;
    wcGrid.lpfnWndProc = MetricGridWndProc;
    wcGrid.hInstance = hInstance;
    wcGrid.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcGrid.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wcGrid.lpszClassName = L"MetricGridClass";
    RegisterClassExW(&wcGrid);

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

    LoadSettings();

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

    UINT dpi = GetWindowDpi(g_hVolumeWnd);
    int width = MulDiv(300, dpi, 96);
    int height = MulDiv(50, dpi, 96);
    // Python: x = screen_width - width - 20, y = screen_height - 110 (physical screen bottom)
    int x = mi.rcMonitor.right - width - MulDiv(20, dpi, 96);
    int y = mi.rcMonitor.bottom - MulDiv(110, dpi, 96);

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

class CAudioNotificationClient : public IMMNotificationClient
{
    LONG m_cRef;
public:
    CAudioNotificationClient() : m_cRef(1) {}
    virtual ~CAudioNotificationClient() {}

    // IUnknown methods
    ULONG STDMETHODCALLTYPE AddRef() override {
        return InterlockedIncrement(&m_cRef);
    }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG ulRef = InterlockedDecrement(&m_cRef);
        if (ulRef == 0) {
            delete this;
        }
        return ulRef;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, VOID** ppvInterface) override {
        if (!ppvInterface) return E_POINTER;
        if (riid == __uuidof(IUnknown)) {
            AddRef();
            *ppvInterface = static_cast<IUnknown*>(this);
            return S_OK;
        }
        if (riid == __uuidof(IMMNotificationClient)) {
            AddRef();
            *ppvInterface = static_cast<IMMNotificationClient*>(this);
            return S_OK;
        }
        *ppvInterface = NULL;
        return E_NOINTERFACE;
    }

    // IMMNotificationClient methods
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR pwstrDefaultDeviceId) override {
        if (flow == eRender && (role == eConsole || role == eMultimedia)) {
            InterlockedExchange(&g_lAudioDeviceChanged, 1);
            if (g_hWnd && IsWindow(g_hWnd)) {
                PostMessageW(g_hWnd, WM_APP_DEVICE_CHANGED, 0, 0);
            }
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR pwstrDeviceId, DWORD dwNewState) override {
        InterlockedExchange(&g_lAudioDeviceChanged, 1);
        if (g_hWnd && IsWindow(g_hWnd)) {
            PostMessageW(g_hWnd, WM_APP_DEVICE_CHANGED, 0, 0);
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR pwstrDeviceId) override {
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR pwstrDeviceId) override {
        InterlockedExchange(&g_lAudioDeviceChanged, 1);
        if (g_hWnd && IsWindow(g_hWnd)) {
            PostMessageW(g_hWnd, WM_APP_DEVICE_CHANGED, 0, 0);
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR pwstrDeviceId, const PROPERTYKEY key) override {
        return S_OK;
    }
};

bool InitializeAudio()
{
    if (g_pEndpointVolume) {
        g_pEndpointVolume->Release();
        g_pEndpointVolume = NULL;
    }

    if (!g_pEnumerator) {
        HRESULT hr = CoCreateInstance(
            __uuidof(MMDeviceEnumerator),
            NULL,
            CLSCTX_ALL,
            __uuidof(IMMDeviceEnumerator),
            (void**)&g_pEnumerator
        );
        if (FAILED(hr) || !g_pEnumerator) return false;

        if (!g_pNotificationClient) {
            g_pNotificationClient = new CAudioNotificationClient();
            HRESULT hrCb = g_pEnumerator->RegisterEndpointNotificationCallback(g_pNotificationClient);
            if (FAILED(hrCb)) {
                g_pNotificationClient->Release();
                g_pNotificationClient = NULL;
            }
        }
    }

    IMMDevice* pDevice = NULL;
    HRESULT hr = g_pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice);
    if (FAILED(hr) || !pDevice) {
        // If enumerator is stale or disconnected (other than no devices present), reset enumerator
        if (hr != E_NOTFOUND) {
            if (g_pNotificationClient) {
                g_pEnumerator->UnregisterEndpointNotificationCallback(g_pNotificationClient);
                g_pNotificationClient->Release();
                g_pNotificationClient = NULL;
            }
            g_pEnumerator->Release();
            g_pEnumerator = NULL;
        }
        g_strCurrentDeviceId.clear();
        return false;
    }

    LPWSTR pstrId = NULL;
    if (SUCCEEDED(pDevice->GetId(&pstrId)) && pstrId) {
        g_strCurrentDeviceId = pstrId;
        CoTaskMemFree(pstrId);
    } else {
        g_strCurrentDeviceId.clear();
    }

    hr = pDevice->Activate(
        __uuidof(IAudioEndpointVolume),
        CLSCTX_ALL,
        NULL,
        (void**)&g_pEndpointVolume
    );

    pDevice->Release();

    return SUCCEEDED(hr) && g_pEndpointVolume != NULL;
}

void UninitializeAudio()
{
    if (g_pEndpointVolume) {
        g_pEndpointVolume->Release();
        g_pEndpointVolume = NULL;
    }
    if (g_pEnumerator) {
        if (g_pNotificationClient) {
            g_pEnumerator->UnregisterEndpointNotificationCallback(g_pNotificationClient);
            g_pNotificationClient->Release();
            g_pNotificationClient = NULL;
        }
        g_pEnumerator->Release();
        g_pEnumerator = NULL;
    }
    g_strCurrentDeviceId.clear();
}

bool EnsureAudio()
{
    bool needRefresh = (InterlockedCompareExchange(&g_lAudioDeviceChanged, 0, 1) == 1);
    DWORD now = GetTickCount();

    if (!needRefresh && g_pEndpointVolume && (now - g_dwLastAudioCheck < AUDIO_CHECK_MS)) {
        return true;
    }
    g_dwLastAudioCheck = now;

    if (!needRefresh && g_pEndpointVolume) {
        // Fallback: Check if default audio endpoint ID has changed
        if (g_pEnumerator) {
            IMMDevice* pDefaultDev = NULL;
            HRESULT hr = g_pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pDefaultDev);
            if (SUCCEEDED(hr) && pDefaultDev) {
                LPWSTR pstrId = NULL;
                if (SUCCEEDED(pDefaultDev->GetId(&pstrId)) && pstrId) {
                    if (g_strCurrentDeviceId.empty() || wcscmp(g_strCurrentDeviceId.c_str(), pstrId) != 0) {
                        needRefresh = true;
                    }
                    CoTaskMemFree(pstrId);
                }
                pDefaultDev->Release();
            } else {
                needRefresh = true;
            }
        }

        if (!needRefresh) {
            float level = 0.0f;
            HRESULT hr = g_pEndpointVolume->GetMasterVolumeLevelScalar(&level);
            if (FAILED(hr)) {
                needRefresh = true;
            }
        }
    }

    if (needRefresh || !g_pEndpointVolume) {
        return InitializeAudio();
    }

    return true;
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

    float currentLevel = GetCurrentVolume();
    
    // 지원: 부드러운 스크롤 마우스 (120 이하의 delta값도 정밀하게 볼륨 조절 가능)
    float step = ((float)delta / 120.0f) * VOLUME_DELTA;
    float newLevel = currentLevel + step;

    if (newLevel < 0.0f) newLevel = 0.0f;
    if (newLevel > 1.0f) newLevel = 1.0f;

    // 시스템이 음소거 상태일 경우 볼륨을 조절하면 즉시 음소거 해제 (표준 윈도우 동작)
    BOOL isMuted = FALSE;
    if (SUCCEEDED(g_pEndpointVolume->GetMute(&isMuted)) && isMuted) {
        g_pEndpointVolume->SetMute(FALSE, NULL);
    }

    if (!SetVolume(newLevel)) {
        if (!InitializeAudio()) {
            return false;
        }
        if (!SetVolume(newLevel)) {
            return false;
        }
    }

    int volumePercent = (int)(newLevel * 100.0f + 0.5f);
    ShowVolumeDisplay(volumePercent);
    return true;
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

    if (!Shell_NotifyIconW(NIM_ADD, &g_nid)) {
        SetTimer(hWnd, 5, 2000, NULL);
    }
}

void RemoveTrayIcon()
{
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
}

void ShowTrayMenu(HWND hWnd)
{
    HMENU hMenu = CreatePopupMenu();
    if (g_bShutdownTimerActive) {
        int h = g_shutdownTimerSecondsLeft / 3600;
        int m = (g_shutdownTimerSecondsLeft % 3600) / 60;
        int s = g_shutdownTimerSecondsLeft % 60;
        WCHAR szTimer[64];
        swprintf_s(szTimer, L"컴퓨터 종료 타이머 취소 (%02d:%02d:%02d)", h, m, s);
        AppendMenu(hMenu, MF_STRING, ID_TRAY_SHUTDOWNTIMER, szTimer);
    } else {
        AppendMenu(hMenu, MF_STRING, ID_TRAY_SHUTDOWNTIMER, L"컴퓨터 종료 타이머...");
    }
    AppendMenu(hMenu, MF_STRING, ID_TRAY_SETTINGS, L"설정...");
    AppendMenu(hMenu, MF_STRING, ID_TRAY_EXIT, L"종료");

    POINT pt;
    GetCursorPos(&pt);

    // 메뉴가 열려 있는 동안 타이머에 의한 Z-order 재배치 방지
    g_bMenuOpen = true;

    // 포그라운드로 활성화 (외부 클릭 시 닫힘 보장 및 포커스 획득)
    SetForegroundWindow(hWnd);

    TPMPARAMS tpm = { sizeof(TPMPARAMS) };
    // 모니터링 창 영역을 배제 영역(rcExclude)으로 설정하여 메뉴가 모니터링 화면을 덮지 않도록 방지
    if (g_hSysMonitorWnd && IsWindow(g_hSysMonitorWnd)) {
        GetWindowRect(g_hSysMonitorWnd, &tpm.rcExclude);
    }

    UINT cmd = TrackPopupMenuEx(
        hMenu,
        TPM_RETURNCMD | TPM_LEFTALIGN | TPM_BOTTOMALIGN | TPM_RIGHTBUTTON,
        pt.x, pt.y,
        hWnd,
        &tpm
    );

    PostMessage(hWnd, WM_NULL, 0, 0);
    g_bMenuOpen = false;
    DestroyMenu(hMenu);

    if (cmd != 0) {
        SendMessage(g_hWnd, WM_COMMAND, MAKEWPARAM(cmd, 0), 0);
    }
}

bool IsAutoStartEnabled()
{
    WCHAR szUserName[256];
    DWORD dwSize = 256;
    GetUserNameW(szUserName, &dwSize);
    WCHAR szTaskName[512];
    swprintf_s(szTaskName, L"VolumeControl_%s", szUserName);

    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = { 0 };
    WCHAR cmd[1024];
    swprintf_s(cmd, L"schtasks.exe /query /tn \"%s\"", szTaskName);
    if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD exitCode = 1;
        GetExitCodeProcess(pi.hProcess, &exitCode);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return (exitCode == 0);
    }
    return false;
}

void SetAutoStart(bool bEnable)
{
    HKEY hKey;
    if (RegOpenKeyEx(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_ALL_ACCESS, &hKey) == ERROR_SUCCESS) {
        RegDeleteValue(hKey, L"VolumeControl");
        RegCloseKey(hKey);
    }

    WCHAR szUserName[256];
    DWORD dwSize = 256;
    GetUserNameW(szUserName, &dwSize);
    WCHAR szTaskName[512];
    swprintf_s(szTaskName, L"VolumeControl_%s", szUserName);

    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = { 0 };

    // Legacy task cleanup (just in case)
    WCHAR cmdLegacy[] = L"schtasks.exe /delete /tn \"VolumeControl\" /f";
    if (CreateProcessW(NULL, cmdLegacy, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, INFINITE);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }

    if (!bEnable) {
        WCHAR cmd[1024];
        swprintf_s(cmd, L"schtasks.exe /delete /tn \"%s\" /f", szTaskName);
        if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
            WaitForSingleObject(pi.hProcess, INFINITE);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        }
    }
    else {
        WCHAR szPath[MAX_PATH];
        GetModuleFileNameW(NULL, szPath, MAX_PATH);
        WCHAR cmd[2048];
        swprintf_s(cmd, L"schtasks.exe /create /tn \"%s\" /tr \"\\\"%s\\\"\" /rl highest /sc onlogon /f", szTaskName, szPath);
        if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
            WaitForSingleObject(pi.hProcess, INFINITE);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        }
    }
}

void SetScreensaverBlock(bool bEnable)
{
    g_bPreventScreensaver = bEnable;

    if (g_bPreventScreensaver) {
        if (!g_nScreensaverTimerId) {
            g_nScreensaverTimerId = SetTimer(g_hWnd, 2, 30000, NULL);
        }
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

bool IsFullscreenAppRunning()
{
    QUERY_USER_NOTIFICATION_STATE state;
    if (SUCCEEDED(SHQueryUserNotificationState(&state))) {
        if (state == QUNS_RUNNING_D3D_FULL_SCREEN || 
            state == QUNS_PRESENTATION_MODE || 
            state == QUNS_BUSY) 
        {
            return true;
        }
    }

    HWND hWnd = GetForegroundWindow();
    if (!hWnd || hWnd == GetDesktopWindow() || hWnd == GetShellWindow()) {
        return false;
    }

    RECT rcClient;
    GetClientRect(hWnd, &rcClient);
    POINT pt = { 0, 0 };
    ClientToScreen(hWnd, &pt);
    
    int width = rcClient.right - rcClient.left;
    int height = rcClient.bottom - rcClient.top;
    
    rcClient.left = pt.x;
    rcClient.top = pt.y;
    rcClient.right = pt.x + width;
    rcClient.bottom = pt.y + height;

    HMONITOR hMonitor = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    if (GetMonitorInfoW(hMonitor, &mi)) {
        return (rcClient.left <= mi.rcMonitor.left &&
                rcClient.top <= mi.rcMonitor.top &&
                rcClient.right >= mi.rcMonitor.right &&
                rcClient.bottom >= mi.rcMonitor.bottom);
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

        if (!IsMouseOnTaskbar(pt) || IsFullscreenAppRunning()) {
            return CallNextHookEx(g_hMouseHook, nCode, wParam, lParam);
        }

        short zDelta = (short)HIWORD(pMouseStruct->mouseData);

        if (g_hWnd && IsWindow(g_hWnd)) {
            PostMessageW(g_hWnd, WM_APP_VOLUME_WHEEL, (WPARAM)zDelta, 0);
            return 1;
        }
    }

    return CallNextHookEx(g_hMouseHook, nCode, wParam, lParam);
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    static UINT s_uTaskbarRestart = 0;
    if (s_uTaskbarRestart == 0) {
        s_uTaskbarRestart = RegisterWindowMessageW(L"TaskbarCreated");
        ChangeWindowMessageFilter(s_uTaskbarRestart, MSGFLT_ADD);
    }

    if (message == s_uTaskbarRestart && s_uTaskbarRestart != 0) {
        CreateTrayIcon(hWnd);
        if (g_hSysMonitorWnd && IsWindow(g_hSysMonitorWnd)) {
            DestroyWindow(g_hSysMonitorWnd);
        }
        CreateSysMonitorWindow(hInst);
        return 0;
    }

    switch (message)
    {
    case WM_APP_VOLUME_WHEEL:
        HandleVolumeWheel((int)wParam);
        return 0;

    case WM_APP_DEVICE_CHANGED:
        EnsureAudio();
        return 0;

    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED:
        UpdateTaskbarColors();
        UpdateSysMonitorLayout();
        if (g_hSysMonitorWnd && IsWindow(g_hSysMonitorWnd)) {
            InvalidateRect(g_hSysMonitorWnd, NULL, TRUE);
        }
        break;

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
        case ID_TRAY_SETTINGS:
            DialogBox(hInst, MAKEINTRESOURCE(IDD_SETTINGS), hWnd, SettingsDlgProc);
            break;
        case ID_TRAY_SHUTDOWNTIMER:
            if (g_bShutdownTimerActive) {
                KillTimer(hWnd, g_nShutdownTimerId);
                g_bShutdownTimerActive = false;
                g_nShutdownTimerId = 0;
                wcscpy_s(g_nid.szTip, L"Volume Control");
                Shell_NotifyIconW(NIM_MODIFY, &g_nid);
            } else {
                DialogBox(hInst, MAKEINTRESOURCE(IDD_SHUTDOWNTIMER), hWnd, ShutdownTimerDlgProc);
            }
            break;
        case ID_TRAY_EXIT:
            DestroyWindow(hWnd);
            break;
        default:
            return DefWindowProc(hWnd, message, wParam, lParam);
        }
    }
    break;

    case WM_TIMER:
        if (wParam == SHUTDOWN_TIMER_ID && g_bShutdownTimerActive) {
            g_shutdownTimerSecondsLeft--;
            if (g_shutdownTimerSecondsLeft <= 0) {
                KillTimer(hWnd, g_nShutdownTimerId);
                g_bShutdownTimerActive = false;
                g_nShutdownTimerId = 0;
                wcscpy_s(g_nid.szTip, L"Volume Control");
                Shell_NotifyIconW(NIM_MODIFY, &g_nid);
                system("shutdown /s /t 0");
            } else {
                int h = g_shutdownTimerSecondsLeft / 3600;
                int m = (g_shutdownTimerSecondsLeft % 3600) / 60;
                int s = g_shutdownTimerSecondsLeft % 60;
                swprintf_s(g_nid.szTip, L"Volume Control\n종료까지: %02d:%02d:%02d", h, m, s);
                Shell_NotifyIconW(NIM_MODIFY, &g_nid);
            }
        }
        else if (wParam == 2 && g_bPreventScreensaver) {
            INPUT input = { 0 };
            input.type = INPUT_MOUSE;
            input.mi.dwFlags = MOUSEEVENTF_MOVE;
            SendInput(1, &input, sizeof(INPUT));
        }
        else if (wParam == 5) {
            KillTimer(hWnd, 5);
            if (!Shell_NotifyIconW(NIM_ADD, &g_nid)) {
                SetTimer(hWnd, 5, 2000, NULL);
            }
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

        int dpi = GetDeviceCaps(hdc, LOGPIXELSY);
        if (dpi <= 0) dpi = 96;

        int marginX = MulDiv(20, dpi, 96);
        int barTop = MulDiv(15, dpi, 96);
        int barBottom = MulDiv(25, dpi, 96);

        RECT rcBarBg = { marginX, barTop, rcClient.right - marginX, barBottom };
        HBRUSH hBrushBarBg = CreateSolidBrush(RGB(64, 64, 64));
        FillRect(hdc, &rcBarBg, hBrushBarBg);
        DeleteObject(hBrushBarBg);

        int barWidth = (int)((rcClient.right - marginX * 2) * g_nCurrentVolume / 100.0);
        if (barWidth > 0) {
            RECT rcBar = { marginX, barTop, marginX + barWidth, barBottom };
            HBRUSH hBrushBar = CreateSolidBrush(RGB(255, 255, 255));
            FillRect(hdc, &rcBar, hBrushBar);
            DeleteObject(hBrushBar);
        }

        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(255, 255, 255));

        int fontHeight = -MulDiv(14, dpi, 96);
        HFONT hFont = CreateFontW(fontHeight, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        HFONT hOldFont = (HFONT)SelectObject(hdc, hFont);

        WCHAR szVolume[32];
        swprintf_s(szVolume, L"Volume: %d%%", g_nCurrentVolume);

        RECT rcText = { 0, MulDiv(30, dpi, 96), rcClient.right, MulDiv(45, dpi, 96) };
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

struct ColumnLayout {
    int x;
    int width;
    int topItemIndex;
    int bottomItemIndex;
};

void GetMetricDisplay(MetricType type, const HardwareData* pData, DWORD memLoad, bool isPreview,
                      std::wstring& outLabel, std::wstring& outValue, bool& outAlert)
{
    outLabel = g_metricMetadata[type].label;
    outAlert = false;

    HardwareData dummyData = {
        18.0f,  // CpuUsage: 18%
        48.0f,  // CpuTemp: 48 C
        3.20f,  // GpuMemUsedGB: 3.20 GB
        35.0f,  // GpuUsage: 35%
        52.0f,  // GpuTemp: 52 C
        8.50f,  // MemUsedGB: 8.50 GB
        2.50f,  // NetInMBs: 2.50 MB/s
        1.20f   // NetOutMBs: 1.20 MB/s
    };
    DWORD dummyMemLoad = 45;

    const HardwareData* d = (pData && pData->MemUsedGB > 0.0f) ? pData : &dummyData;
    DWORD ml = (pData && pData->MemUsedGB > 0.0f) ? memLoad : dummyMemLoad;

    WCHAR buf[64];
    switch (type) {
    case METRIC_NET_OUT:
        if (d->NetOutMBs < 1.0f)
            swprintf_s(buf, L"%.0f KB/s", d->NetOutMBs * 1024.0f);
        else
            swprintf_s(buf, L"%.2f MB/s", d->NetOutMBs);
        outValue = buf;
        outAlert = (d->NetOutMBs >= 20.0f);
        break;

    case METRIC_NET_IN:
        if (d->NetInMBs < 1.0f)
            swprintf_s(buf, L"%.0f KB/s", d->NetInMBs * 1024.0f);
        else
            swprintf_s(buf, L"%.2f MB/s", d->NetInMBs);
        outValue = buf;
        outAlert = (d->NetInMBs >= 20.0f);
        break;

    case METRIC_CPU_USAGE:
        swprintf_s(buf, L"%d %%", (int)(d->CpuUsage + 0.5f));
        outValue = buf;
        outAlert = (d->CpuUsage >= 70.0f);
        break;

    case METRIC_GPU_USAGE:
        swprintf_s(buf, L"%d %%", (int)(d->GpuUsage + 0.5f));
        outValue = buf;
        outAlert = (d->GpuUsage >= 70.0f);
        break;

    case METRIC_RAM_USED:
        swprintf_s(buf, L"%.2f GB", d->MemUsedGB);
        outValue = buf;
        outAlert = (ml >= 60);
        break;

    case METRIC_GPU_MEM:
        swprintf_s(buf, L"%.2f GB", d->GpuMemUsedGB);
        outValue = buf;
        outAlert = false;
        break;

    case METRIC_CPU_TEMP:
        swprintf_s(buf, L"%d \u00B0C", (int)(d->CpuTemp + 0.5f));
        outValue = buf;
        outAlert = (d->CpuTemp >= 90.0f);
        break;

    case METRIC_GPU_TEMP:
        swprintf_s(buf, L"%d \u00B0C", (int)(d->GpuTemp + 0.5f));
        outValue = buf;
        outAlert = (d->GpuTemp >= 90.0f);
        break;
    }
}

int CalculateColumnsLayout(const MetricItem* items, int count, int startX, int colSpacing, std::vector<ColumnLayout>& outCols, UINT dpi = 96)
{
    outCols.clear();
    int currentX = startX;

    for (int col = 0; col < 4; col++) {
        int topIdx = col * 2;
        int btmIdx = col * 2 + 1;
        bool topEn = items[topIdx].enabled;
        bool btmEn = items[btmIdx].enabled;

        if (!topEn && !btmEn) {
            continue;
        }

        ColumnLayout c;
        c.x = currentX;
        c.topItemIndex = topEn ? topIdx : -1;
        c.bottomItemIndex = btmEn ? btmIdx : -1;

        int wTopRaw = topEn ? g_metricMetadata[items[topIdx].type].defaultWidth : 0;
        int wBtmRaw = btmEn ? g_metricMetadata[items[btmIdx].type].defaultWidth : 0;
        int wTop = MulDiv(wTopRaw, dpi, 96);
        int wBtm = MulDiv(wBtmRaw, dpi, 96);
        c.width = (wTop > wBtm) ? wTop : wBtm;

        outCols.push_back(c);
        currentX += c.width + colSpacing;
    }

    if (outCols.empty()) {
        return 0;
    }
    return (currentX - colSpacing - startX);
}

void RenderMonitoringDisplay(HDC hdc, const RECT& rc, const MetricItem* items, bool isPreview)
{
    HBRUSH hBgBrush = CreateSolidBrush(g_taskbarBgColor);
    FillRect(hdc, &rc, hBgBrush);
    DeleteObject(hBgBrush);

    SetBkMode(hdc, TRANSPARENT);

    int dpi = GetDeviceCaps(hdc, LOGPIXELSY);
    if (dpi <= 0) dpi = 96;
    int fontHeight = -MulDiv(12, dpi, 96);

    HFONT hFont = CreateFontW(fontHeight, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
        ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Malgun Gothic");
    HFONT oldFont = (HFONT)SelectObject(hdc, hFont);

    TEXTMETRICW tm;
    GetTextMetricsW(hdc, &tm);
    int lineH = tm.tmHeight + MulDiv(2, dpi, 96);
    int totalTextH = lineH * 2;
    int yStart = (rc.bottom - rc.top - totalTextH) / 2;
    if (yStart < 1) yStart = 1;

    int r1 = rc.top + yStart;
    int r2 = r1 + lineH;

    bool isLight = IsWindowsLightTheme();
    COLORREF clrText  = isLight ? RGB(20, 20, 20)  : RGB(245, 245, 245);
    COLORREF clrAlert = isLight ? RGB(225, 10, 10) : RGB(255, 50, 50);

    MEMORYSTATUSEX ms = { sizeof(ms) };
    GlobalMemoryStatusEx(&ms);
    DWORD memLoad = ms.dwMemoryLoad;

    std::vector<ColumnLayout> cols;
    int startX = rc.left + MulDiv(10, dpi, 96);
    int colSpacing = MulDiv(20, dpi, 96);
    CalculateColumnsLayout(items, METRIC_COUNT, startX, colSpacing, cols, dpi);

    if (cols.empty()) {
        SetTextColor(hdc, isLight ? RGB(120, 120, 120) : RGB(160, 160, 160));
        RECT rcEmpty = { rc.left + MulDiv(10, dpi, 96), r1, rc.right - MulDiv(10, dpi, 96), r1 + lineH };
        DrawTextW(hdc, L"표시할 모니터링 항목이 없습니다.", -1, &rcEmpty, DT_LEFT | DT_SINGLELINE);
    }
    else if (!isPreview && (!g_pHWData || g_pHWData->MemUsedGB <= 0.0f)) {
        SetTextColor(hdc, isLight ? RGB(100, 100, 100) : RGB(160, 160, 160));
        RECT rcWait = { rc.left + MulDiv(6, dpi, 96), r1, rc.right, r1 + lineH };
        DrawTextW(hdc, L"Waiting for hardware data...", -1, &rcWait, DT_LEFT | DT_SINGLELINE);
    }
    else {
        for (const auto& col : cols) {
            if (col.topItemIndex >= 0) {
                std::wstring lbl, val;
                bool alert = false;
                GetMetricDisplay(items[col.topItemIndex].type, g_pHWData, memLoad, isPreview, lbl, val, alert);

                int yTop = (col.bottomItemIndex >= 0) ? r1 : (rc.top + (rc.bottom - rc.top - lineH) / 2);
                RECT rcCell = { col.x, yTop, col.x + col.width, yTop + lineH };

                SetTextColor(hdc, alert ? clrAlert : clrText);
                DrawTextW(hdc, lbl.c_str(), -1, &rcCell, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
                DrawTextW(hdc, val.c_str(), -1, &rcCell, DT_RIGHT | DT_SINGLELINE | DT_NOPREFIX);
            }

            if (col.bottomItemIndex >= 0) {
                std::wstring lbl, val;
                bool alert = false;
                GetMetricDisplay(items[col.bottomItemIndex].type, g_pHWData, memLoad, isPreview, lbl, val, alert);

                int yBtm = (col.topItemIndex >= 0) ? r2 : (rc.top + (rc.bottom - rc.top - lineH) / 2);
                RECT rcCell = { col.x, yBtm, col.x + col.width, yBtm + lineH };

                SetTextColor(hdc, alert ? clrAlert : clrText);
                DrawTextW(hdc, lbl.c_str(), -1, &rcCell, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
                DrawTextW(hdc, val.c_str(), -1, &rcCell, DT_RIGHT | DT_SINGLELINE | DT_NOPREFIX);
            }
        }
    }

    SelectObject(hdc, oldFont);
    DeleteObject(hFont);
}

LRESULT CALLBACK SysMonitorWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_DPICHANGED:
        UpdateSysMonitorLayout();
        InvalidateRect(hWnd, NULL, TRUE);
        return 0;

    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED:
        UpdateTaskbarColors();
        UpdateSysMonitorLayout();
        InvalidateRect(hWnd, NULL, TRUE);
        return 0;

    case WM_NCHITTEST:
        return HTCLIENT;

    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;

    case WM_MOUSEWHEEL:
    {
        short zDelta = (short)HIWORD(wParam);
        ChangeVolume((int)zDelta);
        return 0;
    }

    case WM_RBUTTONUP:
        ShowTrayMenu(hWnd);
        return 0;

    case WM_CONTEXTMENU:
        return 0;

    case WM_ERASEBKGND:
        return 1; // Prevent background erasing to eliminate flicker

    case WM_TIMER:
        if (wParam == 3) {
            if (g_bMenuOpen) {
                // 메뉴가 열려 있는 동안에는 Z-order 변경 및 레이아웃 재배치 방지
                return 0;
            }

            if (IsFullscreenAppRunning()) {
                if (IsWindowVisible(hWnd)) {
                    ShowWindow(hWnd, SW_HIDE);
                }
                return 0;
            } else {
                if (!IsWindowVisible(hWnd)) {
                    ShowWindow(hWnd, SW_SHOWNOACTIVATE);
                }
            }

            if (!g_pHWData) OpenSharedMemory();
            
            // Check taskbar visibility for auto-hide or resolution changes
            UpdateSysMonitorLayout();
            
            DisableEfficiencyMode();
            
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

        RenderMonitoringDisplay(memDC, rc, g_metricItems, false);

        BitBlt(hdc, 0, 0, rc.right, rc.bottom, memDC, 0, 0, SRCCOPY);

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
            g_hSysMonitorWnd = NULL;
        }
        break;

    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

void LoadSettings()
{
    HKEY hKey;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\VolumeControlHW\\Settings", 0, NULL, 0, KEY_READ, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        DWORD val = 1, sz = sizeof(DWORD);
        if (RegQueryValueExW(hKey, L"DisableEfficiency", NULL, NULL, (LPBYTE)&val, &sz) == ERROR_SUCCESS) g_bDisableEfficiency = (val != 0);
        sz = sizeof(DWORD);
        if (RegQueryValueExW(hKey, L"PreventScreensaver", NULL, NULL, (LPBYTE)&val, &sz) == ERROR_SUCCESS) {
            bool bPrevent = (val != 0);
            if (bPrevent) {
                SetScreensaverBlock(true);
            }
        }

        WCHAR szOrder[128] = { 0 };
        DWORD dwSize = sizeof(szOrder);
        if (RegQueryValueExW(hKey, L"MetricOrderV2", NULL, NULL, (LPBYTE)szOrder, &dwSize) == ERROR_SUCCESS && wcslen(szOrder) > 0) {
            std::wstring s(szOrder);
            size_t start = 0;
            int count = 0;
            while (start < s.length() && count < METRIC_COUNT) {
                size_t comma = s.find(L',', start);
                std::wstring token = (comma == std::wstring::npos) ? s.substr(start) : s.substr(start, comma - start);
                size_t colon = token.find(L':');
                if (colon != std::wstring::npos) {
                    int type = _wtoi(token.substr(0, colon).c_str());
                    int en = _wtoi(token.substr(colon + 1).c_str());
                    if (type >= 0 && type < METRIC_COUNT) {
                        g_metricItems[count].type = (MetricType)type;
                        g_metricItems[count].enabled = (en != 0);
                        count++;
                    }
                }
                if (comma == std::wstring::npos) break;
                start = comma + 1;
            }
        }
        else {
            DWORD oldNet = 1, oldUsage = 1, oldMem = 1, oldTemp = 1;
            sz = sizeof(DWORD);
            if (RegQueryValueExW(hKey, L"ShowNetwork", NULL, NULL, (LPBYTE)&oldNet, &sz) == ERROR_SUCCESS) {
                g_metricItems[0].enabled = (oldNet != 0);
                g_metricItems[1].enabled = (oldNet != 0);
            }
            sz = sizeof(DWORD);
            if (RegQueryValueExW(hKey, L"ShowUsage", NULL, NULL, (LPBYTE)&oldUsage, &sz) == ERROR_SUCCESS) {
                g_metricItems[2].enabled = (oldUsage != 0);
                g_metricItems[3].enabled = (oldUsage != 0);
            }
            sz = sizeof(DWORD);
            if (RegQueryValueExW(hKey, L"ShowMemory", NULL, NULL, (LPBYTE)&oldMem, &sz) == ERROR_SUCCESS) {
                g_metricItems[4].enabled = (oldMem != 0);
                g_metricItems[5].enabled = (oldMem != 0);
            }
            sz = sizeof(DWORD);
            if (RegQueryValueExW(hKey, L"ShowTemp", NULL, NULL, (LPBYTE)&oldTemp, &sz) == ERROR_SUCCESS) {
                g_metricItems[6].enabled = (oldTemp != 0);
                g_metricItems[7].enabled = (oldTemp != 0);
            }
        }
        RegCloseKey(hKey);
    }
}

void SaveSettings()
{
    HKEY hKey;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\VolumeControlHW\\Settings", 0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        DWORD val;
        val = g_bDisableEfficiency ? 1 : 0; RegSetValueExW(hKey, L"DisableEfficiency", 0, REG_DWORD, (LPBYTE)&val, sizeof(DWORD));
        val = g_bPreventScreensaver ? 1 : 0; RegSetValueExW(hKey, L"PreventScreensaver", 0, REG_DWORD, (LPBYTE)&val, sizeof(DWORD));

        std::wstring sOrder;
        for (int i = 0; i < METRIC_COUNT; i++) {
            if (i > 0) sOrder += L",";
            sOrder += std::to_wstring((int)g_metricItems[i].type);
            sOrder += L":";
            sOrder += g_metricItems[i].enabled ? L"1" : L"0";
        }
        RegSetValueExW(hKey, L"MetricOrderV2", 0, REG_SZ, (LPBYTE)sOrder.c_str(), (DWORD)((sOrder.length() + 1) * sizeof(wchar_t)));

        RegCloseKey(hKey);
    }
}

void UpdateSysMonitorLayout()
{
    if (!g_hSysMonitorWnd) return;

    UINT dpi = GetWindowDpi(g_hSysMonitorWnd);
    int startX = MulDiv(10, dpi, 96);
    int colSpacing = MulDiv(20, dpi, 96);

    std::vector<ColumnLayout> cols;
    int contentWidth = CalculateColumnsLayout(g_metricItems, METRIC_COUNT, startX, colSpacing, cols, dpi);
    int newWidth = 0;
    if (contentWidth > 0) {
        newWidth = startX + contentWidth + MulDiv(12, dpi, 96);
    } else {
        newWidth = MulDiv(100, dpi, 96);
    }

    HWND hTaskbar = FindWindowW(L"Shell_TrayWnd", NULL);
    int xPos = 0, yPos = 0;
    int height = MulDiv(40, dpi, 96);

    if (hTaskbar) {
        HWND hCurrentOwner = GetWindow(g_hSysMonitorWnd, GW_OWNER);
        if (hCurrentOwner != hTaskbar) {
            SetWindowLongPtr(g_hSysMonitorWnd, GWLP_HWNDPARENT, (LONG_PTR)hTaskbar);
            SetWindowPos(g_hSysMonitorWnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }

        RECT rcTaskbar;
        GetWindowRect(hTaskbar, &rcTaskbar);
        height = rcTaskbar.bottom - rcTaskbar.top;
        int minH = MulDiv(36, dpi, 96);
        if (height < minH) height = minH;
        
        yPos = rcTaskbar.top;

        xPos = rcTaskbar.right - newWidth - MulDiv(240, dpi, 96);

        HWND hTrayNotify = FindWindowExW(hTaskbar, NULL, L"TrayNotifyWnd", NULL);
        if (hTrayNotify) {
            RECT rcTrayNotify = { 0 };
            GetWindowRect(hTrayNotify, &rcTrayNotify);
            
            if (rcTrayNotify.left > rcTaskbar.left + newWidth) {
                xPos = rcTrayNotify.left - newWidth - MulDiv(8, dpi, 96);
            }
        }
        if (xPos < 0) xPos = 0;
    }
    static int s_lastX = -1, s_lastY = -1, s_lastW = -1, s_lastH = -1;
    if (s_lastX != xPos || s_lastY != yPos || s_lastW != newWidth || s_lastH != height) {
        s_lastX = xPos;
        s_lastY = yPos;
        s_lastW = newWidth;
        s_lastH = height;
        // SWP_NOZORDER를 제거하고 HWND_TOPMOST를 지정하여 작업표시줄 뒤로 숨는 현상 방지
        SetWindowPos(g_hSysMonitorWnd, HWND_TOPMOST, xPos, yPos, newWidth, height, SWP_NOACTIVATE);
        InvalidateRect(g_hSysMonitorWnd, NULL, TRUE);
    }
}

static MetricItem s_dlgMetrics[METRIC_COUNT];

int HitTestSlot(POINT pt, const RECT& rcClient)
{
    int gap = 6;
    int availW = rcClient.right - rcClient.left - 8 - 3 * gap;
    if (availW <= 0) return -1;
    int colW = availW / 4;
    int availH = rcClient.bottom - rcClient.top - 20 - gap;
    if (availH <= 0) return -1;
    int rowH = availH / 2;

    for (int i = 0; i < METRIC_COUNT; i++) {
        int col = i / 2;
        int row = i % 2;
        int x = 4 + col * (colW + gap);
        int y = 16 + row * (rowH + gap);
        RECT rcSlot = { x, y, x + colW, y + rowH };
        if (PtInRect(&rcSlot, pt)) {
            return i;
        }
    }
    return -1;
}

LRESULT CALLBACK MetricGridWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    static int s_iDragSlot = -1;
    static int s_iDropSlot = -1;
    static int s_iHoverSlot = -1;
    static bool s_bDragging = false;
    static POINT s_ptDragStart = { 0, 0 };

    switch (uMsg)
    {
    case WM_ERASEBKGND:
        return 1;

    case WM_MOUSEMOVE:
    {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        RECT rcClient;
        GetClientRect(hWnd, &rcClient);

        if ((wParam & MK_LBUTTON) && s_iDragSlot >= 0) {
            if (!s_bDragging) {
                int dx = abs(pt.x - s_ptDragStart.x);
                int dy = abs(pt.y - s_ptDragStart.y);
                if (dx >= 4 || dy >= 4) {
                    s_bDragging = true;
                    SetCapture(hWnd);
                }
            }
            if (s_bDragging) {
                int target = HitTestSlot(pt, rcClient);
                if (target != s_iDropSlot) {
                    s_iDropSlot = target;
                    InvalidateRect(hWnd, NULL, FALSE);
                }
                SetCursor(LoadCursor(NULL, IDC_SIZEALL));
                return 0;
            }
        }
        else {
            int hover = HitTestSlot(pt, rcClient);
            if (hover != s_iHoverSlot) {
                s_iHoverSlot = hover;
                InvalidateRect(hWnd, NULL, FALSE);
            }
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hWnd, 0 };
            TrackMouseEvent(&tme);
        }
        break;
    }

    case WM_MOUSELEAVE:
    {
        if (s_iHoverSlot >= 0 && !s_bDragging) {
            s_iHoverSlot = -1;
            InvalidateRect(hWnd, NULL, FALSE);
        }
        break;
    }

    case WM_LBUTTONDOWN:
    {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        RECT rcClient;
        GetClientRect(hWnd, &rcClient);
        int hit = HitTestSlot(pt, rcClient);
        if (hit >= 0) {
            s_iDragSlot = hit;
            s_iDropSlot = hit;
            s_ptDragStart = pt;
            s_bDragging = false;
        }
        break;
    }

    case WM_LBUTTONUP:
    {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        RECT rcClient;
        GetClientRect(hWnd, &rcClient);
        HWND hDlg = GetParent(hWnd);

        if (s_bDragging) {
            ReleaseCapture();
            SetCursor(LoadCursor(NULL, IDC_ARROW));
            int target = HitTestSlot(pt, rcClient);
            if (target >= 0 && target != s_iDragSlot) {
                std::swap(s_dlgMetrics[s_iDragSlot], s_dlgMetrics[target]);
            }
            s_bDragging = false;
            s_iDragSlot = -1;
            s_iDropSlot = -1;
            InvalidateRect(hWnd, NULL, FALSE);
            InvalidateRect(GetDlgItem(hDlg, IDC_STATIC_PREVIEW), NULL, FALSE);
            return 0;
        }
        else if (s_iDragSlot >= 0) {
            int hit = HitTestSlot(pt, rcClient);
            if (hit == s_iDragSlot) {
                s_dlgMetrics[hit].enabled = !s_dlgMetrics[hit].enabled;
                InvalidateRect(hWnd, NULL, FALSE);
                InvalidateRect(GetDlgItem(hDlg, IDC_STATIC_PREVIEW), NULL, FALSE);
            }
        }
        s_iDragSlot = -1;
        s_iDropSlot = -1;
        s_bDragging = false;
        break;
    }

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT rc;
        GetClientRect(hWnd, &rc);

        HDC memDC = CreateCompatibleDC(hdc);
        HBITMAP memBmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
        HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, memBmp);

        HBRUSH hBgBrush = (HBRUSH)(COLOR_BTNFACE + 1);
        FillRect(memDC, &rc, hBgBrush);

        SetBkMode(memDC, TRANSPARENT);

        int dpi = GetDeviceCaps(memDC, LOGPIXELSY);
        if (dpi <= 0) dpi = 96;
        int fontH = -MulDiv(12, dpi, 96);
        int fontHSmall = -MulDiv(10, dpi, 96);

        HFONT hFont = CreateFontW(fontH, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
            ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Malgun Gothic");
        HFONT hFontBold = CreateFontW(fontH, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
            ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Malgun Gothic");
        HFONT hFontSmall = CreateFontW(fontHSmall, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
            ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Malgun Gothic");
        HFONT oldFont = (HFONT)SelectObject(memDC, hFont);

        bool isLight = IsWindowsLightTheme();
        COLORREF clrHeader = RGB(130, 130, 130);
        COLORREF clrCardBg = isLight ? RGB(255, 255, 255) : RGB(42, 42, 45);
        COLORREF clrCardHover = isLight ? RGB(235, 242, 255) : RGB(55, 58, 65);
        COLORREF clrCardBorder = isLight ? RGB(215, 215, 220) : RGB(65, 65, 70);
        COLORREF clrCardDropBorder = RGB(0, 120, 215);
        COLORREF clrText = isLight ? RGB(20, 20, 20) : RGB(240, 240, 240);
        COLORREF clrTextDisabled = RGB(150, 150, 150);

        int gap = 6;
        int availW = rc.right - rc.left - 8 - 3 * gap;
        int colW = (availW > 0) ? (availW / 4) : 60;
        int availH = rc.bottom - rc.top - 20 - gap;
        int rowH = (availH > 0) ? (availH / 2) : 30;

        // Column headers
        SelectObject(memDC, hFontSmall);
        SetTextColor(memDC, clrHeader);
        for (int c = 0; c < 4; c++) {
            int x = 4 + c * (colW + gap);
            RECT rcHeader = { x, 1, x + colW, 15 };
            WCHAR szColHeader[16];
            swprintf_s(szColHeader, L"%d열", c + 1);
            DrawTextW(memDC, szColHeader, -1, &rcHeader, DT_CENTER | DT_SINGLELINE);
        }

        // Draw slots
        SelectObject(memDC, hFont);
        for (int i = 0; i < METRIC_COUNT; i++) {
            int col = i / 2;
            int row = i % 2;
            int x = 4 + col * (colW + gap);
            int y = 16 + row * (rowH + gap);
            RECT rcSlot = { x, y, x + colW, y + rowH };

            bool isEnabled = s_dlgMetrics[i].enabled;
            bool isDrop = (s_bDragging && s_iDropSlot == i && s_iDragSlot != i);
            bool isHover = (!s_bDragging && s_iHoverSlot == i);

            HBRUSH hBrushCard = CreateSolidBrush(isHover ? clrCardHover : clrCardBg);
            FillRect(memDC, &rcSlot, hBrushCard);
            DeleteObject(hBrushCard);

            HPEN hPenBorder = CreatePen(PS_SOLID, isDrop ? 2 : 1, isDrop ? clrCardDropBorder : clrCardBorder);
            HPEN oldPen = (HPEN)SelectObject(memDC, hPenBorder);
            HBRUSH oldBrush = (HBRUSH)SelectObject(memDC, GetStockObject(NULL_BRUSH));
            Rectangle(memDC, rcSlot.left, rcSlot.top, rcSlot.right, rcSlot.bottom);
            SelectObject(memDC, oldBrush);
            SelectObject(memDC, oldPen);
            DeleteObject(hPenBorder);

            int cbSize = 13;
            int cbX = x + 6;
            int cbY = y + (rowH - cbSize) / 2;
            RECT rcCheck = { cbX, cbY, cbX + cbSize, cbY + cbSize };

            if (isEnabled) {
                HBRUSH hBrCheck = CreateSolidBrush(RGB(0, 120, 215));
                FillRect(memDC, &rcCheck, hBrCheck);
                DeleteObject(hBrCheck);

                SelectObject(memDC, hFontSmall);
                SetTextColor(memDC, RGB(255, 255, 255));
                DrawTextW(memDC, L"\u2713", -1, &rcCheck, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            } else {
                HBRUSH hBrCheck = CreateSolidBrush(isLight ? RGB(250, 250, 250) : RGB(35, 35, 35));
                FillRect(memDC, &rcCheck, hBrCheck);
                DeleteObject(hBrCheck);
                FrameRect(memDC, &rcCheck, (HBRUSH)GetStockObject(GRAY_BRUSH));
            }

            SelectObject(memDC, isEnabled ? hFontBold : hFont);
            SetTextColor(memDC, isEnabled ? clrText : clrTextDisabled);
            RECT rcText = { cbX + cbSize + 6, y, x + colW - 14, y + rowH };
            DrawTextW(memDC, g_metricMetadata[s_dlgMetrics[i].type].name, -1, &rcText,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

            RECT rcGrip = { x + colW - 12, y, x + colW - 2, y + rowH };
            SelectObject(memDC, hFontSmall);
            SetTextColor(memDC, RGB(170, 170, 170));
            DrawTextW(memDC, L"\u22EE", -1, &rcGrip, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }

        BitBlt(hdc, 0, 0, rc.right, rc.bottom, memDC, 0, 0, SRCCOPY);

        SelectObject(memDC, oldFont);
        DeleteObject(hFont);
        DeleteObject(hFontBold);
        DeleteObject(hFontSmall);
        SelectObject(memDC, oldBmp);
        DeleteObject(memBmp);
        DeleteDC(memDC);

        EndPaint(hWnd, &ps);
    }
    break;

    default:
        return DefWindowProc(hWnd, uMsg, wParam, lParam);
    }
    return 0;
}

INT_PTR CALLBACK SettingsDlgProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_INITDIALOG:
    {
        UpdateTaskbarColors();
        CheckDlgButton(hDlg, IDC_CHK_AUTOSTART, IsAutoStartEnabled() ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, IDC_CHK_SCREENSAVER, g_bPreventScreensaver ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, IDC_CHK_EFFICIENCY, g_bDisableEfficiency ? BST_CHECKED : BST_UNCHECKED);

        memcpy(s_dlgMetrics, g_metricItems, sizeof(s_dlgMetrics));
        InvalidateRect(GetDlgItem(hDlg, IDC_CUSTOM_GRID), NULL, FALSE);
        InvalidateRect(GetDlgItem(hDlg, IDC_STATIC_PREVIEW), NULL, FALSE);
        return (INT_PTR)TRUE;
    }

    case WM_DRAWITEM:
    {
        LPDRAWITEMSTRUCT pDIS = (LPDRAWITEMSTRUCT)lParam;
        if (pDIS->CtlID == IDC_STATIC_PREVIEW) {
            RECT rc = pDIS->rcItem;
            RenderMonitoringDisplay(pDIS->hDC, rc, s_dlgMetrics, true);
            FrameRect(pDIS->hDC, &rc, GetSysColorBrush(COLOR_3DSHADOW));
            return (INT_PTR)TRUE;
        }
        break;
    }

    case WM_COMMAND:
    {
        int wmId = LOWORD(wParam);
        switch (wmId)
        {
        case IDOK:
        {
            bool newAutoStart = (IsDlgButtonChecked(hDlg, IDC_CHK_AUTOSTART) == BST_CHECKED);
            if (newAutoStart != IsAutoStartEnabled()) {
                SetAutoStart(newAutoStart);
            }
            bool newScreensaver = (IsDlgButtonChecked(hDlg, IDC_CHK_SCREENSAVER) == BST_CHECKED);
            if (newScreensaver != g_bPreventScreensaver) {
                SetScreensaverBlock(newScreensaver);
            }

            g_bDisableEfficiency = (IsDlgButtonChecked(hDlg, IDC_CHK_EFFICIENCY) == BST_CHECKED);

            memcpy(g_metricItems, s_dlgMetrics, sizeof(g_metricItems));
            SaveSettings();
            UpdateSysMonitorLayout();
            EndDialog(hDlg, LOWORD(wParam));
            return (INT_PTR)TRUE;
        }

        case IDCANCEL:
            EndDialog(hDlg, LOWORD(wParam));
            return (INT_PTR)TRUE;
        }
        break;
    }
    }
    return (INT_PTR)FALSE;
}

INT_PTR CALLBACK ShutdownTimerDlgProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_INITDIALOG:
        SetDlgItemInt(hDlg, IDC_EDIT_HOURS, 0, FALSE);
        SetDlgItemInt(hDlg, IDC_EDIT_MINUTES, 0, FALSE);
        SetDlgItemInt(hDlg, IDC_EDIT_SECONDS, 0, FALSE);
        return (INT_PTR)TRUE;

    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK)
        {
            BOOL bTranslated;
            int h = GetDlgItemInt(hDlg, IDC_EDIT_HOURS, &bTranslated, FALSE);
            int m = GetDlgItemInt(hDlg, IDC_EDIT_MINUTES, &bTranslated, FALSE);
            int s = GetDlgItemInt(hDlg, IDC_EDIT_SECONDS, &bTranslated, FALSE);
            
            int totalSeconds = h * 3600 + m * 60 + s;
            if (totalSeconds > 0) {
                g_shutdownTimerSecondsLeft = totalSeconds;
                g_bShutdownTimerActive = true;
                g_nShutdownTimerId = SetTimer(g_hWnd, SHUTDOWN_TIMER_ID, 1000, NULL);
                
                swprintf_s(g_nid.szTip, L"Volume Control\n종료까지: %02d:%02d:%02d", h, m, s);
                Shell_NotifyIconW(NIM_MODIFY, &g_nid);
            }
            EndDialog(hDlg, LOWORD(wParam));
            return (INT_PTR)TRUE;
        }
        else if (LOWORD(wParam) == IDCANCEL)
        {
            EndDialog(hDlg, LOWORD(wParam));
            return (INT_PTR)TRUE;
        }
        break;
    }
    return (INT_PTR)FALSE;
}

void CreateSysMonitorWindow(HINSTANCE hInstance)
{
    UpdateTaskbarColors();

    WNDCLASSEXW wcMon = { 0 };
    if (!GetClassInfoExW(hInstance, L"SysMonitorClass", &wcMon)) {
        wcMon.cbSize        = sizeof(WNDCLASSEX);
        wcMon.style         = CS_HREDRAW | CS_VREDRAW;
        wcMon.lpfnWndProc   = SysMonitorWndProc;
        wcMon.hInstance     = hInstance;
        wcMon.hCursor       = LoadCursor(nullptr, IDC_ARROW);
        wcMon.hbrBackground = CreateSolidBrush(g_taskbarBgColor);
        wcMon.lpszClassName = L"SysMonitorClass";
        RegisterClassExW(&wcMon);
    }

    HWND hTaskbar = FindWindowW(L"Shell_TrayWnd", NULL);

    g_hSysMonitorWnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_TOPMOST,
        L"SysMonitorClass",
        L"",
        WS_POPUP | WS_CLIPSIBLINGS,
        0, 0, 100, 40,
        hTaskbar, // Owner is taskbar: keeps it naturally above taskbar
        NULL, hInstance, NULL
    );

    if (g_hSysMonitorWnd) {
        SetLayeredWindowAttributes(g_hSysMonitorWnd, g_taskbarBgColor, 0, LWA_COLORKEY);
        UpdateSysMonitorLayout();
        ShowWindow(g_hSysMonitorWnd, SW_SHOWNOACTIVATE);
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

void DisableEfficiencyMode()
{
    if (!g_bDisableEfficiency) return;

    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE) return;

    PROCESSENTRY32W pe32;
    pe32.dwSize = sizeof(PROCESSENTRY32W);

    if (Process32FirstW(hSnapshot, &pe32)) {
        do {
            if (_wcsicmp(pe32.szExeFile, L"chrome.exe") == 0 ||
                _wcsicmp(pe32.szExeFile, L"msedge.exe") == 0) {
                
                HANDLE hProcess = OpenProcess(PROCESS_SET_INFORMATION, FALSE, pe32.th32ProcessID);
                if (hProcess) {
                    PROCESS_POWER_THROTTLING_STATE PowerThrottling;
                    RtlZeroMemory(&PowerThrottling, sizeof(PowerThrottling));
                    PowerThrottling.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
                    PowerThrottling.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
                    PowerThrottling.StateMask = 0; // Turn off efficiency mode
                    
                    SetProcessInformation(hProcess, ProcessPowerThrottling, &PowerThrottling, sizeof(PowerThrottling));
                    CloseHandle(hProcess);
                }
            }
        } while (Process32NextW(hSnapshot, &pe32));
    }

    CloseHandle(hSnapshot);
}
