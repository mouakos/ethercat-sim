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
    # tapinstall.exe ships with OpenVPN and accepts the same arguments as devcon.exe.
    # devcon.exe ships with the WDK under a versioned subfolder:
    #   %ProgramFiles(x86)%\Windows Kits\10\Tools\10.0.XXXXX.0\x64\devcon.exe
    # We use a wildcard search so the script works regardless of WDK version.

    $devconCmd = Get-Command devcon.exe     -ErrorAction SilentlyContinue
    $tapCmd    = Get-Command tapinstall.exe -ErrorAction SilentlyContinue

    $candidates = New-Object System.Collections.Generic.List[string]

    # OpenVPN tapinstall.exe (no WDK needed)
    $candidates.Add("$env:ProgramFiles\OpenVPN\bin\tapinstall.exe")
    $candidates.Add("$env:ProgramFiles\TAP-Windows\bin\tapinstall.exe")
    $candidates.Add("$env:ProgramFiles(x86)\OpenVPN\bin\tapinstall.exe")

    # WDK devcon.exe — versioned path, search with wildcard
    $wdkRoots = @(
        "$env:ProgramFiles(x86)\Windows Kits\10\Tools",
        "$env:ProgramFiles\Windows Kits\10\Tools"
    )
    foreach ($root in $wdkRoots) {
        if (Test-Path $root) {
            $found = Get-ChildItem -Path $root -Filter 'devcon.exe' -Recurse -ErrorAction SilentlyContinue |
                     Where-Object { $_.FullName -like '*x64*' } |
                     Select-Object -First 1
            if ($found) { $candidates.Add($found.FullName) }
        }
    }

    if ($tapCmd)    { $candidates.Add($tapCmd.Source) }
    if ($devconCmd) { $candidates.Add($devconCmd.Source) }

    foreach ($c in $candidates) {
        if ($c -and (Test-Path $c)) { return $c }
    }

    throw ('devcon.exe / tapinstall.exe not found.' + [Environment]::NewLine +
           'The WDK is installed but devcon.exe was not found under Windows Kits\10\Tools.' + [Environment]::NewLine +
           'Try: Get-ChildItem "C:\Program Files (x86)\Windows Kits\10\Tools" -Filter devcon.exe -Recurse')
}

function Find-TapInf {
    $candidates = New-Object System.Collections.Generic.List[string]
    $candidates.Add("$env:ProgramFiles\OpenVPN\driver\OemVista.inf")
    $candidates.Add("$env:ProgramFiles\OpenVPN\driver\tap-windows6.inf")
    $candidates.Add("$env:ProgramFiles(x86)\OpenVPN\driver\OemVista.inf")
    foreach ($c in $candidates) {
        if (Test-Path $c) { return $c }
    }
    $pnp = pnputil /enum-drivers | Select-String -Pattern 'tap'
    if ($pnp) {
        Write-Host 'TAP driver already in driver store.'
        return $null
    }
    throw ('TAP-Windows driver not found. Install OpenVPN first:' + [Environment]::NewLine +
           '  winget install OpenVPN.OpenVPN' + [Environment]::NewLine +
           'Then re-run this script.')
}

Assert-Admin

switch ($Action) {

    'Install' {
        $devcon = Find-DevCon
        $tapInf = Find-TapInf

        Write-Host "Creating 'EtherCAT-Master' TAP adapter..."
        if ($tapInf) {
            & $devcon install $tapInf 'tap0901'
        } else {
            & $devcon install 'tap0901'
        }

        Write-Host "Creating 'EtherCAT-Slave' TAP adapter..."
        if ($tapInf) {
            & $devcon install $tapInf 'tap0901'
        } else {
            & $devcon install 'tap0901'
        }

        Start-Sleep -Seconds 2

        $tapAdapters = Get-NetAdapter |
            Where-Object { ($_.InterfaceDescription -like '*TAP*') -or
                           ($_.InterfaceDescription -like '*tap*') } |
            Sort-Object ifIndex -Descending |
            Select-Object -First 2

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
