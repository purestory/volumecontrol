# Agent Instructions (AI 행동 지침)

**항상 다음 지침을 준수하여 사용자를 지원하세요.**
1. **지식 업데이트:** 향후 개발을 진행하면서 특정 프로젝트에 국한되지 않는 **일반적인 Win32 C++ 개발, 성능 최적화, 혹은 UI/UX 개선 패턴**을 새롭게 발견하거나 구현하게 되면, AI는 스스로 판단하여 **이 문서(GEMINI.md)의 '개발 규칙' 항목에 해당 내용을 자동으로 요약 및 추가**해야 합니다.
2. 단, 너무 지엽적이거나 특정 상황에만 해당하는 특수한 예외 사항은 억지로 추가하지 않습니다.

---

# 빌드 규칙 (Build Rules)

환경 변수에 `cmake`와 `msbuild`가 등록되어 있지 않은 Windows 환경에서 C++ 프로젝트를 빌드할 때는 다음 규칙을 따릅니다.

1. PowerShell에서 `cmake --build`나 `msbuild` 명령어를 직접 실행하지 않습니다.
2. 빌드 시 반드시 **Visual Studio 2022 Community의 MSBuild 절대 경로**를 사용해야 합니다.
3. `&` 연산자를 사용하여 다음과 같이 솔루션을 빌드합니다. (경로는 현재 프로젝트에 맞게 수정)
   `& "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" build\<SolutionName>.sln /p:Configuration=Release`

---

# 개발 규칙 (Development Rules)

Win32 C++ 프로젝트 개발 시 소프트웨어의 완성도를 높이기 위해 다음 원칙을 항상 준수하세요.

1. **동적 레이아웃 및 리사이징 (WM_SIZE 처리)**
   - `.rc` 파일에 정의된 컨트롤 크기 및 위치는 픽셀이 아닌 **DLU(Dialog Units)** 기반이므로, 시스템 DPI나 시스템 폰트에 따라 렌더링되는 크기가 달라집니다.
   - 따라서 `WM_SIZE` 핸들러에서 하드코딩된 픽셀 값(예: `width - 90`)을 사용한 위치/크기 계산을 피해야 합니다.
   - 항상 `GetWindowRect`로 컨트롤의 실제 렌더링 크기(width/height)를 구하거나, `MapDialogRect`를 통해 DLU를 픽셀로 변환한 후, 부모 클라이언트 영역(`GetClientRect`)에 비례하여 동적으로 배치(우측/하단 정렬 등)하세요.

2. **깜빡임 없는 UI 업데이트 (In-place Update)**
   - 리스트뷰(`ListView`) 등의 아이템 상태가 일부 변경되었을 때, 전체 리스트를 지우고 새로고침(`DeleteAllItems` 후 재로드)하지 마세요.
   - 화면 깜빡임과 스크롤 초기화를 방지하기 위해 `ListView_SetItemText` 등을 사용하여 **변경된 특정 행(Row)만 부분 업데이트(In-place Update)** 해야 합니다.

3. **데이터 수집 시 레지스트리 유연한 파싱 (Fallback 구현)**
   - Windows 레지스트리를 통해 프로그램 정보 등을 조회할 때, `InstallLocation` 같은 표준 키는 비어있거나 누락된 경우가 매우 많습니다.
   - 항상 특정 키 값이 비어있을 것을 가정하고, `UninstallString`이나 `DisplayIcon` 같은 다른 문자열에서 `.exe` 경로와 디렉토리를 역추적해내는 **Fallback(예비) 로직**을 반드시 구현하여 데이터 수집의 정확도와 안정성을 높이세요.

4. **투명 윈도우 (LWA_COLORKEY)에서의 폰트 렌더링 최적화**
   - `LWA_COLORKEY`를 사용하여 배경을 투명하게 만드는 레이어드 윈도우(Layered Window)에 텍스트를 출력할 때, `CreateFont`의 파라미터로 `CLEARTYPE_QUALITY`를 사용하면 안 됩니다.
   - ClearType의 서브픽셀 렌더링이 배경색과 혼합되면서 투명화 처리(Color Key)가 제대로 되지 않아 글씨 외곽이 번지거나 두껍게 나오는 현상(Color Fringing)이 발생합니다.
   - 이 경우 반드시 `ANTIALIASED_QUALITY`를 사용하여 그레이스케일 안티앨리어싱을 적용함으로써 텍스트 렌더링 품질을 유지하고 투명화가 깔끔하게 되도록 해야 합니다.

5. **Core Audio (WASAPI) 기본 출력 장치 변경 실시간 감지**
   - Windows에서 사용자가 기본 오디오 출력 장치를 변경(블루투스 이어폰 연결, 헤드셋 전환, 작업표시줄 사운드 출력 전환 등)하더라도, 기존에 획득한 `IAudioEndpointVolume` 인터페이스 포인터는 여전히 유효한 COM 객체로 남아 있어 함수 호출(예: `GetMasterVolumeLevelScalar`)이 정상(`S_OK`) 반환됩니다.
   - 이 때문에 단순히 오류 발생 여부만 체크해서는 장치 변경을 알아채지 못하고 이전 장치의 볼륨만 계속 조절하는 문제가 발생합니다.
   - 이를 해결하기 위해 `IMMNotificationClient`를 구현하여 `RegisterEndpointNotificationCallback`을 통해 `OnDefaultDeviceChanged` 콜백을 등록해야 합니다.
   - 콜백은 백그라운드 RPC/MTA 스레드에서 호출되므로, `PostMessage`나 원자적 플래그(`InterlockedExchange`)를 통해 메인 UI(STA) 스레드로 안전하게 전달한 뒤 새 기본 엔드포인트(`IMMDevice::Activate`)로 볼륨 인터페이스를 재초기화(Re-initialize)해야 합니다.
   - 또한 주기적 확인 루틴(`EnsureAudio`)에서 현재 기본 장치의 고유 ID(`IMMDevice::GetId`)를 저장된 장치 ID와 비교하는 Fallback 로직을 함께 구현하면 이벤트 누락 시에도 안전하게 복구됩니다.

6. **커스텀 컨트롤을 활용한 2차원 직관적 드래그 앤 드롭 UI 구현**
   - 표준 Win32 리스트뷰(`SysListView32`)는 2열 이상의 그리드 형태나 실제 출력 영역을 직관적으로 미러링하는 배치 변경 및 실시간 미리보기를 제공하기에 시각적 제약과 깜빡임 문제가 있습니다.
   - 실제 출력 형태(예: 4열 x 2행 위젯 등)와 동일한 배치를 갖는 커스텀 윈도우 클래스를 등록하고 더블 버퍼링(`CreateCompatibleDC` / `BitBlt`) 기반의 `WM_PAINT`와 드래그 앤 드롭 상태 머신(`WM_LBUTTONDOWN`, 이동 임계값 4px 체크 후 `SetCapture`, 드롭 타깃 피드백, `WM_LBUTTONUP`에서 `std::swap`)을 구현하면 외부 라이브러리 없이도 반응성이 뛰어나고 직관적인 사용자 경험(UX)을 완성할 수 있습니다.

