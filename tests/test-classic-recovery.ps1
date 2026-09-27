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
$generated = Join-Path $root 'build\tests\classic-recovery'
& $cmake "-DBTSTACK_PATH=$btstack" "-DOUTPUT_DIR=$generated" -P (Join-Path $PSScriptRoot 'generate_classic_recovery_test.cmake')
if ($LASTEXITCODE -ne 0) { throw 'Classic recovery overlay generation failed' }
$output = Join-Path $generated 'classic_recovery_tests.exe'
& $compiler -Wall -Werror -DENABLE_BLE=1 -DENABLE_CLASSIC=1 -DENABLE_SDP_EXTRA_QUERIES=1 `
    "-I$(Join-Path $root 'include')" "-I$(Join-Path $btstack 'src')" "-I$generated" `
    (Join-Path $PSScriptRoot 'classic_recovery_test.c') `
    (Join-Path $btstack 'src\btstack_util.c') (Join-Path $btstack 'src\btstack_linked_list.c') `
    (Join-Path $btstack 'src\classic\sdp_util.c') -o $output
if ($LASTEXITCODE -ne 0) { throw 'Classic recovery test compilation failed' }
& $output
if ($LASTEXITCODE -ne 0) { throw 'Classic recovery tests failed' }
