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
$output = Join-Path $outputDirectory 'hub_classic_mux_tests.exe'
& $compiler -Wall -Werror -DENABLE_CLASSIC=1 -DENABLE_BLE=1 `
    "-I$(Join-Path $root 'include')" "-I$btstack" `
    (Join-Path $root 'tests\classic_mux_test.c') (Join-Path $btstack 'btstack_util.c') -o $output
if ($LASTEXITCODE -ne 0) { throw "Classic mux test compilation failed: $LASTEXITCODE" }
& $output
if ($LASTEXITCODE -ne 0) { throw "Classic mux tests failed: $LASTEXITCODE" }

$cmake = Join-Path $root '.tools\python\cmake\data\bin\cmake.exe'
$ninja = Join-Path $root '.tools\python\bin\ninja.exe'
$guardRoot = Join-Path $outputDirectory 'classic_mux_overlay'
New-Item -ItemType Directory -Force -Path $guardRoot | Out-Null
$guardScript = @'
cmake_minimum_required(VERSION 3.20)
project(ClassicMuxGuards LANGUAGES NONE)
get_filename_component(HUB_ROOT "${HUB_ROOT}" ABSOLUTE)
get_filename_component(PICO_BTSTACK_PATH "${PICO_BTSTACK_PATH}" ABSOLUTE)
include("${HUB_ROOT}/cmake/HubBtstackClassicMux.cmake")
set(PICO_SDK_VERSION_STRING "2.3.1")
set(host "${PICO_BTSTACK_PATH}/src/classic/hid_host.c")
set(device "${PICO_BTSTACK_PATH}/src/classic/hid_device.c")
add_library(pico_btstack_classic INTERFACE)
add_library(pico_btstack_base INTERFACE)
set_property(TARGET pico_btstack_base PROPERTY INTERFACE_SOURCES "/already/patched/hci.c")
set(sources "${host};/untouched/classic.c;${device}")
if(GUARD STREQUAL "duplicate")
    list(APPEND sources "${host}")
elseif(GUARD STREQUAL "missing")
    list(REMOVE_ITEM sources "${device}")
elseif(GUARD STREQUAL "version")
    set(PICO_SDK_VERSION_STRING "2.3.2")
elseif(GUARD STREQUAL "hash")
    file(READ "${host}" text)
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/mutated.c" "${text}\n")
    hub_generate_classic_mux_source("${CMAKE_CURRENT_BINARY_DIR}/mutated.c"
        "${CMAKE_CURRENT_BINARY_DIR}/must_not_exist.c" host)
elseif(GUARD STREQUAL "anchor_missing")
    hub_classic_replace("abc" "xyz" "replacement" 1 ignored)
elseif(GUARD STREQUAL "anchor_duplicate")
    hub_classic_replace("abcabc" "abc" "replacement" 1 ignored)
endif()
set_property(TARGET pico_btstack_classic PROPERTY INTERFACE_SOURCES "${sources}")
hub_patch_btstack_classic_mux()
get_target_property(actual pico_btstack_classic INTERFACE_SOURCES)
set(expected "${CMAKE_CURRENT_BINARY_DIR}/generated/btstack/classic/hid_host.c;/untouched/classic.c;${CMAKE_CURRENT_BINARY_DIR}/generated/btstack/classic/hid_device.c")
if(NOT actual STREQUAL expected)
    message(FATAL_ERROR "Classic target replacement lost an unrelated source")
endif()
get_target_property(hci pico_btstack_base INTERFACE_SOURCES)
if(NOT hci STREQUAL "/already/patched/hci.c")
    message(FATAL_ERROR "Classic overlay clobbered HCI overlay")
endif()
foreach(role host device)
    file(READ "${CMAKE_CURRENT_BINARY_DIR}/generated/btstack/classic/hid_${role}.c" text)
    if(text MATCHES "l2cap_register_service\\(" OR text MATCHES "l2cap_decline_connection\\(")
        message(FATAL_ERROR "Unpatched Classic registration/decline")
    endif()
endforeach()
# Normalized hash must accept a CRLF checkout, without modifying the SDK.
file(READ "${host}" text)
string(REPLACE "\r\n" "\n" text "${text}")
string(REPLACE "\n" "\r\n" text "${text}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/crlf.c" "${text}")
hub_generate_classic_mux_source("${CMAKE_CURRENT_BINARY_DIR}/crlf.c"
    "${CMAKE_CURRENT_BINARY_DIR}/crlf_patched.c" host)
'@
[IO.File]::WriteAllText((Join-Path $guardRoot 'CMakeLists.txt'), $guardScript)
$positiveBuild = Join-Path $guardRoot 'valid-ninja'
$arguments = @('-S', $guardRoot, '-G', 'Ninja', "-DCMAKE_MAKE_PROGRAM=$ninja", "-DHUB_ROOT=$root",
    "-DPICO_BTSTACK_PATH=$(Split-Path $btstack -Parent)")
$log = & $cmake @arguments -B $positiveBuild 2>&1
if ($LASTEXITCODE -ne 0) { $log | Write-Output; throw 'Classic mux overlay generation failed' }
foreach ($guard in @('duplicate', 'missing', 'version', 'hash', 'anchor_missing', 'anchor_duplicate')) {
    $log = & $cmake @arguments -B (Join-Path $guardRoot $guard) "-DGUARD=$guard" 2>&1
    if ($LASTEXITCODE -eq 0 -or ($log -join "`n") -notmatch 'Expected exactly|requires Pico SDK|differs from pinned|Pinned Classic mux anchor') {
        $log | Write-Output
        throw "Classic mux guard did not reject $guard for the expected reason"
    }
}
foreach ($role in @('host', 'device')) {
    $source = Join-Path $positiveBuild "generated\btstack\classic\hid_$role.c"
    & $compiler -Wall -Werror -DENABLE_CLASSIC=1 -DENABLE_BLE=1 `
        "-I$(Join-Path $root 'include')" "-I$btstack" `
        -c $source -o (Join-Path $outputDirectory "classic_mux_hid_$role.o")
    if ($LASTEXITCODE -ne 0) { throw "Generated Classic $role compilation failed: $LASTEXITCODE" }
}
Write-Output 'Classic mux overlay guards passed (pins, anchors, replacement, CRLF, generated C compilation).'
