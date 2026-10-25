#Requires -Version 5.1
[CmdletBinding()]
param([string]$SdkPath = $env:PICO_SDK_PATH)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
if ([string]::IsNullOrWhiteSpace($SdkPath)) { $SdkPath = Join-Path $root '.deps\pico-sdk' }
$compiler = Join-Path $root '.tools\tcc-0.9.27\tcc\tcc.exe'
$btstack = Join-Path $SdkPath 'lib\btstack\src'
$outputDirectory = Join-Path $root 'build\tests'
New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
$output = Join-Path $outputDirectory 'hub_output_tests.exe'
& $compiler -Wall -Werror -DENABLE_BLE=1 `
    "-I$(Join-Path $root 'include')" "-I$btstack" `
    "-I$(Join-Path $root 'tests\output_stubs')" `
    (Join-Path $root 'tests\output_test.c') (Join-Path $btstack 'btstack_util.c') -o $output
if ($LASTEXITCODE -ne 0) { throw "Output test compilation failed: $LASTEXITCODE" }
$log = & $output 2>&1
if ($LASTEXITCODE -ne 0) {
    $log | Write-Output
    throw "Output tests failed: $LASTEXITCODE"
}
$log | Select-Object -Last 1
$output = Join-Path $outputDirectory 'hub_router_output_switch_tests.exe'
& $compiler -Wall -Werror -DENABLE_BLE=1 -DHUB_DUAL_BLUETOOTH=1 -DHUB_TEST_REAL_ROUTER=1 `
    "-I$(Join-Path $root 'include')" "-I$btstack" `
    "-I$(Join-Path $root 'tests\output_stubs')" `
    (Join-Path $root 'tests\output_test.c') (Join-Path $root 'src\hub_router.c') `
    (Join-Path $btstack 'btstack_util.c') -o $output
if ($LASTEXITCODE -ne 0) { throw "Real router/output compilation failed: $LASTEXITCODE" }
$log = & $output 2>&1
if ($LASTEXITCODE -ne 0) {
    $log | Write-Output
    throw "Real router/output tests failed: $LASTEXITCODE"
}
$log | Select-Object -Last 1
$output = Join-Path $outputDirectory 'hub_output_dual_dedup_tests.exe'
& $compiler -Wall -Werror -DENABLE_BLE=1 -DHUB_DUAL_BLUETOOTH=1 `
    "-I$(Join-Path $root 'include')" "-I$btstack" `
    "-I$(Join-Path $root 'tests\output_stubs')" `
    (Join-Path $root 'tests\output_test.c') (Join-Path $btstack 'btstack_util.c') -o $output
if ($LASTEXITCODE -ne 0) { throw "Dual-board output compilation failed: $LASTEXITCODE" }
$log = & $output 2>&1
if ($LASTEXITCODE -ne 0) {
    $log | Write-Output
    throw "Dual-board output tests failed: $LASTEXITCODE"
}
$log | Select-Object -Last 1
