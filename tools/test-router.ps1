#Requires -Version 5.1
[CmdletBinding()]
param(
    [switch]$BootstrapCompiler,
    [string]$SdkPath = $env:PICO_SDK_PATH
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$compilerRoot = Join-Path $root '.tools\tcc-0.9.27'
$compiler = Join-Path $compilerRoot 'tcc\tcc.exe'
$archive = Join-Path $root '.tools\downloads\tcc-0.9.27-win64-bin.zip'
$expectedHash = '34a721949a2583fdff725312da092fa0f5f1f284b702e6f811c6954714faabb2'

if (-not (Test-Path -LiteralPath $compiler -PathType Leaf)) {
    if (-not $BootstrapCompiler) {
        throw 'Native test compiler missing. Run tools\test-router.ps1 -BootstrapCompiler.'
    }
    New-Item -ItemType Directory -Force -Path (Split-Path $archive -Parent) | Out-Null
    if (-not (Test-Path -LiteralPath $archive)) {
        Invoke-WebRequest -UseBasicParsing `
            -Uri 'https://download.savannah.gnu.org/releases/tinycc/tcc-0.9.27-win64-bin.zip' `
            -OutFile $archive -TimeoutSec 120
    }
    if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $expectedHash) {
        throw "Compiler archive checksum mismatch: $archive"
    }
    Expand-Archive -LiteralPath $archive -DestinationPath $compilerRoot -Force
}

$outputDirectory = Join-Path $root 'build\tests'
New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
foreach ($suite in @('router', 'passkey', 'button', 'smp_decode', 'classic_decode', 'diag_ring')) {
    $output = Join-Path $outputDirectory "hub_${suite}_tests.exe"
    & $compiler -Wall -Werror "-I$(Join-Path $root 'include')" `
        (Join-Path $root "tests\${suite}_test.c") (Join-Path $root "src\hub_${suite}.c") -o $output
    if ($LASTEXITCODE -ne 0) { throw "$suite test compilation failed: $LASTEXITCODE" }
    & $output
    if ($LASTEXITCODE -ne 0) { throw "$suite tests failed: $LASTEXITCODE" }
}

if ([string]::IsNullOrWhiteSpace($SdkPath)) { $SdkPath = Join-Path $root '.deps\pico-sdk' }
$btstack = Join-Path $SdkPath 'lib\btstack\src'
$hidParser = Join-Path $btstack 'btstack_hid_parser.c'
$util = Join-Path $btstack 'btstack_util.c'
if (-not (Test-Path -LiteralPath $hidParser) -or -not (Test-Path -LiteralPath $util)) {
    throw 'SDK HID parser missing for input tests. Run bootstrap.ps1 or pass -SdkPath.'
}
$output = Join-Path $outputDirectory 'hub_input_tests.exe'
& $compiler -Wall -Werror -DENABLE_BLE=1 -DENABLE_CLASSIC=1 `
    "-I$(Join-Path $root 'include')" "-I$btstack" `
    (Join-Path $root 'tests\input_test.c') $hidParser $util -o $output
if ($LASTEXITCODE -ne 0) { throw "Input test compilation failed: $LASTEXITCODE" }
& $output
if ($LASTEXITCODE -ne 0) { throw "Input tests failed: $LASTEXITCODE" }
$output = Join-Path $outputDirectory 'hub_open_input_tests.exe'
& $compiler -Wall -Werror -DENABLE_BLE=1 -DENABLE_CLASSIC=1 -DHUB_CLASSIC_OPEN_TEST=1 `
    "-I$(Join-Path $root 'include')" "-I$btstack" `
    (Join-Path $root 'tests\open_input_test.c') $hidParser $util -o $output
if ($LASTEXITCODE -ne 0) { throw "Open input test compilation failed: $LASTEXITCODE" }
& $output
if ($LASTEXITCODE -ne 0) { throw "Open input tests failed: $LASTEXITCODE" }
$output = Join-Path $outputDirectory 'hub_security_policy_tests.exe'
& $compiler -Wall -Werror -DENABLE_BLE=1 -DENABLE_CLASSIC=1 `
    "-I$(Join-Path $root 'include')" "-I$btstack" `
    (Join-Path $root 'tests\security_policy_test.c') `
    (Join-Path $root 'src\hub_security_policy.c') -o $output
if ($LASTEXITCODE -ne 0) { throw "Security profile compilation failed: $LASTEXITCODE" }
& $output
if ($LASTEXITCODE -ne 0) { throw "Security profile tests failed: $LASTEXITCODE" }

& (Join-Path $root 'tests\test-hci-reconnect.ps1') -SdkPath $SdkPath
& (Join-Path $root 'tests\test-output.ps1') -SdkPath $SdkPath
$output = Join-Path $outputDirectory 'hub_latency_tests.exe'
& $compiler -Wall -Werror -DENABLE_BLE=1 -DENABLE_CLASSIC=1 `
    "-I$(Join-Path $root 'include')" "-I$btstack" `
    (Join-Path $root 'tests\latency_test.c') $util -o $output
if ($LASTEXITCODE -ne 0) { throw "Latency test compilation failed: $LASTEXITCODE" }
& $output
if ($LASTEXITCODE -ne 0) { throw "Latency tests failed: $LASTEXITCODE" }
$output = Join-Path $outputDirectory 'hub_perf_tests.exe'
& $compiler -Wall -Werror -DENABLE_BLE=1 -DENABLE_CLASSIC=1 `
    "-I$(Join-Path $root 'include')" "-I$btstack" `
    (Join-Path $root 'tests\perf_test.c') $util -o $output
if ($LASTEXITCODE -ne 0) { throw "Performance recorder compilation failed: $LASTEXITCODE" }
& $output
if ($LASTEXITCODE -ne 0) { throw "Performance recorder tests failed: $LASTEXITCODE" }
