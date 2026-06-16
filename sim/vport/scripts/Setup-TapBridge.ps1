<#
.SYNOPSIS
    Create the two TAP-Windows virtual NICs needed by vport-bridge.

.DESCRIPTION
    The TAP-Windows driver ships with OpenVPN and is signed by the OpenVPN
    Foundation — no test-signing or kernel signing required.

    This script creates two TAP adapter instances:
        "EtherCAT-Master"  — TwinCAT 3 will use this (generic NDIS mode)
        "EtherCAT-Slave"   — ec-core opens this via Npcap

    Prerequisites:
        1. Install OpenVPN (community edition).
           The installer registers and signs the TAP-Windows driver.
        2. Run this script as Administrator.

.PARAMETER Action
    Install   — create the two TAP NICs (default)
    Uninstall — remove them
    Status    — show current TAP adapters

.EXAMPLE
    .\Setup-TapBridge.ps1 -Action Install
    .\Setup-TapBridge.ps1 -Action Status
#>

[CmdletBinding()]
param(
    [ValidateSet('Install','Uninstall','Status')]
    [string]$Action = 'Install'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Assert-Admin {
    $id = [System.Security.Principal.WindowsIdentity]::GetCurrent()
    $p  = [System.Security.Principal.WindowsPrincipal]$id
    if (-not $p.IsInRole([System.Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw "Run this script as Administrator."
    }
}

function Find-DevCon {
    # ?.Source is PS7-only; use explicit null-check for PS5 compatibility
    $devconCmd = Get-Command devcon.exe -ErrorAction SilentlyContinue
    $candidates = @(
        "$env:ProgramFiles(x86)\Windows Kits\10\Tools\x64\devcon.exe",
        "$env:ProgramFiles\Windows Kits\10\Tools\x64\devcon.exe"
    )
    if ($devconCmd) { $candidates += $devconCmd.Source }
    foreach ($c in $candidates) {
        if ($c -and (Test-Path $c)) { return $c }
    }
    throw "devcon.exe not found. Install the WDK or: winget install Microsoft.DevCon"
}

function Find-TapInf {
    # OpenVPN installs the TAP driver INF in its driver directory
    $candidates = @(
        "$env:ProgramFiles\OpenVPN\driver\OemVista.inf",
        "$env:ProgramFiles\OpenVPN\driver\tap-windows6.inf",
        "$env:ProgramFiles(x86)\OpenVPN\driver\OemVista.inf"
    )
    foreach ($c in $candidates) {
        if (Test-Path $c) { return $c }
    }
    # Try pnputil to find already-registered tap driver
    $pnp = pnputil /enum-drivers | Select-String -Pattern "tap" -Context 0,5
    if ($pnp) {
        Write-Host "TAP driver found in driver store via pnputil."
        return $null   # already in store, devcon can use hw id directly
    }
    throw @"
TAP-Windows driver not found. Install OpenVPN first:
  winget install OpenVPN.OpenVPN
Then re-run this script.
"@
}

Assert-Admin

switch ($Action) {

    'Install' {
        $devcon = Find-DevCon
        $tapInf = Find-TapInf   # may be $null if already in driver store

        Write-Host "Creating 'EtherCAT-Master' TAP adapter (master side for TwinCAT)..."
        if ($tapInf) {
            & $devcon install $tapInf "tap0901"
        } else {
            & $devcon install tap0901
        }

        Write-Host "Creating 'EtherCAT-Slave' TAP adapter (slave side for ec-core)..."
        if ($tapInf) {
            & $devcon install $tapInf "tap0901"
        } else {
            & $devcon install tap0901
        }

        # Rename the two newly created adapters via netsh
        # devcon creates them as "Local Area Connection N" or "Ethernet N"
        # Find the two most recently created TAP adapters and rename them.
        Start-Sleep -Seconds 2   # give PnP time to finish

        $tapAdapters = Get-NetAdapter |
            Where-Object { $_.InterfaceDescription -like "*TAP*" -or
                           $_.InterfaceDescription -like "*tap*" } |
            Sort-Object ifIndex -Descending |
            Select-Object -First 2

        if ($tapAdapters.Count -ge 2) {
            Rename-NetAdapter -Name $tapAdapters[0].Name -NewName "EtherCAT-Slave"
            Rename-NetAdapter -Name $tapAdapters[1].Name -NewName "EtherCAT-Master"
            Write-Host "Renamed adapters: 'EtherCAT-Master' and 'EtherCAT-Slave'" -ForegroundColor Green
        } else {
            Write-Warning "Could not auto-rename — rename manually in Network Connections."
        }

        Write-Host ""
        Write-Host "Done. Next steps:" -ForegroundColor Green
        Write-Host ""
        Write-Host "  1. Get the adapter GUIDs:"
        Write-Host "       vport-bridge --list"
        Write-Host ""
        Write-Host "  2. Start the bridge (keep this window open):"
        Write-Host "       vport-bridge <GUID-EtherCAT-Master> <GUID-EtherCAT-Slave>"
        Write-Host ""
        Write-Host "  3. TwinCAT XAE → EtherCAT Master → Adapter tab"
        Write-Host "       → Search → Show all adapters → 'EtherCAT-Master' → OK"
        Write-Host ""
        Write-Host "  4. Find Slave GUID for Npcap:"
        Write-Host "       ec-core --list"
        Write-Host "       ec-core --serve \Device\NPF_<GUID-EtherCAT-Slave> --slaves 5"
    }

    'Uninstall' {
        $devcon = Find-DevCon
        Write-Host "Removing EtherCAT TAP adapters..."
        & $devcon remove "@ROOT\NET\*" "EtherCAT-Master" 2>$null
        & $devcon remove "@ROOT\NET\*" "EtherCAT-Slave"  2>$null
        Write-Host "Done." -ForegroundColor Green
    }

    'Status' {
        Write-Host "=== TAP-Windows adapters on this machine ==="
        Get-NetAdapter | Where-Object {
            $_.InterfaceDescription -like "*TAP*" -or
            $_.Name -like "EtherCAT*"
        } | Format-Table Name, InterfaceDescription, Status, MacAddress
    }
}
