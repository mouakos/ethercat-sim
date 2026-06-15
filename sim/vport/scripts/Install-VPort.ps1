<#
.SYNOPSIS
    Install or uninstall the EtherCAT virtual port driver (vport.sys).

.DESCRIPTION
    Manages the two virtual NICs created by vport.sys:
        NIC-A  "EtherCAT vPort Master"  (ROOT\ETHERCAT_VPORT_MASTER)
        NIC-B  "EtherCAT vPort Slave"   (ROOT\ETHERCAT_VPORT_SLAVE)

    Must be run as Administrator in test-signing mode.
    Enable test-signing once (then reboot):
        bcdedit /set testsigning on

.PARAMETER Action
    Install   — add the driver and create both virtual NICs.
    Uninstall — remove both virtual NICs and uninstall the driver package.
    Status    — list vport devices currently visible to the system.

.PARAMETER DriverDir
    Directory containing vport.sys and vport.inf.
    Defaults to the build output: <repo>\build\sim\vport\

.EXAMPLE
    .\Install-VPort.ps1 -Action Install
    .\Install-VPort.ps1 -Action Status
    .\Install-VPort.ps1 -Action Uninstall
#>

[CmdletBinding()]
param (
    [Parameter(Mandatory)]
    [ValidateSet('Install', 'Uninstall', 'Status')]
    [string]$Action,

    [string]$DriverDir = "$PSScriptRoot\..\..\..\build\sim\vport"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# ── Helpers ──────────────────────────────────────────────────────────────────

function Assert-Admin {
    $id = [System.Security.Principal.WindowsIdentity]::GetCurrent()
    $p  = [System.Security.Principal.WindowsPrincipal]$id
    if (-not $p.IsInRole([System.Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw "This script must be run as Administrator."
    }
}

function Find-DevCon {
    # devcon.exe ships with the WDK; also available via winget as "Microsoft.DevCon"
    $candidates = @(
        "$env:ProgramFiles(x86)\Windows Kits\10\Tools\x64\devcon.exe",
        "$env:ProgramFiles\Windows Kits\10\Tools\x64\devcon.exe",
        (Get-Command devcon.exe -ErrorAction SilentlyContinue)?.Source
    )
    foreach ($c in $candidates) {
        if ($c -and (Test-Path $c)) { return $c }
    }
    throw "devcon.exe not found. Install the WDK or run: winget install Microsoft.DevCon"
}

# ── Main ─────────────────────────────────────────────────────────────────────

Assert-Admin

$infPath = Join-Path $DriverDir 'vport.inf'
$devcon  = Find-DevCon

switch ($Action) {

    'Install' {
        if (-not (Test-Path $infPath)) {
            throw "Driver package not found at '$infPath'. Build the driver first (Build-VPort.ps1)."
        }

        Write-Host "Adding driver package to the driver store..."
        & pnputil /add-driver $infPath /install
        if ($LASTEXITCODE -ne 0) { throw "pnputil failed (exit $LASTEXITCODE)." }

        Write-Host "Creating EtherCAT vPort Master (NIC-A)..."
        & $devcon install $infPath ROOT\ETHERCAT_VPORT_MASTER
        if ($LASTEXITCODE -ne 0) { throw "devcon install MASTER failed." }

        Write-Host "Creating EtherCAT vPort Slave (NIC-B)..."
        & $devcon install $infPath ROOT\ETHERCAT_VPORT_SLAVE
        if ($LASTEXITCODE -ne 0) { throw "devcon install SLAVE failed." }

        Write-Host ""
        Write-Host "Done. Next steps:" -ForegroundColor Green
        Write-Host "  1. TwinCAT XAE → 'Show Real Time Ethernet Compatible Devices'"
        Write-Host "     → select 'EtherCAT vPort Master' → Install"
        Write-Host "  2. Find the Slave NIC GUID:"
        Write-Host "     ec-core --list"
        Write-Host "  3. Run the simulator:"
        Write-Host "     ec-core --serve \Device\NPF_{<GUID-of-Slave>} --slaves 5"
    }

    'Uninstall' {
        Write-Host "Removing EtherCAT vPort Master..."
        & $devcon remove ROOT\ETHERCAT_VPORT_MASTER
        Write-Host "Removing EtherCAT vPort Slave..."
        & $devcon remove ROOT\ETHERCAT_VPORT_SLAVE

        Write-Host "Removing driver package from driver store..."
        & pnputil /delete-driver vport.inf /uninstall /force
        Write-Host "Done." -ForegroundColor Green
    }

    'Status' {
        Write-Host "=== vport devices ==="
        & $devcon status ROOT\ETHERCAT_VPORT_MASTER
        & $devcon status ROOT\ETHERCAT_VPORT_SLAVE
    }
}
