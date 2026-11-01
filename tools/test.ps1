$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual Studio C++ build tools required.' }
$out = Join-Path $root 'test-output'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$batch = Join-Path $out 'test.cmd'
@"
@echo off
call "$vs\VC\Auxiliary\Build\vcvars64.bat" >nul
cl /nologo /W4 /WX /D_CRT_SECURE_NO_WARNINGS /EHsc /std:c++17 /utf-8 /I"$root\common" "$root\tests\control_tests.cpp" "$root\common\core.cpp" /Fo"$out\\" /Fe"$out\core.exe"
if errorlevel 1 exit /b 1
"$out\core.exe"
if errorlevel 1 exit /b 1
cl /nologo /W4 /WX /D_CRT_SECURE_NO_WARNINGS /EHsc /std:c++17 /utf-8 /I"$root\common" "$root\tests\lcd_tests.cpp" "$root\common\lcd.cpp" /Fo"$out\\" /Fe"$out\lcd.exe"
if errorlevel 1 exit /b 1
"$out\lcd.exe" "$out"
if errorlevel 1 exit /b 1
cl /nologo /W4 /WX /D_CRT_SECURE_NO_WARNINGS /EHsc /std:c++17 /utf-8 /I"$root\common" "$root\tests\analysis_tests.cpp" "$root\common\analysis.cpp" "$root\common\core.cpp" /Fo"$out\\" /Fe"$out\analysis.exe"
if errorlevel 1 exit /b 1
cl /nologo /W0 /D_CRT_SECURE_NO_WARNINGS /c "C:\Espressif\frameworks\esp-idf-v5.3.1\components\json\cJSON\cJSON.c" /Fo"$out\cJSON.obj"
if errorlevel 1 exit /b 1
cl /nologo /W4 /WX /D_CRT_SECURE_NO_WARNINGS /EHsc /std:c++17 /utf-8 /I"$root\common" /I"C:\Espressif\frameworks\esp-idf-v5.3.1\components\json\cJSON" "$root\tests\network_tests.cpp" "$root\common\network_config.cpp" "$root\common\mqtt_discovery.cpp" "$out\cJSON.obj" /Fo"$out\\" /Fe"$out\network.exe"
if errorlevel 1 exit /b 1
"$out\network.exe" "$out\network-fixture.json" "$out\discovery.json"

if errorlevel 1 exit /b 1
"$out\analysis.exe"
if errorlevel 1 exit /b 1
cl /nologo /W4 /WX /D_CRT_SECURE_NO_WARNINGS /EHsc /std:c++17 /utf-8 /I"$root\common" "$root\tests\policy_tests.cpp" /Fo"$out\\" /Fe"$out\policy.exe"
if errorlevel 1 exit /b 1
"$out\policy.exe"

"@ -replace "`r?`n", "`r`n" | Set-Content -LiteralPath $batch -Encoding utf8NoBOM
& cmd /c $batch
if ($LASTEXITCODE -ne 0) { throw 'Solo core tests failed.' }
