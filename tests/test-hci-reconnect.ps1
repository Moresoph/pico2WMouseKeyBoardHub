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
$ctest = Join-Path $root '.tools\python\cmake\data\bin\ctest.exe'
$ninja = Join-Path $root '.tools\python\bin\ninja.exe'
foreach ($file in @($compiler, $cmake, $ctest, $ninja, (Join-Path $btstack 'src\hci.c'))) {
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw "Missing HCI test dependency: $file" }
}
$outputDirectory = Join-Path $root 'build\tests'
$generated = Join-Path $outputDirectory 'generated'
& $cmake "-DBTSTACK_PATH=$btstack" "-DOUTPUT_DIR=$generated" `
    -P (Join-Path $PSScriptRoot 'generate_hci_reconnect_test.cmake')
if ($LASTEXITCODE -ne 0) { throw "HCI overlay generation failed: $LASTEXITCODE" }
$output = Join-Path $outputDirectory 'hub_hci_reconnect_tests.exe'
& $compiler -Wall -Werror -DENABLE_BLE=1 -DENABLE_CLASSIC=1 `
    "-I$(Join-Path $root 'include')" "-I$(Join-Path $btstack 'src')" "-I$generated" `
    (Join-Path $PSScriptRoot 'hci_reconnect_test.c') `
    (Join-Path $btstack 'src\btstack_linked_list.c') `
    (Join-Path $btstack 'src\btstack_util.c') -o $output
if ($LASTEXITCODE -ne 0) { throw "HCI reconnect test compilation failed: $LASTEXITCODE" }
& $output
if ($LASTEXITCODE -ne 0) { throw "HCI reconnect tests failed: $LASTEXITCODE" }

$guardBuild = Join-Path $outputDirectory 'hci_overlay'
& $cmake -S (Join-Path $PSScriptRoot 'hci_overlay') -B $guardBuild -G Ninja `
    "-DCMAKE_MAKE_PROGRAM=$ninja" "-DPICO_BTSTACK_PATH=$btstack"
if ($LASTEXITCODE -ne 0) { throw "HCI overlay target replacement failed: $LASTEXITCODE" }
& $ctest --test-dir $guardBuild --output-on-failure
if ($LASTEXITCODE -ne 0) { throw "HCI overlay guard tests failed: $LASTEXITCODE" }
