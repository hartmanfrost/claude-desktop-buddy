# Fallback flasher that runs esptool from the host instead of the container.
# Use this if usbipd's /dev/ttyACM0 passthrough into Docker is flaky.
#
#     PowerShell -ExecutionPolicy Bypass -File scripts\flash-from-host.ps1
#
# Requires: Python 3.x on PATH. The first run installs esptool into a
# project-local venv so the host stays clean.

$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..')
$venv = Join-Path $repo '.venv'
$python = if (Get-Command py -ErrorAction SilentlyContinue) { 'py' } else { 'python' }

if (-not (Test-Path $venv)) {
    Write-Host "Creating local venv at $venv"
    & $python -m venv $venv
    & "$venv\Scripts\python.exe" -m pip install --upgrade pip esptool
}

$esptool = "$venv\Scripts\esptool.exe"
$build   = Join-Path $repo 'firmware\.pio\build\waveshare-esp32-c6'

$bootloader = Join-Path $build 'bootloader.bin'
$partitions = Join-Path $build 'partitions.bin'
$firmware   = Join-Path $build 'firmware.bin'
$littlefs   = Join-Path $build 'littlefs.bin'

foreach ($p in @($bootloader, $partitions, $firmware)) {
    if (-not (Test-Path $p)) {
        Write-Error "Missing $p — run 'docker compose run --rm build' first."
        exit 1
    }
}

$args = @(
    '--chip', 'esp32c6',
    '--port', 'COM3',
    '--baud', '460800',
    'write_flash',
    '-z',
    # arduino-esp32 v3.x bootloader for ESP32-C6 is built in DIO mode;
    # writing the app in QIO leaves the chip stuck in a TG0_WDT reset
    # loop right after `ets_loader.c 67`. Match the bootloader.
    '--flash_mode', 'dio',
    '--flash_freq', '80m',
    '--flash_size', '4MB',
    '0x0',     $bootloader,
    '0x8000',  $partitions,
    '0x10000', $firmware
)
if (Test-Path $littlefs) {
    $args += '0x200000'
    $args += $littlefs
} else {
    Write-Host "Note: $littlefs not built yet (run 'docker compose run --rm fs')."
}

Write-Host "Flashing via host esptool…"
& $esptool @args
