#Requires -Version 5.1
[CmdletBinding()]
param([string]$SdkPath = $env:PICO_SDK_PATH)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
if ([string]::IsNullOrWhiteSpace($SdkPath)) { $SdkPath = Join-Path $root '.deps\pico-sdk' }
$compiler = Join-Path $root '.tools\tcc-0.9.27\tcc\tcc.exe'
$outputDirectory = Join-Path $root 'build\tests'
New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
$generated = Join-Path $outputDirectory 'usb-hcd-generated'
& (Join-Path $root '.tools\python\cmake\data\bin\cmake.exe') "-DSDK_PATH=$SdkPath" `
    "-DOUTPUT_DIR=$generated" -P (Join-Path $PSScriptRoot 'generate_usb_hcd_test.cmake')
if ($LASTEXITCODE -ne 0) { throw 'Generating production USB HCD test failed' }
foreach ($suite in @('usb_descriptor', 'usb_host', 'usb_trace', 'usb_hcd', 'usb_hcd_rp2040', 'usb_mouse', 'usb_wire', 'usb_enum', 'usb_hub_timing')) {
    $output = Join-Path $outputDirectory "hub_${suite}_tests.exe"
    $extraSources = @()
    $defines = @()
    $source = "${suite}_test.c"
    if ($suite -eq 'usb_hcd_rp2040') {
        $source = 'usb_hcd_test.c'
        $defines = @('-DPICO_RP2350=0')
    }
    if ($suite -eq 'usb_mouse') {
        $extraSources = @((Join-Path $root 'src\hub_usb_mouse_decode.c'),
            (Join-Path $SdkPath 'lib\btstack\src\btstack_hid_parser.c'),
            (Join-Path $SdkPath 'lib\btstack\src\btstack_util.c'))
    }
    & $compiler -Wall -Werror "-I$generated" "-I$(Join-Path $PSScriptRoot 'usb_stubs')" `
        "-I$(Join-Path $root 'include')" "-I$(Join-Path $root 'include\usb_host')" `
        "-I$(Join-Path $SdkPath 'lib\btstack\src')" `
        (Join-Path $PSScriptRoot $source) (Join-Path $root 'src\hub_usb_descriptor.c') @defines @extraSources -o $output
    if ($LASTEXITCODE -ne 0) { throw "$suite compilation failed" }
    & $output
    if ($LASTEXITCODE -ne 0) { throw "$suite failed" }
}
& python -B (Join-Path $PSScriptRoot 'usb_wire_decode_test.py')
if ($LASTEXITCODE -ne 0) { throw 'USB wire decoder tests failed' }
