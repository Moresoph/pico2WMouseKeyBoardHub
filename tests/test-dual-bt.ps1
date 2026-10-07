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
$exe = Join-Path $out 'link_output_test.exe'
& $cc -Wall -Werror "-I$root\include" "$root\tests\link_output_test.c" "$root\src\hub_link.c" -o $exe
if ($LASTEXITCODE) { throw 'Computer telemetry compilation failed' }
& $exe
if ($LASTEXITCODE) { throw 'Computer telemetry tests failed' }
$exe = Join-Path $out 'classic_security_bridge_test.exe'
& $cc -Wall -Werror "-I$root\include" "-I$root\esp32\main" `
    "-I$root\tests\esp_stubs" `
    "-I$root\.deps\esp-idf\components\esp_common\include" `
    "$root\tests\classic_security_bridge_test.c" -o $exe
if ($LASTEXITCODE) { throw 'Classic encryption bridge compilation failed' }
& $exe
if ($LASTEXITCODE) { throw 'Classic encryption bridge tests failed' }
$exe = Join-Path $out 'esp_reconnect_test.exe'
& $cc -Wall -Werror "-I$root\esp32\main" "$root\tests\esp_reconnect_test.c" -o $exe
if ($LASTEXITCODE) { throw 'Reconnect policy compilation failed' }
& $exe
if ($LASTEXITCODE) { throw 'Reconnect policy tests failed' }
foreach($suite in @('ui_records','pc_slots')){
    $exe=Join-Path $out "$($suite)_test.exe"
    & $cc -Wall -Werror -DHUB_DUAL_BLUETOOTH=1 "-I$root\include" "-I$btstack" "$root\tests\$($suite)_test.c" -o $exe
    if($LASTEXITCODE){throw "$suite compilation failed"}
    & $exe
    if($LASTEXITCODE){throw "$suite tests failed"}
}
$cmake = Join-Path $root '.tools\python\cmake\data\bin\cmake.exe'
& $cmake "-DROOT=$root" "-DOUTPUT=$out\input_management_generated.inc" -P "$root\tests\generate_enrollment_test.cmake"
if($LASTEXITCODE){throw 'Production enrollment extraction failed'}
$exe=Join-Path $out 'input_enrollment_test.exe'
& $cc -Wall -Werror "-I$out" "-I$root\include" "-I$root\esp32\main" "-I$root\tests\esp_stubs" `
    "-I$root\.deps\esp-idf\components\esp_common\include" "$root\tests\input_enrollment_test.c" `
    "$root\src\hub_link.c" -o $exe
if($LASTEXITCODE){throw 'Production enrollment compilation failed'}
& $exe
if($LASTEXITCODE){throw 'Production enrollment tests failed'}
$exe=Join-Path $out 'ap_password_test.exe'
& $cc -Wall -Werror "-I$root\esp32\main" "$root\tests\ap_password_test.c" -o $exe
if($LASTEXITCODE){throw 'AP password policy compilation failed'}
& $exe
if($LASTEXITCODE){throw 'AP password policy tests failed'}
& $cmake "-DROOT=$root" "-DOUTPUT=$out\reconnect_lifecycle_generated.inc" -P "$root\tests\generate_reconnect_lifecycle_test.cmake"
if($LASTEXITCODE){throw 'Production reconnect extraction failed'}
$exe=Join-Path $out 'reconnect_lifecycle_test.exe'
& $cc -Wall -Werror "-I$out" "-I$root\include" "-I$root\esp32\main" "-I$root\tests\esp_stubs" `
    "-I$root\.deps\esp-idf\components\esp_common\include" "-I$btstack" "$root\tests\reconnect_lifecycle_test.c" `
    "$root\src\hub_remote_hid.c" "$btstack\btstack_hid_parser.c" "$btstack\btstack_util.c" -o $exe
if($LASTEXITCODE){throw 'Production reconnect compilation failed'}
& $exe
if($LASTEXITCODE){throw 'Production reconnect tests failed'}
& $cmake "-DROOT=$root" "-DOUTPUT=$out\web_startup_generated.inc" -P "$root\tests\generate_web_startup_test.cmake"
if($LASTEXITCODE){throw 'Production web startup extraction failed'}
$exe=Join-Path $out 'web_startup_test.exe'
& $cc -Wall -Werror "-I$out" "-I$root\tests\esp_stubs" "-I$root\.deps\esp-idf\components\esp_common\include" `
    "$root\tests\web_startup_test.c" -o $exe
if($LASTEXITCODE){throw 'Web startup compilation failed'}
& $exe
if($LASTEXITCODE){throw 'Web startup tests failed'}
