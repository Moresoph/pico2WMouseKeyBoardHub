#Requires -Version 5.1
<#
.SYNOPSIS
Configures and builds pico_bt_hub.uf2 using workspace-local dependencies.
.DESCRIPTION
Run bootstrap.ps1 first. This script never downloads tools or flashes hardware.
SdkPath and ToolchainPath default to their PICO_* environment overrides, then
the local installation. Use a new BuildDirectory when changing toolchains.
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo', 'MinSizeRel')]
    [string]$Configuration = 'Release',
    [string]$BuildDirectory = 'build',
    [ValidateRange(1, 256)]
    [int]$Jobs = [Math]::Max(1, [Environment]::ProcessorCount),
    [switch]$OpenClassicInputTest,
    [switch]$ClassicOutputTest,
    [switch]$ClassicInputPeerRole,
    [switch]$MouseInterval7p5,
    [switch]$ClassicOutput7p5,
    [switch]$UsbHostProbe,
    [switch]$DualBluetooth,
    [string]$SdkPath = $env:PICO_SDK_PATH,
    [string]$ToolchainPath = $env:PICO_TOOLCHAIN_PATH
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
if ($DualBluetooth -and ($UsbHostProbe -or $ClassicOutputTest -or $OpenClassicInputTest)) {
    throw '-DualBluetooth cannot be combined with USB Host, Classic output or open-input experiments.'
}
if ($MouseInterval7p5 -and -not $ClassicOutputTest) {
    throw '-MouseInterval7p5 requires -ClassicOutputTest.'
}
if ($ClassicOutput7p5 -and -not $ClassicOutputTest) {
    throw '-ClassicOutput7p5 requires -ClassicOutputTest.'
}
if ($UsbHostProbe -and $ClassicOutputTest) {
    throw '-UsbHostProbe requires BLE output; do not combine it with -ClassicOutputTest.'
}
$tools = Join-Path $root '.tools'
if ($DualBluetooth -and -not $PSBoundParameters.ContainsKey('BuildDirectory')) {
    $BuildDirectory = 'build\dual-bluetooth'
}
if ($ClassicOutputTest -and -not $PSBoundParameters.ContainsKey('BuildDirectory')) {
    $BuildDirectory = 'build\classic-output'
}
if ($UsbHostProbe -and -not $PSBoundParameters.ContainsKey('BuildDirectory')) {
    $BuildDirectory = 'build\usb-host-probe'
}
if ([string]::IsNullOrWhiteSpace($SdkPath)) {
    $SdkPath = Join-Path $root '.deps\pico-sdk'
}
if ([string]::IsNullOrWhiteSpace($ToolchainPath)) {
    $ToolchainPath = Join-Path $tools 'arm-gnu-toolchain-14.2.rel1'
}
if (-not [IO.Path]::IsPathRooted($BuildDirectory)) {
    $BuildDirectory = Join-Path $root $BuildDirectory
}
$BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
$cmake = Join-Path $tools 'python\cmake\data\bin\cmake.exe'
$ninja = Join-Path $tools 'python\bin\ninja.exe'
$pioasmDirectory = Join-Path $tools 'pico-sdk-tools-2.3.1\pioasm'
$picotoolDirectory = Join-Path $tools 'picotool-2.3.1\picotool'
foreach ($file in @(
    $cmake, $ninja, (Join-Path $SdkPath 'external\pico_sdk_import.cmake'),
    (Join-Path $pioasmDirectory 'pioasmConfig.cmake'),
    (Join-Path $pioasmDirectory 'pioasm.exe'),
    (Join-Path $picotoolDirectory 'picotoolConfig.cmake'),
    (Join-Path $picotoolDirectory 'picotool.exe')
)) {
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) {
        throw "Missing dependency: $file. Run .\tools\bootstrap.ps1 first."
    }
}
$SdkPath = (Resolve-Path -LiteralPath $SdkPath).Path
$ToolchainPath = (Resolve-Path -LiteralPath $ToolchainPath).Path
if (Test-Path -LiteralPath (Join-Path $ToolchainPath 'bin\arm-none-eabi-gcc.exe')) {
    $ToolchainPath = Join-Path $ToolchainPath 'bin'
}
if (-not (Test-Path -LiteralPath (Join-Path $ToolchainPath 'arm-none-eabi-gcc.exe'))) {
    throw "Arm compiler missing from $ToolchainPath. Run .\tools\bootstrap.ps1 first."
}
$python = (Get-Command python.exe -ErrorAction Stop).Source

# Windows SDK command lines need the native executables, not pip's Python wrappers.
$configureArguments = @(
    '-S', $root, '-B', $BuildDirectory, '-G', 'Ninja',
    "-DCMAKE_BUILD_TYPE=$Configuration",
    "-DCMAKE_MAKE_PROGRAM=$ninja",
    '-DPICO_BOARD=pico2_w',
    '-DPICO_PLATFORM=rp2350-arm-s',
    "-DHUB_CLASSIC_OPEN_TEST=$([int]$OpenClassicInputTest.IsPresent)",
    "-DHUB_CLASSIC_OUTPUT_TEST=$([int]$ClassicOutputTest.IsPresent)",
    "-DHUB_CLASSIC_INPUT_CENTRAL=$([int](-not $ClassicInputPeerRole.IsPresent))",
    "-DHUB_CLASSIC_MOUSE_7P5_TEST=$([int]$MouseInterval7p5.IsPresent)",
    "-DHUB_CLASSIC_OUTPUT_7P5_TEST=$([int]$ClassicOutput7p5.IsPresent)",
    "-DHUB_USB_HOST_PROBE=$([int]$UsbHostProbe.IsPresent)",
    "-DHUB_DUAL_BLUETOOTH=$([int]$DualBluetooth.IsPresent)",
    "-DPICO_SDK_PATH=$SdkPath",
    "-DPICO_TOOLCHAIN_PATH=$ToolchainPath",
    "-DPython3_EXECUTABLE=$python",
    "-Dpioasm_DIR=$pioasmDirectory",
    "-Dpicotool_DIR=$picotoolDirectory",
    '-DPICO_NO_PICOTOOL=0',
    '-DPICOTOOL_FORCE_FETCH_FROM_GIT=0'
)
if ($env:PICOTOOL_FORCE_FETCH_FROM_GIT) {
    throw 'Unset PICOTOOL_FORCE_FETCH_FROM_GIT to use the pinned local picotool.'
}
& $cmake @configureArguments
if ($LASTEXITCODE -ne 0) {
    throw "CMake configuration failed with exit code $LASTEXITCODE."
}
& $cmake --build $BuildDirectory --target pico_bt_hub --parallel $Jobs
if ($LASTEXITCODE -ne 0) {
    throw "Firmware build failed with exit code $LASTEXITCODE."
}
$uf2 = Join-Path $BuildDirectory 'pico_bt_hub.uf2'
if (-not (Test-Path -LiteralPath $uf2 -PathType Leaf) -or (Get-Item -LiteralPath $uf2).Length -eq 0) {
    throw "Build did not produce the expected UF2: $uf2"
}
Write-Host "`nFirmware ready: $uf2"
Get-FileHash -LiteralPath $uf2 -Algorithm SHA256
