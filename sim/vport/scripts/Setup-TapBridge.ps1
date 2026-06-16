<#
.SYNOPSIS
    Create the two TAP-Windows virtual NICs needed by vport-bridge.

.DESCRIPTION
    The TAP-Windows driver ships with OpenVPN and is signed by the OpenVPN
    Foundation - no test-signing or kernel signing required.

    This script creates two TAP adapter instances:
        "EtherCAT-Master"  - TwinCAT 3 will use this (generic NDIS mode)
        "EtherCAT-Slave"   - ec-core opens this via Npcap

    Prerequisites:
        1. Install OpenVPN (community edition).
           The installer registers and signs the TAP-Windows driver.
        2. Run this script as Administrator.

.PARAMETER Action
    Install   - create the two TAP NICs (default)
    Uninstall - remove them
    Status    - show current TAP adapters

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
        throw 'Run this script as Administrator.'
    }
}

function Find-DevCon {
    # Use Get-Item with path wildcards to find the versioned WDK devcon.exe.
    # Avoids env-var expansion issues with "Program Files (x86)" in PS5.
    $wdkX64_1 = Get-Item 'C:\Program Files (x86)\Windows Kits\10\Tools\*\x64\devcon.exe' `
                    -ErrorAction SilentlyContinue | Select-Object -First 1
    $wdkX64_2 = Get-Item 'C:\Program Files\Windows Kits\10\Tools\*\x64\devcon.exe' `
                    -ErrorAction SilentlyContinue | Select-Object -First 1

    $devconCmd = Get-Command devcon.exe     -ErrorAction SilentlyContinue
    $tapCmd    = Get-Command tapinstall.exe -ErrorAction SilentlyContinue

    $candidates = @(
        'C:\Program Files\OpenVPN\bin\tapinstall.exe',
        'C:\Program Files (x86)\OpenVPN\bin\tapinstall.exe',
        'C:\Program Files\TAP-Windows\bin\tapinstall.exe'
    )
    if ($wdkX64_1) { $candidates += $wdkX64_1.FullName }
    if ($wdkX64_2) { $candidates += $wdkX64_2.FullName }
    if ($tapCmd)   { $candidates += $tapCmd.Source }
    if ($devconCmd){ $candidates += $devconCmd.Source }

    foreach ($c in $candidates) {
        if ($c -and (Test-Path $c)) { return $c }
    }

    $msg = 'devcon.exe not found. Add it to PATH manually:' + [Environment]::NewLine +
           '  copy "C:\Program Files (x86)\Windows Kits\10\Tools\10.0.26100.0\x64\devcon.exe" C:\Windows\System32\'
    throw $msg
}

function Find-TapInf {
    # 1. Look in the OpenVPN installation directory
    $candidates = @(
        'C:\Program Files\OpenVPN\driver\OemVista.inf',
        'C:\Program Files\OpenVPN\driver\tap-windows6.inf',
        'C:\Program Files (x86)\OpenVPN\driver\OemVista.inf',
        'C:\Program Files\TAP-Windows\driver\OemVista.inf'
    )
    foreach ($c in $candidates) {
        if (Test-Path $c) { return $c }
    }

    # 2. Search the driver store for a published TAP INF (oem*.inf containing tap0901)
    Write-Host 'Searching driver store for TAP INF...'
    $oemInf = Get-ChildItem 'C:\Windows\INF\oem*.inf' -ErrorAction SilentlyContinue |
        Select-String -Pattern 'tap0901' -List -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($oemInf) {
        Write-Host ('Found TAP INF in driver store: ' + $oemInf.Path)
        return $oemInf.Path
    }

    throw ('TAP-Windows driver not found. Install OpenVPN first, then re-run this script.')
}

Assert-Admin

switch ($Action) {

    'Install' {
        $devcon = Find-DevCon
        $tapInf = Find-TapInf

        Write-Host "Creating 'EtherCAT-Master' TAP adapter..."
        & $devcon install $tapInf 'tap0901'

        Write-Host "Creating 'EtherCAT-Slave' TAP adapter..."
        & $devcon install $tapInf 'tap0901'

        Start-Sleep -Seconds 2

        $tapAdapters = @(Get-NetAdapter |
            Where-Object { ($_.InterfaceDescription -like '*TAP*') -or
                           ($_.InterfaceDescription -like '*tap*') } |
            Sort-Object ifIndex -Descending |
            Select-Object -First 2)

        if ($tapAdapters.Count -ge 2) {
            Rename-NetAdapter -Name $tapAdapters[0].Name -NewName 'EtherCAT-Slave'
            Rename-NetAdapter -Name $tapAdapters[1].Name -NewName 'EtherCAT-Master'
            Write-Host "Renamed adapters to 'EtherCAT-Master' and 'EtherCAT-Slave'." -ForegroundColor Green
        } else {
            Write-Warning 'Could not auto-rename - rename manually in Network Connections.'
        }

        Write-Host ''
        Write-Host 'Done. Next steps:' -ForegroundColor Green
        Write-Host ''
        Write-Host '  1. Get adapter GUIDs:'
        Write-Host '       vport-bridge --list'
        Write-Host ''
        Write-Host '  2. Start the bridge (keep this window open):'
        Write-Host '       vport-bridge [GUID-EtherCAT-Master] [GUID-EtherCAT-Slave]'
        Write-Host ''
        Write-Host '  3. TwinCAT XAE > EtherCAT Master > Adapter tab'
        Write-Host '       > Search > Show all adapters > EtherCAT-Master > OK'
        Write-Host ''
        Write-Host '  4. Find Slave NPF GUID then run:'
        Write-Host '       ec-core --list'
        Write-Host '       ec-core --serve \\Device\\NPF_[GUID-EtherCAT-Slave] --slaves 5'
    }

    'Uninstall' {
        $devcon = Find-DevCon
        Write-Host 'Removing EtherCAT TAP adapters...'
        & $devcon remove 'tap0901'
        Write-Host 'Done.' -ForegroundColor Green
    }

    'Status' {
        Write-Host '=== TAP-Windows adapters on this machine ==='
        Get-NetAdapter | Where-Object {
            ($_.InterfaceDescription -like '*TAP*') -or
            ($_.Name -like 'EtherCAT*')
        } | Format-Table Name, InterfaceDescription, Status, MacAddress
    }
}
