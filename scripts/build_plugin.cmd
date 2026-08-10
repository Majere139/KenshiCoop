@echo off
REM Build KenshiCoop.dll with the legacy v100 (VC++ 2010) x64 toolchain on a
REM machine that has only "Windows SDK 7.1 + VC2010 SP1 compiler update" (no full
REM VS2010). We hand MSBuild a complete PATH/INCLUDE/LIB and UseEnv=true so it does
REM not rely on VS2010 registry/SDK auto-detection.
REM
REM Prereqs (see resources/BUILD_SETUP.md):
REM   - VC++ 2010 (v100) x64 compiler  (SDK 7.1 + KB2519277)
REM   - VS2022 Build Tools (for MSBuild.exe)
REM   - third_party/KenshiLib_deps (deps + Boost) and env vars set
REM   - third_party/enet/enet patched for C89 (scripts/apply_enet_patch is implicit;
REM     see third_party/enet/patches/0001-enet-c89-for-loops.patch)
setlocal

REM Build configuration (Phase 1 build separation). Default = Harness, the
REM optimized TEST build that INCLUDES the scenario runner - this is what the
REM regression/manual pipeline needs. Pass "Release" to produce the shipped
REM player DLL (no scenario code); "Debug" for a dev build.
REM   Usage:  scripts\build_plugin.cmd [Harness|Release|Debug]
set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=Harness"

REM Repo root = parent of this script's folder.
set "REPO=%~dp0.."
pushd "%REPO%" >nul
set "REPO=%CD%"
popd >nul

set "VS10=C:\Program Files (x86)\Microsoft Visual Studio 10.0"
set "VC=%VS10%\VC"
set "SDK=C:\Program Files\Microsoft SDKs\Windows\v7.1"
set "KL=%REPO%\third_party\KenshiLib_deps"
set "ENET=%REPO%\third_party\enet\enet\include"

REM Locate MSBuild. A pre-set %MSBUILD% wins, then vswhere, then a known path.
REM "-products *" matters: the default product filter skips Community/2026-era
REM installs, and "-requires Microsoft.Component.MSBuild" matches nothing on
REM them either, so the narrower query returned empty and fell through to a
REM VS2022 BuildTools path that need not exist on this machine.
REM The ^"...^" wrapping is required, not decoration: for /f "usebackq" strips the
REM quotes around an exe path that itself contains spaces as soon as the command
REM line carries further quoted arguments, so the unwrapped form ran
REM 'C:\Program' and silently yielded nothing.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not defined MSBUILD (
  for /f "usebackq delims=" %%i in (`^""%VSWHERE%" -latest -products * -find "MSBuild\**\Bin\MSBuild.exe"^"`) do set "MSBUILD=%%i"
)
if not defined MSBUILD set "MSBUILD=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe"
if not exist "%MSBUILD%" (
  echo ERROR: MSBuild.exe not found.
  echo        Set MSBUILD to its full path and re-run, e.g.
  echo          set "MSBUILD=C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe"
  exit /b 1
)

REM x64 native toolchain on PATH so cl.exe finds its sibling DLLs (mspdb100, etc).
set "PATH=%VC%\bin\amd64;%VC%\bin;%VS10%\Common7\IDE;%SDK%\Bin\x64;%SDK%\Bin;%PATH%"

REM Headers: VC10 CRT + Win SDK 7.1 + vc10_compat ammintrin.h shim + our deps.
REM ...\Include\ogre is needed because the vendored ogre math headers include
REM each other by bare name ("OgreVector3.h"); vc10_compat also shims the
REM missing OgreConfig.h/OgrePlatformInformation.h that chain pulls in.
set "INCLUDE=%VC%\include;%SDK%\Include;%REPO%\third_party\vc10_compat;%KL%\KenshiLib\Include;%KL%\KenshiLib\Include\ogre;%KL%\boost_1_60_0;%ENET%"

REM Libs: VC10 x64 CRT + Win SDK 7.1 x64 + KenshiLib (kenshilib.lib, OgreMain_x64.lib).
set "LIB=%VC%\lib\amd64;%SDK%\Lib\x64;%KL%\KenshiLib\Libraries"

echo === Building KenshiCoop.dll (%CONFIG%^|x64, v100) ===

REM Fail early and legibly when the toolchain is absent: without this the build
REM dies much later inside MSBuild with an opaque toolset error. These use goto
REM rather than if(...) blocks because %VC%/%SDK% expand to paths containing
REM "(x86)", whose ")" would close a parenthesised block early.
if not exist "%VC%\bin\amd64\cl.exe" goto :no_v100
if not exist "%SDK%\Include"         goto :no_sdk71
goto :toolchain_ok

:no_v100
echo ERROR: the VC++ 2010 ^(v100^) x64 compiler was not found at
echo          %VC%\bin\amd64\cl.exe
echo        KenshiLib requires this exact toolset. Install Windows SDK 7.1 plus
echo        the "VC++ 2010 SP1 Compiler Update for the Windows SDK 7.1"
echo        ^(KB2519277^). See resources/BUILD_SETUP.md Part A.
exit /b 1

:no_sdk71
echo ERROR: Windows SDK 7.1 headers not found at
echo          %SDK%\Include
echo        See resources/BUILD_SETUP.md Part A.
exit /b 1

:toolchain_ok
where cl.exe

REM UseEnv=true: use the INCLUDE/LIB/PATH above instead of registry-derived paths.
REM TrackFileAccess=false: avoid Tracker.exe TRK0002 under redirected shells.
"%MSBUILD%" "%REPO%\src\plugin\KenshiCoop.vcxproj" /p:Configuration=%CONFIG% /p:Platform=x64 /p:UseEnv=true /p:TrackFileAccess=false /nologo /v:minimal

endlocal
