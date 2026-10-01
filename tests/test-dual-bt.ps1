#Requires -Version 5.1
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path $PSScriptRoot -Parent
$cc = Join-Path $root '.tools\tcc-0.9.27\tcc\tcc.exe'
$out = Join-Path $root 'build\tests'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$btstack = Join-Path $root '.deps\pico-sdk\lib\btstack\src'
$exe = Join-Path $out 'dual_bt_test.exe'
& $cc -Wall -Werror "-I$root\include" "$root\tests\dual_bt_test.c" `
    "$root\src\hub_link.c" "$root\src\hub_external_input.c" -o $exe
if ($LASTEXITCODE) { throw 'Dual Bluetooth compilation failed' }
& $exe
if ($LASTEXITCODE) { throw 'Dual Bluetooth tests failed' }
$exe = Join-Path $out 'dual_bt_router_test.exe'
& $cc -Wall -Werror "-I$root\include" "$root\tests\dual_bt_router_test.c" `
    "$root\src\hub_link.c" "$root\src\hub_external_input.c" "$root\src\hub_router.c" -o $exe
if ($LASTEXITCODE) { throw 'Dual Bluetooth router compilation failed' }
& $exe
if ($LASTEXITCODE) { throw 'Dual Bluetooth router tests failed' }
$exe = Join-Path $out 'remote_hid_test.exe'
& $cc -Wall -Werror "-I$root\esp32\main" "-I$root\include" "-I$btstack" `
    "$root\tests\remote_hid_test.c" "$root\src\hub_remote_hid.c" `
    "$btstack\btstack_hid_parser.c" "$btstack\btstack_util.c" -o $exe
if ($LASTEXITCODE) { throw 'Remote HID compilation failed' }
& $exe
if ($LASTEXITCODE) { throw 'Remote HID tests failed' }
$exe = Join-Path $out 'esp_input_policy_test.exe'
& $cc -Wall -Werror "-I$root\esp32\main" "$root\tests\esp_input_policy_test.c" -o $exe
if ($LASTEXITCODE) { throw 'ESP32 input policy compilation failed' }
& $exe
if ($LASTEXITCODE) { throw 'ESP32 input policy tests failed' }
