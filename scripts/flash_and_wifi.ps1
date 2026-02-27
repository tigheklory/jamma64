param(
  [string]$Uf2Path = (Join-Path $PSScriptRoot "..\build\jamma64.uf2"),
  [Parameter(Mandatory = $true)][string]$Ssid,
  [Parameter(Mandatory = $true)][string]$Password,
  [Parameter(Mandatory = $true)][string]$BootselDrive,
  [Parameter(Mandatory = $true)][string]$RuntimeDrive,
  [int]$TimeoutSec = 120,
  [switch]$NoPauses
)

$ErrorActionPreference = "Stop"

function Normalize-DriveLetter {
  param([string]$Letter)
  if ([string]::IsNullOrWhiteSpace($Letter)) { return "" }
  $d = $Letter.Trim()
  $d = $d.TrimEnd(":")
  $d = $d.TrimEnd("\")
  return $d.Substring(0, 1).ToUpperInvariant()
}

function Drive-Root {
  param([string]$Letter)
  return "$(Normalize-DriveLetter $Letter):\"
}

function Pause-IfEnabled {
  param([string]$Prompt)
  if (-not $NoPauses) {
    [void](Read-Host "$Prompt Press Enter to continue")
  }
}

function Wait-ForBootsel {
  param(
    [string]$DriveRoot,
    [int]$TimeoutSec
  )
  $elapsed = 0
  while ($elapsed -lt $TimeoutSec) {
    $infoUf2 = Join-Path $DriveRoot "INFO_UF2.TXT"
    $indexHtm = Join-Path $DriveRoot "INDEX.HTM"
    if ((Test-Path -LiteralPath $infoUf2) -or (Test-Path -LiteralPath $indexHtm)) {
      return $true
    }
    Start-Sleep -Seconds 1
    $elapsed++
  }
  return $false
}

function Wait-ForRuntime {
  param(
    [string]$DriveRoot,
    [int]$TimeoutSec
  )
  $elapsed = 0
  while ($elapsed -lt $TimeoutSec) {
    $wifiTxtUpper = Join-Path $DriveRoot "WIFI.TXT"
    $wifiTxtLower = Join-Path $DriveRoot "wifi.txt"
    if ((Test-Path -LiteralPath $wifiTxtUpper) -or (Test-Path -LiteralPath $wifiTxtLower)) {
      return $true
    }
    Start-Sleep -Seconds 1
    $elapsed++
  }
  return $false
}

if (-not (Test-Path -LiteralPath $Uf2Path)) {
  throw "UF2 not found: $Uf2Path"
}

$BootselRoot = Drive-Root $BootselDrive
$RuntimeRoot = Drive-Root $RuntimeDrive

Pause-IfEnabled "Put the Pico in BOOTSEL mode now."
Write-Host "Waiting for Pico BOOTSEL drive on $BootselRoot (INFO_UF2.TXT / INDEX.HTM)..."
if (-not (Wait-ForBootsel -DriveRoot $BootselRoot -TimeoutSec $TimeoutSec)) {
  throw "Timed out waiting for BOOTSEL mount at $BootselRoot"
}
Write-Host "Found BOOTSEL drive: $BootselRoot"

Write-Host "Copying UF2: $Uf2Path"
Copy-Item -LiteralPath $Uf2Path -Destination $BootselRoot -Force
Write-Host "UF2 copied."

Pause-IfEnabled "Wait for reboot and JAMMA64 runtime drive mount."
Write-Host "Waiting for runtime drive on $RuntimeRoot (WIFI.TXT)..."
if (-not (Wait-ForRuntime -DriveRoot $RuntimeRoot -TimeoutSec $TimeoutSec)) {
  throw "Timed out waiting for runtime mount at $RuntimeRoot"
}
Write-Host "Found runtime drive: $RuntimeRoot"

$WifiFile = Join-Path $RuntimeRoot "WIFI.TXT"
$content = @"
# JAMMA64 Wi-Fi config
# Auto-written by flash_and_wifi.ps1
SSID=$Ssid
PASSWORD=$Password
# Optional: REBOOT=1
"@
[System.IO.File]::WriteAllText($WifiFile, $content, [System.Text.Encoding]::ASCII)
Write-Host "Wrote Wi-Fi config to: $WifiFile"
Write-Host "Done."
