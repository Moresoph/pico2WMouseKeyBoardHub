#Requires -Version 5.1
<#
.SYNOPSIS
Downloads pinned Windows x64 build dependencies into this workspace.
.DESCRIPTION
Requires Python 3 with pip and Git. No administrator rights, global installs,
PATH changes, board access, or flashing. Existing PICO_SDK_PATH and
PICO_TOOLCHAIN_PATH overrides are validated, never installed into.
#>
[CmdletBinding()]
param(
    [string]$SdkPath = $env:PICO_SDK_PATH,
    [string]$ToolchainPath = $env:PICO_TOOLCHAIN_PATH,
    [string]$GitProxy = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$tools = Join-Path $root '.tools'
$downloads = Join-Path $tools 'downloads'
$sdkCommit = '079c6f39023649b154152db30f1d781e884879bc'
$sdkVersion = '2.3.1'
$localSdk = Join-Path $root '.deps\pico-sdk'
$localToolchain = Join-Path $tools 'arm-gnu-toolchain-14.2.rel1'
$pythonPackages = Join-Path $tools 'python'
$sdkTools = Join-Path $tools 'pico-sdk-tools-2.3.1'
$picotool = Join-Path $tools 'picotool-2.3.1'

function Invoke-Checked {
    param([string]$File, [string[]]$Arguments)
    & $File @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$File failed with exit code $LASTEXITCODE."
    }
}

function Get-VerifiedDownload {
    param([string]$Name, [string]$Url, [string]$Sha256)
    $destination = Join-Path $downloads $Name
    if (Test-Path -LiteralPath $destination -PathType Leaf) {
        if ((Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash -ne $Sha256) {
            throw "Checksum mismatch: $destination. Remove this specific file and rerun bootstrap."
        }
        return $destination
    }
    Write-Host "Downloading $Name"
    $partial = "$destination.partial"
    Invoke-WebRequest -UseBasicParsing -Uri $Url -OutFile $partial -TimeoutSec 1800
    if ((Get-FileHash -LiteralPath $partial -Algorithm SHA256).Hash -ne $Sha256) {
        throw "Checksum mismatch: $partial. The download was not installed."
    }
    Move-Item -LiteralPath $partial -Destination $destination -Force
    return $destination
}

function Assert-SdkFiles {
    param([string]$Path)
    foreach ($file in @(
        'external\pico_sdk_import.cmake',
        'lib\btstack\src\bluetooth.h',
        'lib\cyw43-driver\src\cyw43.h',
        'lib\tinyusb\src\tusb.h',
        'lib\mbedtls\library\aes.c'
    )) {
        if (-not (Test-Path -LiteralPath (Join-Path $Path $file) -PathType Leaf)) {
            throw "SDK dependency missing: $Path\$file. Initialize the required SDK submodules."
        }
    }
    $versionFile = Join-Path $Path 'pico_sdk_version.cmake'
    if (-not (Test-Path -LiteralPath $versionFile)) {
        throw "Not a Pico SDK installation: $Path"
    }
    $versionText = Get-Content -LiteralPath $versionFile -Raw
    foreach ($component in @(@('MAJOR', '2'), @('MINOR', '3'), @('REVISION', '1'))) {
        if ($versionText -notmatch ("set\(PICO_SDK_VERSION_{0}\s+{1}\)" -f $component[0], $component[1])) {
            throw "Pico SDK $sdkVersion is required: $Path"
        }
    }
}

if ($env:OS -ne 'Windows_NT' -or -not [Environment]::Is64BitOperatingSystem) {
    throw 'This bootstrap supports 64-bit Windows only.'
}
$python = (Get-Command python.exe -ErrorAction Stop).Source
$git = (Get-Command git.exe -ErrorAction Stop).Source
$gitOptions = @()
if (-not [string]::IsNullOrWhiteSpace($GitProxy)) {
    # Git does not automatically inherit the Windows web proxy used by downloads.
    $gitOptions = @('-c', "http.proxy=$GitProxy")
    Write-Host 'Using the supplied proxy for Git commands in this bootstrap only.'
}
Invoke-Checked $python @('--version')
Invoke-Checked $python @('-m', 'pip', '--version')
Invoke-Checked $git @('--version')
New-Item -ItemType Directory -Force -Path $downloads | Out-Null

# Hashes are from the pinned PyPI release JSON and official vendor releases.
$cmakeWheel = Get-VerifiedDownload `
    'cmake-3.31.10-py3-none-win_amd64.whl' `
    'https://files.pythonhosted.org/packages/84/4b/e433ab430c580ec8e3d54cb2ec5c7ea76ae0b1e6ef2c2998c854f21d7780/cmake-3.31.10-py3-none-win_amd64.whl' `
    'f1ea1fe826355560e8976c3d5794d9357444209bc0e0d56676c71e6a571fd474'
$ninjaWheel = Get-VerifiedDownload `
    'ninja-1.13.0-py3-none-win_amd64.whl' `
    'https://files.pythonhosted.org/packages/29/45/c0adfbfb0b5895aa18cec400c535b4f7ff3e52536e0403602fc1a23f7de9/ninja-1.13.0-py3-none-win_amd64.whl' `
    'fb8ee8719f8af47fed145cced4a85f0755dd55d45b2bddaf7431fa89803c5f3e'
$cmake = Join-Path $pythonPackages 'cmake\data\bin\cmake.exe'
$ninja = Join-Path $pythonPackages 'bin\ninja.exe'
if (-not (Test-Path -LiteralPath $cmake) -or -not (Test-Path -LiteralPath $ninja)) {
    $pipWork = Join-Path $tools 'pip-work'
    New-Item -ItemType Directory -Force -Path $pipWork | Out-Null
    $oldTemp = $env:TEMP
    $oldTmp = $env:TMP
    try {
        $env:TEMP = $pipWork
        $env:TMP = $pipWork
        Invoke-Checked $python @(
            '-m', 'pip', 'install', '--no-index', '--no-deps', '--no-cache-dir',
            '--disable-pip-version-check', '--no-warn-script-location',
            '--upgrade', '--target', $pythonPackages, $cmakeWheel, $ninjaWheel
        )
    }
    finally {
        $env:TEMP = $oldTemp
        $env:TMP = $oldTmp
    }
}
if ((& $cmake --version | Select-Object -First 1) -ne 'cmake version 3.31.10') {
    throw "Unexpected CMake version in $pythonPackages."
}
if ((& $ninja --version) -ne '1.13.0.git.kitware.jobserver-pipe-1') {
    throw "Unexpected Ninja version in $pythonPackages."
}

if ([string]::IsNullOrWhiteSpace($ToolchainPath)) {
    $ToolchainPath = $localToolchain
    $armArchive = Get-VerifiedDownload `
        'arm-gnu-toolchain-14.2.rel1-mingw-w64-x86_64-arm-none-eabi.zip' `
        'https://developer.arm.com/-/media/Files/downloads/gnu/14.2.rel1/binrel/arm-gnu-toolchain-14.2.rel1-mingw-w64-x86_64-arm-none-eabi.zip' `
        'f074615953f76036e9a51b87f6577fdb4ed8e77d3322a6f68214e92e7859888f'
    if (-not (Test-Path -LiteralPath (Join-Path $ToolchainPath 'bin\arm-none-eabi-g++.exe'))) {
        Write-Host 'Extracting Arm GNU 14.2.Rel1'
        Expand-Archive -LiteralPath $armArchive -DestinationPath $ToolchainPath -Force
    }
}
$ToolchainPath = (Resolve-Path -LiteralPath $ToolchainPath).Path
$compilerBin = $ToolchainPath
if (Test-Path -LiteralPath (Join-Path $ToolchainPath 'bin\arm-none-eabi-gcc.exe')) {
    $compilerBin = Join-Path $ToolchainPath 'bin'
}
foreach ($compiler in @('arm-none-eabi-gcc.exe', 'arm-none-eabi-g++.exe', 'arm-none-eabi-objcopy.exe')) {
    if (-not (Test-Path -LiteralPath (Join-Path $compilerBin $compiler))) {
        throw "Incomplete Arm toolchain: $compilerBin\$compiler"
    }
}
Invoke-Checked (Join-Path $compilerBin 'arm-none-eabi-gcc.exe') @('--version')

$pioArchive = Get-VerifiedDownload `
    'pico-sdk-tools-2.3.1-x64-win.zip' `
    'https://github.com/raspberrypi/pico-sdk-tools/releases/download/v2.3.1-0/pico-sdk-tools-2.3.1-x64-win.zip' `
    'c5d18dae371eb088da61a038ccf56a23fbd19171a319461efebeae1646c06ade'
$picotoolArchive = Get-VerifiedDownload `
    'picotool-2.3.1-x64-win.zip' `
    'https://github.com/raspberrypi/pico-sdk-tools/releases/download/v2.3.1-0/picotool-2.3.1-x64-win.zip' `
    '68730be0813f8f35be2cca147cf7f1572662d5dcf0f5ba468e02a6dd9e85db2b'
if (-not (Test-Path -LiteralPath (Join-Path $sdkTools 'pioasm\pioasmConfig.cmake'))) {
    Expand-Archive -LiteralPath $pioArchive -DestinationPath $sdkTools -Force
}
if (-not (Test-Path -LiteralPath (Join-Path $picotool 'picotool\picotoolConfig.cmake'))) {
    Expand-Archive -LiteralPath $picotoolArchive -DestinationPath $picotool -Force
}
Invoke-Checked (Join-Path $sdkTools 'pioasm\pioasm.exe') @('--version')
Invoke-Checked (Join-Path $picotool 'picotool\picotool.exe') @('version')

if ([string]::IsNullOrWhiteSpace($SdkPath)) {
    $SdkPath = $localSdk
    if (-not (Test-Path -LiteralPath $SdkPath)) {
        New-Item -ItemType Directory -Force -Path (Split-Path $SdkPath -Parent) | Out-Null
        Invoke-Checked $git ($gitOptions + @('clone', '--depth', '1', '--branch', $sdkVersion,
            'https://github.com/raspberrypi/pico-sdk.git', $SdkPath))
    }
    $commit = & $git -C $SdkPath rev-parse HEAD
    if ($LASTEXITCODE -ne 0 -or $commit -ne $sdkCommit) {
        throw "Expected SDK $sdkVersion at commit $sdkCommit. Existing directory left untouched: $SdkPath"
    }
    $changes = & $git -C $SdkPath status --porcelain --untracked-files=no --ignore-submodules=all
    if ($LASTEXITCODE -ne 0 -or $changes) {
        throw "Local SDK has tracked modifications. Refusing to alter $SdkPath"
    }
    # Bluetooth-only cyw43_arch_none does not require lwIP or nested submodules.
    Invoke-Checked $git ($gitOptions + @('-C', $SdkPath, 'submodule', 'update', '--init', '--depth', '1',
        '--', 'lib/btstack', 'lib/cyw43-driver', 'lib/tinyusb', 'lib/mbedtls'))
}
$SdkPath = (Resolve-Path -LiteralPath $SdkPath).Path
Assert-SdkFiles $SdkPath

Write-Host "`nLocal dependencies are ready."
Write-Host "SDK:       $SdkPath"
Write-Host "Toolchain: $compilerBin"
Write-Host "CMake:     $cmake"
Write-Host 'Build:     .\tools\build.ps1'
Write-Host 'If you supplied -SdkPath or -ToolchainPath, pass the same overrides to build.ps1.'
