#Requires -Version 5.1
[CmdletBinding()]
param(
    [string]$IdfPath,
    [string]$IdfToolsPath,
    [string]$BuildDirectory = 'build\esp32-dual-bluetooth'
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path $PSScriptRoot -Parent
if (-not $IdfPath) { $IdfPath = Join-Path $root '.deps\esp-idf' }
if (-not $IdfToolsPath) { $IdfToolsPath = Join-Path $root '.tools\esp-idf' }
$IdfPath = (Resolve-Path -LiteralPath $IdfPath).Path
if (-not [IO.Path]::IsPathRooted($BuildDirectory)) { $BuildDirectory = Join-Path $root $BuildDirectory }
$BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
$env:IDF_PATH = $IdfPath
$env:IDF_TOOLS_PATH = $IdfToolsPath
$exports = & python (Join-Path $IdfPath 'tools\idf_tools.py') export --format key-value
if ($LASTEXITCODE -ne 0) { throw 'ESP-IDF tool export failed; install the pinned ESP32 tools first.' }
foreach ($line in $exports) {
    if ($line -match '^([A-Z_][A-Z_0-9]*)=(.*)$') {
        $name = $Matches[1]; $value = $Matches[2]
        if ($name -eq 'PATH') { $value = $value.Replace('%PATH%', $env:PATH) }
        [Environment]::SetEnvironmentVariable($name, $value, 'Process')
    }
}
$python = Join-Path $env:IDF_PYTHON_ENV_PATH 'Scripts\python.exe'
if (-not (Test-Path -LiteralPath $python)) { throw "IDF Python missing: $python" }
$env:IDF_TARGET = 'esp32'
& $python (Join-Path $IdfPath 'tools\idf.py') -C (Join-Path $root 'esp32') -B $BuildDirectory `
    "-DSDKCONFIG=$BuildDirectory\sdkconfig" build
if ($LASTEXITCODE -ne 0) { throw "ESP32 firmware build failed: $LASTEXITCODE" }
Get-FileHash -LiteralPath (Join-Path $BuildDirectory 'pico_dual_bt_input.bin') -Algorithm SHA256
