# 빌드 규칙 (Build Rules)

이 프로젝트(volumecontrol)를 빌드할 때 환경 변수에 `cmake`와 `msbuild`가 등록되어 있지 않으므로 다음 규칙을 엄격히 따르세요.

1. `cmake --build`나 `msbuild` 명령어를 직접 실행하지 마세요.
2. 빌드할 때는 반드시 Visual Studio 2022 Community의 MSBuild 절대 경로를 사용해야 합니다.
3. PowerShell 환경이므로 반드시 `&` 연산자를 사용하여 다음과 같이 실행하세요:
   `& "C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\MSBuild\\Current\\Bin\\MSBuild.exe" volumecontrol.sln /p:Configuration=Release`
