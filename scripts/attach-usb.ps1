# Attach the Waveshare ESP32-C6 board (USB JTAG/serial debug unit, VID 303a:1001)
# to WSL so the Docker flash/monitor containers can reach it as /dev/ttyACM0.
#
# Run from an elevated PowerShell prompt the first time:
#     PowerShell -ExecutionPolicy Bypass -File scripts\attach-usb.ps1
#
# After a fresh re-plug, just rerun this script — the `bind --force` is a
# one-time persist, the `attach` is per-session.

$ErrorActionPreference = 'Stop'

$vidpid = '303a:1001'
$line   = (usbipd list) | Select-String $vidpid | Select-Object -First 1
if (-not $line) {
    Write-Error "ESP32-C6 ($vidpid) not found. Is the board plugged in?"
    exit 1
}
$busid = ($line -split '\s+')[0]
Write-Host "Found ESP32-C6 at busid $busid"

if ($line -notmatch 'Shared|Attached') {
    Write-Host "Binding $busid (one-time, requires admin)..."
    usbipd bind --busid $busid --force
}

Write-Host "Attaching $busid to WSL..."
usbipd attach --wsl --busid $busid

Write-Host ""
Write-Host "Done. Verify in WSL:"
Write-Host "    wsl -d docker-desktop -- ls -l /dev/ttyACM0"
Write-Host ""
Write-Host "Then build / flash:"
Write-Host "    docker compose run --rm build"
Write-Host "    docker compose run --rm flashfs"
Write-Host "    docker compose run --rm flash"
Write-Host "    docker compose run --rm monitor"
