param(
  [string]$IdfPath = 'C:\Espressif\frameworks\esp-idf-v5.3.1',
  [string]$ToolsPath = 'C:\Espressif',
  [string]$PythonEnv = 'C:\Espressif\python_env\idf5.3_py3.11_env'
)
$ErrorActionPreference = 'Stop'
$project = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$python = Join-Path $PythonEnv 'Scripts\python.exe'
$env:IDF_TOOLS_PATH = $ToolsPath
$env:IDF_PYTHON_ENV_PATH = $PythonEnv
$env:PATH = "$PythonEnv\Scripts;$env:PATH"
$env:PYTHONUTF8 = '1'
. (Join-Path $IdfPath 'export.ps1')
# Independent ASCII build workspace, same layout as this standalone project.
$stage = Join-Path $ToolsPath 'thermostat-40-solo'
New-Item -ItemType Directory -Force -Path $stage | Out-Null
Copy-Item -LiteralPath "$project\firmware" -Destination $stage -Recurse -Force
Copy-Item -LiteralPath "$project\common" -Destination $stage -Recurse -Force
# A cached generated sdkconfig overrides changed defaults (for example HTTP header size).
# Preserve a user-supplied project config; otherwise regenerate from tracked defaults.
$generatedConfig = Join-Path $stage 'firmware\sdkconfig'
if (!(Test-Path -LiteralPath "$project\firmware\sdkconfig") -and (Test-Path -LiteralPath $generatedConfig)) {
  Remove-Item -LiteralPath $generatedConfig
}
& $python (Join-Path $IdfPath 'tools\idf.py') -C "$stage\firmware" -B "$stage\build" '-DIDF_TARGET=esp32' build
if ($LASTEXITCODE -ne 0) { throw 'Solo firmware compilation failed.' }
$out = Join-Path $project 'build'
New-Item -ItemType Directory -Force -Path $out | Out-Null
foreach ($name in @('esp_termostat_solo.bin','esp_termostat_solo.elf','esp_termostat_solo.map','bootloader\bootloader.bin','partition_table\partition-table.bin','flasher_args.json','ota_data_initial.bin')) {
  $destination = Join-Path $out $name
  New-Item -ItemType Directory -Force -Path (Split-Path $destination) | Out-Null
  Copy-Item -LiteralPath "$stage\build\$name" -Destination $destination -Force
}
Copy-Item -LiteralPath "$stage\firmware\dependencies.lock" -Destination "$out\dependencies.lock" -Force
& $python "$PSScriptRoot\package.py" $out
if ($LASTEXITCODE -ne 0) { throw 'Packaging failed.' }
