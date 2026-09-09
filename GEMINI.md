# 빌드 규칙 (Build Rules)

이 프로젝트(volumecontrol)를 빌드할 때 환경 변수에 `cmake`와 `msbuild`가 등록되어 있지 않으므로 다음 규칙을 엄격히 따르세요.

1. `cmake --build`나 `msbuild` 명령어를 직접 실행하지 마세요.
2. 빌드할 때는 반드시 Visual Studio 2022 Community의 MSBuild 절대 경로를 사용해야 합니다.
3. PowerShell 환경이므로 반드시 `&` 연산자를 사용하여 다음과 같이 실행하세요:
   `& "C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\MSBuild\\Current\\Bin\\MSBuild.exe" volumecontrol.sln /p:Configuration=Release`

# 리소스 파일(RC) 인코딩 규칙

1. `.rc` 파일에 한글(UTF-8) 문자열을 포함할 때는 파일 최상단에 반드시 `#pragma code_page(65001)` 지시어를 추가해야 합니다.
2. 이 지시어가 없으면 리소스 컴파일러(rc.exe)가 UTF-8 문자열을 시스템 기본 인코딩으로 잘못 해석하여 다이얼로그나 팝업 메뉴 등 UI에서 한글이 깨지는 문제(Mojibake)가 발생합니다.
