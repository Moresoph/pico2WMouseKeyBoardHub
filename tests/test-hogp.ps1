#Requires -Version 5.1
[CmdletBinding()]
param([string]$SdkPath = $env:PICO_SDK_PATH)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
if ([string]::IsNullOrWhiteSpace($SdkPath)) { $SdkPath = Join-Path $root '.deps\pico-sdk' }
$btstack = Join-Path $SdkPath 'lib\btstack'
$compiler = Join-Path $root '.tools\tcc-0.9.27\tcc\tcc.exe'
$cmake = Join-Path $root '.tools\python\cmake\data\bin\cmake.exe'
$generated = Join-Path $root 'build\tests\generated'
& $cmake "-DBTSTACK_PATH=$btstack" "-DOUTPUT_DIR=$generated" -P (Join-Path $PSScriptRoot 'generate_hids_test.cmake')
if ($LASTEXITCODE -ne 0) { throw "HIDS overlay generation failed: $LASTEXITCODE" }
foreach ($suite in @('hids', 'hogp')) {
    $output = Join-Path $root "build\tests\hub_${suite}_tests.exe"
    & $compiler -Wall -Werror -DENABLE_BLE=1 -DENABLE_CLASSIC=1 `
        "-I$(Join-Path $root 'include')" "-I$(Join-Path $btstack 'src')" "-I$generated" `
        (Join-Path $PSScriptRoot "${suite}_test.c") `
        (Join-Path $btstack 'src\btstack_util.c') (Join-Path $btstack 'src\btstack_linked_list.c') -o $output
    if ($LASTEXITCODE -ne 0) { throw "$suite test compilation failed: $LASTEXITCODE" }
    & $output
    if ($LASTEXITCODE -ne 0) { throw "$suite tests failed: $LASTEXITCODE" }
}
