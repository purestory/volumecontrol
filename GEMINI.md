# AI 에이전트 개발 행동 강령 및 실수 방지 지침 (Core Rules & Anti-Patterns)

**[경고] 본 문서는 단순 기능 설명서나 커밋 로그가 아닙니다.**  
AI 에이전트(당신)가 개발 중 **실제로 저질렀던 치명적인 실수들을 박제하고 영구히 재발을 방지**하기 위한 강령입니다.  
다른 프로젝트에서도 공통으로 적용되는 **공통 규칙 및 빌드 환경**이 최상단에 배치되어 있으므로, 새로운 작업을 진행하기 전 반드시 숙지하세요.

### 🚨 최우선 원칙 1: 지시 수신 시 구현 전 계획(Plan) 사전 공유 필수 (선 계획, 후 구현)
- **철칙**: 사용자가 기능 추가, 버그 수정, 리팩토링 등 어떤 작업을 지시하든 **임의로 코드를 곧바로 작성하거나 수정하는 구현 작업을 시작하기 전에, 반드시 무엇을 어떻게 진행할 것인지 명확한 실행 계획(Plan)을 먼저 상세히 설명하고 진행**할 것.
- **방지 목적**: 사전 계획 공유 없이 성급하게 코드를 수정하여 엉뚱한 방향으로 구현되거나 불필요한 시행착오와 번복이 발생하는 것을 원천 차단함.

### 🚨 최우선 원칙 2: 실수 발생 즉시 자발적 문서 박제 (사용자 요청 대기 절대 금지)
- **실제 안일한 태도**: 빌드 실패, UI 깨짐, 텍스트 짤림 등의 치명적인 실수를 저지르고 수정 코드를 반영했음에도, "수정 완료되었습니다"라고만 보고하고 사용자가 "GEMINI.md에 추가할 건 없냐?"고 직접 지적할 때까지 가만히 있는 수동적인 태도를 보임.
- **철칙**: 버그 수정이나 실패 수습이 완료되면, **사용자가 묻기 전에 무조건 작업 완료 단계에서 본 문서(GEMINI.md)에 '실제 실수'와 '방지 원칙'을 자동으로 기록**하고 그 사실을 함께 보고할 것. 사용자가 먼저 문서를 챙기게 만드는 것은 에이전트의 심각한 직무유기임.
- **지식 자동 업데이트**: 향후 개발을 진행하면서 특정 프로젝트에 국한되지 않는 **일반적인 Win32 C++ 개발, 성능 최적화, 혹은 UI/UX 개선 패턴**을 새롭게 발견하거나 구현하게 되면, 스스로 판단하여 '개발 규칙' 항목에 해당 내용을 자동으로 요약 및 추가할 것 (단, 너무 지엽적인 예외 사항은 억지로 추가하지 않음).

---

# 1. 모든 C++/Win32 프로젝트 공통 필수 규칙 (Global & Common Rules)

다른 프로젝트에서도 동일하게 준수해야 하는 최우선 공통 규칙들입니다.

### ⚙️ 빌드 및 CLI 명령어 규칙 (Build & Environment)
- **절대 경로 MSBuild 사용**: Windows 환경 변수(PATH)에 `msbuild`나 `cmake`가 등록되어 있지 않은 경우가 많으므로 터미널에서 직접 실행하지 말 것.  
  반드시 Visual Studio 2022 Community MSBuild 절대 경로를 사용:
  `& "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" volumecontrol.sln /p:Configuration=Release /p:Platform=x64`
- **Git 명령어 임의 실행 절대 금지**: 사용자의 명시적인 지시가 없는 한 `git diff`, `git log`, `git checkout`, `git reset` 등 Git 명령어를 절대 임의로 실행하지 말 것.
- **바이너리 파일 잠금(`LNK1104`) 간과 및 미검증 완료 보고 절대 금지**:
  - **실제 실수**: 사용자가 테스트 중 프로그램을 켜둔 상태에서 빌드를 돌리면 `LINK : fatal error LNK1104: '...exe' 파일을 열 수 없습니다`로 링크가 실패함. 이 에러를 대충 넘기고 "수정 완료"라고 보고하여, 사용자가 버그가 전혀 고쳐지지 않은 구버전 바이너리를 보고 극도로 분노하게 만듦.
  - **방지 원칙**: 코드를 수정한 뒤에는 반드시 MSBuild 출력에서 **`빌드했습니다. 오류 0개, 경고 0개`와 `.exe` 생성 성공**을 100% 확인한 후에만 보고할 것. 실행 중인 프로세스로 인한 `LNK1104` 발생 시 프로세스 종료 후 재빌드를 완결할 것.
- **신규 설치 CLI 도구 환경 설정 사용자 떠넘기기 및 세션 PATH 미반영 금지**:
  - **실제 실수**: 도구 설치 후 에이전트가 직접 터미널 환경 변수를 갱신하거나 절대 경로로 후속 작업을 자동화할 수 있는 권한(`run_command`)이 있음에도 불구하고, 사용자에게 수동 작업을 떠넘기거나 환경 변수 상속 오류를 유발함.
  - **방지 원칙**:
    1. 도구 설치 후 필요한 환경 변수 갱신, 바이너리 검증, 후속 작업은 **에이전트가 직접 실행(`run_command`)하여 사용자가 곧바로 쓸 수 있는 상태로 완결**해 둘 것.
    2. 사용자에게 터미널 명령어를 안내할 때는 IDE 재시작 전까지 환경 변수가 상속되지 않는 문제를 감안하여 **무조건 검증된 절대 경로**를 기본으로 제공할 것.
- **배포 산출물 중복 생성 및 패치 수정 시 버전(Version) 미갱신 금지**:
  - 배포 아카이브는 중복 파일 없이 **단일 표준 버전 명칭(예: `VolumeControl_v1.0.1_Portable.zip`) 딱 1개만** 정갈하게 생성할 것.
  - 코드/리소스 수정 및 배포가 발생하면 반드시 **시맨틱 버저닝(SemVer) 규칙에 따라 버전 번호를 명확히 상향(`v1.0.0` ➡️ `v1.0.1`)**하여 릴리스할 것.
- **설치 프로그램(Setup.exe) 생성 시 외부 도구 탐색 낭비 금지 및 EasyInstall 표준 파이프라인 준수**:
  - **실제 실수**: 설치 프로그램(Setup.exe) 생성 지시를 받았을 때, 프로젝트 환경에 구축된 자체 빌더를 확인하지 않고 Inno Setup(`iscc`), NSIS(`makensis`) 등 엉뚱한 외부 도구를 찾으며 헤매고 시간을 지체함. 동일 워크스페이스 그룹(`D:\coding\visualstudio\easyinstall`)에 전용 인스톨러 빌더가 이미 완성되어 있음에도 이를 즉시 활용하지 못함.
  - **방지 원칙 (표준 배포 패키징 파이프라인)**:
    1. **단일 설치 파일 빌더 절대 경로**:  
       `& "D:\coding\visualstudio\easyinstall\bin\Release\easyinstall.exe" -i "<입력_바이너리_또는_폴더>" -o "<출력경로>\Setup.exe" -n "<프로그램명>" -v "<버전>" -p "purestory" -m "<메인_exe명>"`  
       *(예: `& "D:\coding\visualstudio\easyinstall\bin\Release\easyinstall.exe" -i "D:\coding\visualstudio\clearMax\build\Release\clearMax.exe" -o "D:\coding\visualstudio\clearMax\ClearMax_Setup.exe" -n "clearMax" -v "1.14" -p "purestory" -m "clearMax.exe"`)*
    2. **무설치 포터블 압축 아카이브 생성**:  
       `powershell -Command "Compress-Archive -Path '<바이너리_경로>' -DestinationPath '<출력경로>\<프로그램명>_portable.zip' -Force"`
    3. **GitHub Release 자동 배포 규격**:  
       `gh release create <버전태그> <Setup.exe> <portable.zip> -F <릴리스노트.md> -t "<버전태그>"`
    4. 설치 파일 생성 요구 시 타사 도구(`iscc`, `makensis` 등)를 일절 찾지 말고, 위 `easyinstall.exe` 절대 경로를 이용하여 즉시 1초 만에 원스톱으로 빌드할 것.


### 🛠️ 도구 사용 실수 방지: `replace_file_content` 줄 번호 포함 금지
- **실제 실수**: `view_file`로 확인한 내용 앞의 줄 번호(`136: `, `150: ` 등)를 복사해 `TargetContent`나 `ReplacementContent`에 전달했다가 `chunk 0: target content not found` 에러를 반복 유발함.
- **방지 원칙**: 파일 수정 도구를 호출할 때는 파일의 **순수한 실제 코드(공백, 들여쓰기 포함, 줄 번호 절대 제외)**만을 정확하게 지정할 것.

### 🖥️ UI 컨트롤 라이프사이클 4단계 및 너비·줄바꿈·동적 리사이징 원칙 ('반쪽짜리 구현' 금지)
- **동적 레이아웃 및 리사이징 (WM_SIZE 처리)**:
  - `.rc` 파일에 정의된 컨트롤 크기 및 위치는 픽셀이 아닌 **DLU(Dialog Units)** 기반이므로, 시스템 DPI나 시스템 폰트에 따라 렌더링되는 크기가 달라집니다.
  - 따라서 `WM_SIZE` 핸들러에서 하드코딩된 픽셀 값(예: `width - 90`)을 사용한 위치/크기 계산을 피해야 합니다.
  - 항상 `GetWindowRect`로 컨트롤의 실제 렌더링 크기(width/height)를 구하거나, `MapDialogRect`를 통해 DLU를 픽셀로 변환한 후, 부모 클라이언트 영역(`GetClientRect`)에 비례하여 동적으로 배치(우측/하단 정렬 등)하세요.
- **UI 구현 4단계 완결**: 리소스 선언(`.rc`) ➡️ 컨트롤 초기화(`WM_INITDIALOG`) ➡️ 조작/렌더링(`WM_COMMAND`, `WM_DRAWITEM` 등) ➡️ 반응형 재배치(`WM_SIZE`).
- **1줄 라벨 줄바꿈 방지**: 1줄 라벨에는 반드시 `SS_LEFTNOWORDWRAP`을 사용하고, 한글 기준 글자당 최소 14~15px 이상의 넉넉한 너비를 할당할 것.
- **체크박스/라디오 너비 계산**: `텍스트 글자 폭 + 체크마크(24px) + 안전 마진(20~30%)`을 무조건 합산하여 할당할 것.
- **최소 창 크기 방어**: 반응형 대화상자는 `WM_GETMINMAXINFO`를 필수로 구현하여 사용자가 창을 줄이더라도 컨트롤이 겹치거나 깨지지 않도록 최소 너비/높이를 제한할 것.

### 🎨 깜빡임 없는 UI 업데이트 (In-place Update & Notification Suppression)
- 리스트뷰(`ListView`) 등의 아이템 상태가 일부 변경되었을 때, 전체 리스트를 지우고 새로고침(`DeleteAllItems` 후 재로드)하지 말 것.
- 화면 깜빡임과 스크롤 초기화를 방지하기 위해 `ListView_SetItemText` 등을 사용하여 **변경된 특정 행(Row)만 부분 업데이트(In-place Update)** 해야 함.
- 대량의 아이템 갱신 시 불리언 가드(`g_bUpdating = true`)와 `WM_SETREDRAW`를 걸어 `LVN_ITEMCHANGED` 연쇄 이벤트 폭주 및 화면 깜빡임을 원천 차단할 것.

### 🖌️ Win32 Dialog 내 Custom Draw 처리 (`DWLP_MSGRESULT`)
- DialogBox 기반의 윈도우(DialogProc)에서 `WM_NOTIFY` 메시지의 `NM_CUSTOMDRAW`를 처리할 때, 직접 return 값을 반환하면 시스템이 인식하지 못하고 무시하는 경우가 발생함.
- 반드시 `SetWindowLongPtr(hWnd, DWLP_MSGRESULT, 반환값)`을 호출하여 결과를 명시한 후, `return (INT_PTR)TRUE;`를 반환해야 정상적으로 렌더링을 가로채거나(`CDRF_NOTIFYPOSTPAINT`) 넘길 수 있음.
- 기존 컨트롤의 아이콘이나 체크박스 등을 보존하면서 텍스트만 다르게 그리고 싶다면, `CDRF_SKIPDEFAULT` 대신 `CDRF_NOTIFYPOSTPAINT`를 사용하여 기본 렌더링이 끝난 후 `CDDS_ITEMPOSTPAINT` 단계에서 덮어쓰는 방식을 사용해야 함.

### 🌲 TreeView 폴더 확장 시 Auto-Scroll(화면 튐) 방지
- `SysTreeView32` 컨트롤에서 사용자가 폴더(+)를 확장하여 다수의 자식 노드를 삽입(`TVN_ITEMEXPANDING`)할 때, 컨트롤이 자식들을 화면에 욱여넣으려고 자동 스크롤하면서 부모 노드가 화면 최상단으로 튕겨 올라가는 불쾌한 UX가 발생함.
- 이를 방지하기 위해 노드 삽입 직전 `GetScrollPos(hTree, SB_VERT)`로 스크롤 위치를 기억하고, `PostMessage` 등을 이용해 비동기로(즉시) 해당 스크롤 위치를 복구(`WM_VSCROLL`)하여 화면이 그 상태 그대로 유지되도록 처리해야 함.

### 🔤 C++ 트레일링 역슬래시(`\`) 줄바꿈(Line-Splicing) 버그 방지
- C/C++ 소스 및 헤더의 한 줄 주석(`//`) 끝에는 절대로 단독 역슬래시(`\`)를 두지 말고, `\\` 또는 `(예: C:\\)` 등으로 처리할 것. 전처리기가 다음 줄을 주석으로 삼키는 참사 방지.

### ⌨️ 사용자 입력 시나리오 검증 및 키보드 편의성 보장
- 표준 Edit 컨트롤을 사용하고 유연한 파서를 작성할 것. 마우스 클릭 또는 포커스 진입 시 `EM_SETSEL(0, -1)`을 보내 즉시 전체 선택 수정 모드를 제공하고, Enter 키 입력 시 대화상자가 닫히지 않도록 서브클래싱(`DLGC_WANTALLKEYS`)할 것.

### 🔒 멀티스레드 동기화 뮤텍스(Lock) 내부 동기식 디스크 I/O 배제
- 워커 스레드 동기화 임계 영역(Mutex Lock) 내부에서는 디스크/네트워크 시스템 콜을 일절 배제하고 **순수 RAM 메모리 해시셋/맵 조회 및 삽입(0.0001ms)**만 수행할 것. 이미 확인된 경로는 RAM 해시셋에 캐싱하여 중복 디스크 I/O를 0ms로 통과시킬 것.

### 🧵 백그라운드 워커 스레드의 COM 다중 스레드 아파트먼트 초기화
- Shell Property Store, WIC, IShellItem, WASAPI 등을 워커 스레드에서 사용할 때는 시작 시 `CoInitializeEx(NULL, COINIT_MULTITHREADED)`를, 종료 시 `CoUninitialize()`를 반드시 쌍으로 호출할 것.

### 🗑️ 데이터 유실 방지를 위한 파일 안전 삭제 원칙
- 파일 삭제 시 `SHFileOperationW` + `FOF_ALLOWUNDO`로 휴지통 복구 여지를 남길 것.

### 📜 PowerShell 텍스트 파일 조작 시 UTF-8 인코딩 강제
- Windows PowerShell에서 `Add-Content`나 `Out-File` 등의 명령어로 텍스트 파일에 내용을 덧붙이거나 작성할 때, 기본 인코딩(ANSI 등)이 적용되어 기존 UTF-8(특히 한글)이 깨지는 현상이 발생함.
- 따라서 파일 I/O를 수행하는 셸 스크립트를 작성할 때는 반드시 `-Encoding UTF8` 파라미터를 명시하여 인코딩 깨짐을 원천 차단할 것.

---

# 2. 오디오 제어 및 volumecontrol 프로젝트 특화 규칙 (Project-Specific Traps)

본 프로젝트(볼륨 제어, Core Audio WASAPI, 투명 OSD 위젯, 드래그 앤 드롭 그리드) 도메인 특성상 반드시 준수해야 하는 규칙입니다.

### 🔍 데이터 수집 시 레지스트리 유연한 파싱 (Fallback 구현)
- Windows 레지스트리를 통해 프로그램 정보 등을 조회할 때, `InstallLocation` 같은 표준 키는 비어있거나 누락된 경우가 매우 많음.
- 항상 특정 키 값이 비어있을 것을 가정하고, `UninstallString`이나 `DisplayIcon` 같은 다른 문자열에서 `.exe` 경로와 디렉토리를 역추적해내는 **Fallback(예비) 로직**을 반드시 구현하여 데이터 수집의 정확도와 안정성을 높일 것.

### 🎨 투명 윈도우(`LWA_COLORKEY`)에서의 폰트 렌더링 최적화
- `LWA_COLORKEY`를 사용하여 배경을 투명하게 만드는 레이어드 윈도우(Layered Window)에 텍스트를 출력할 때, `CreateFont`의 파라미터로 `CLEARTYPE_QUALITY`를 사용하면 안 됨.
- ClearType의 서브픽셀 렌더링이 배경색과 혼합되면서 투명화 처리(Color Key)가 제대로 되지 않아 글씨 외곽이 번지거나 두껍게 나오는 현상(Color Fringing)이 발생함.
- 이 경우 반드시 `ANTIALIASED_QUALITY`를 사용하여 그레이스케일 안티앨리어싱을 적용함으로써 텍스트 렌더링 품질을 유지하고 투명화가 깔끔하게 되도록 해야 함.

### 🔊 Core Audio (WASAPI) 기본 출력 장치 변경 실시간 감지
- Windows에서 사용자가 기본 오디오 출력 장치를 변경(블루투스 이어폰 연결, 헤드셋 전환, 작업표시줄 사운드 출력 전환 등)하더라도, 기존에 획득한 `IAudioEndpointVolume` 인터페이스 포인터는 여전히 유효한 COM 객체로 남아 있어 함수 호출(예: `GetMasterVolumeLevelScalar`)이 정상(`S_OK`) 반환됨.
- 이 때문에 단순히 오류 발생 여부만 체크해서는 장치 변경을 알아채지 못하고 이전 장치의 볼륨만 계속 조절하는 문제가 발생함.
- 이를 해결하기 위해 `IMMNotificationClient`를 구현하여 `RegisterEndpointNotificationCallback`을 통해 `OnDefaultDeviceChanged` 콜백을 등록해야 함.
- 콜백은 백그라운드 RPC/MTA 스레드에서 호출되므로, `PostMessage`나 원자적 플래그(`InterlockedExchange`)를 통해 메인 UI(STA) 스레드로 안전하게 전달한 뒤 새 기본 엔드포인트(`IMMDevice::Activate`)로 볼륨 인터페이스를 재초기화(Re-initialize)해야 함.
- 또한 주기적 확인 루틴(`EnsureAudio`)에서 현재 기본 장치의 고유 ID(`IMMDevice::GetId`)를 저장된 장치 ID와 비교하는 Fallback 로직을 함께 구현하면 이벤트 누락 시에도 안전하게 복구됨.

### 🖱️ 커스텀 컨트롤을 활용한 2차원 직관적 드래그 앤 드롭 UI 구현
- 표준 Win32 리스트뷰(`SysListView32`)는 2열 이상의 그리드 형태나 실제 출력 영역을 직관적으로 미러링하는 배치 변경 및 실시간 미리보기를 제공하기에 시각적 제약과 깜빡임 문제가 있음.
- 실제 출력 형태(예: 4열 x 2행 위젯 등)와 동일한 배치를 갖는 커스텀 윈도우 클래스를 등록하고 더블 버퍼링(`CreateCompatibleDC` / `BitBlt`) 기반의 `WM_PAINT`와 드래그 앤 드롭 상태 머신(`WM_LBUTTONDOWN`, 이동 임계값 4px 체크 후 `SetCapture`, 드롭 타깃 피드백, `WM_LBUTTONUP`에서 `std::swap`)을 구현하면 외부 라이브러리 없이도 반응성이 뛰어나고 직관적인 사용자 경험(UX)을 완성할 수 있음.

### 🖥️ 고DPI 및 다중 모니터 환경에서의 폰트/창 비트맵 스트레칭 방지 (Per-Monitor V2 DPI & MulDiv 스케일링)
- 구형 API인 `SetProcessDPIAware()`는 프로세스를 단일 시스템 DPI에 고정시킴. 이로 인해 다중 모니터(배율 불일치), 절전 모드(Sleep) 복귀, 화면 해상도/배율 변경 시 Windows DWM이 창을 강제로 비트맵 확대/축소(Bitmap Stretching)하여 폰트가 저해상도 이미지처럼 뭉개지고 블러(Blur) 처리되는 현상이 간헐적으로 발생함.
- 반드시 런타임에 `SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)`를 최우선 적용(실패 시 `SetProcessDpiAwareness` -> `SetProcessDPIAware` Fallback)하여 DWM 가상화 확대를 원천 차단해야 함.
- 또한 모든 폰트 높이(`CreateFontW`) 및 윈도우 배치 좌표/패딩은 `-MulDiv(기본픽셀, dpi, 96)`로 현재 모니터의 실제 DPI(`GetDpiForWindow`)에 비례하여 동적으로 계산하고, `WM_DPICHANGED`와 `WM_SETTINGCHANGE` / `WM_THEMECHANGED` 메시지를 핸들링하여 배율 및 테마 변화 시 레이아웃과 폰트를 즉시 갱신해야 함.

### 🪟 레이어드 윈도우(`WS_EX_LAYERED`) 마우스 클릭 투과 방지 및 팝업 메뉴 Z-Order/배제 영역 보장
- **레이어드 투명 영역 클릭 투과 버그 및 DWM 우회 한계**:
  - `LWA_COLORKEY`를 사용하는 레이어드 윈도우는 Windows DWM에 의해 컬러키 색상(투명 배경) 영역의 마우스 이벤트가 창(`SysMonitorWndProc`)으로 전달되지 않고 뒷 창(`Shell_TrayWnd`)으로 강제 통과(Bypass)됨.
  - 단순 `WM_NCHITTEST`에서 `HTCLIENT`를 반환하는 것만으로는 DWM의 투명 픽셀 마우스 바이패스를 막을 수 없어, 텍스트가 아닌 여백을 클릭하면 마우스 우클릭 메시지가 아예 들어오지 않는 버그가 발생함.
  - **해결 원칙**: 상시 동작하는 전역 저수준 마우스 훅(`WH_MOUSE_LL`, `LowLevelMouseProc`)에서 모니터링 창의 화면 사각형(`PtInRect(&rcMon, pt)`)을 검사하여, 모니터링 창 영역 내에서의 마우스 우클릭(`WM_RBUTTONDOWN`, `WM_RBUTTONUP`)을 선제적으로 가로채어(`return 1`) 작업표시줄로의 누수를 차단하고 메인 루프에 비동기 메시지(`WM_APP_SHOW_MONITOR_MENU`)를 전달하여 메뉴를 표출해야 함.
- **주기적 타이머에 의한 팝업 메뉴 가림 및 Z-Order 역전 방지**:
  - `UpdateSysMonitorLayout` 등에서 매초 `SetWindowPos(..., HWND_TOPMOST)`를 무조건 호출하면, 사용자가 띄운 팝업 메뉴(`#32768`) 위로 모니터링 창이 올라와 메뉴를 가려버리는 치명적인 버그가 발생함.
  - 메뉴가 열려 있는 동안 활성화되는 불리언 가드(`g_bMenuOpen = true`)를 적용하여 메뉴가 떠 있는 동안에는 타이머에 의한 Z-order 재배치 및 무효화를 스킵해야 함.
  - 또한 이전 좌표/크기와 비교하여 실제로 위치나 크기가 변경되었을 때만 `SetWindowPos`를 호출하도록 최적화해야 함.
- **메뉴 배제 영역 지정 (`TrackPopupMenuEx` + `TPMPARAMS.rcExclude`) 및 커서 좌표 충돌 방지**:
  - 팝업 메뉴 생성 시 `TrackPopupMenuEx`의 `TPMPARAMS.rcExclude`에 모니터링 창 RECT를 지정하면 메뉴가 모니터링 화면을 가리지 않음.
  - **주의**: 모니터링 창 내부를 클릭한 경우 마우스 좌표 `pt` 자체가 `rcExclude` 안에 위치하게 되므로, `TPM_BOTTOMALIGN`과 함께 쓰이면 Windows 내부 배제 배치 알고리즘과 충돌하여 메뉴가 즉시 닫히거나 표시되지 않는 문제가 발생함.
  - 반드시 모니터링 창 내부 클릭 시 `pt.y`를 모니터링 창 상단(`rcExclude.top`, 상단 작업표시줄은 `rcExclude.bottom`)으로 보정하여 메뉴 바닥이 창 외곽에 밀착되어 정갈하게 펼쳐지도록 좌표를 보정해야 함.
  - 또한 메뉴를 띄울 때 숨은 창이 아닌 실제로 눈에 보이는 최상위 윈도우(`g_hSysMonitorWnd`)를 Owner 및 `SetForegroundWindow` 대상으로 넘겨야 포그라운드 활성화가 실패하지 않고, 메뉴 윈도우 역시 최상위(`TOPMOST`) 계층으로 올바르게 생성됨.

