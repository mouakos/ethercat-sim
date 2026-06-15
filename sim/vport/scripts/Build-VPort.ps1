<#
.SYNOPSIS
    Build vport.sys using the WDK + MSBuild toolchain.

.DESCRIPTION
    Creates a minimal WDK driver project on-the-fly, compiles vport.c,
    and outputs vport.sys + vport.inf to build\sim\vport\.

    Requirements:
        - Windows Driver Kit (WDK) for Windows 11 installed
          (download: https://learn.microsoft.com/windows-hardware/drivers/download-the-wdk)
        - Visual Studio 2022 or 2026 with the WDK extension
        - Run from a "Developer PowerShell for VS 2026" (or set VSINSTALLDIR)

    Test-signing must be enabled before installing the built driver:
        bcdedit /set testsigning on    # then reboot

.PARAMETER Config
    Debug (default) or Release.

.PARAMETER WdkRoot
    Override the WDK installation root (auto-detected from registry if omitted).
#>

[CmdletBinding()]
param (
    [ValidateSet('Debug', 'Release')]
    [string]$Config = 'Debug',

    [string]$WdkRoot = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# ── Locate WDK ───────────────────────────────────────────────────────────────

if (-not $WdkRoot) {
    $regPath = 'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots'
    $WdkRoot = (Get-ItemProperty $regPath -ErrorAction Stop).KitsRoot10
}
if (-not (Test-Path $WdkRoot)) {
    throw "WDK not found at '$WdkRoot'. Install the WDK or pass -WdkRoot <path>."
}
Write-Host "WDK: $WdkRoot"

# ── Paths ─────────────────────────────────────────────────────────────────────

$repoRoot  = Resolve-Path "$PSScriptRoot\..\..\..\"
$srcDir    = Join-Path $repoRoot 'sim\vport'
$outDir    = Join-Path $repoRoot "build\sim\vport"
$projFile  = Join-Path $outDir   'vport.vcxproj'

New-Item -ItemType Directory -Force -Path $outDir | Out-Null

# ── Generate a minimal WDK .vcxproj ──────────────────────────────────────────
# The WDK MSBuild integration is loaded via the WDK .props/.targets files.
# We generate the project rather than checking it in (matches repo etiquette).

$wdkProps   = Join-Path $WdkRoot 'build\10.0.26100.0\bin\wdk.props'
$wdkTargets = Join-Path $WdkRoot 'build\10.0.26100.0\bin\wdk.targets'

# Fallback: find any available WDK build props
if (-not (Test-Path $wdkProps)) {
    $wdkBuilds = Get-ChildItem (Join-Path $WdkRoot 'build') -Filter 'wdk.props' -Recurse |
                     Sort-Object -Descending | Select-Object -First 1
    if ($wdkBuilds) {
        $wdkProps   = $wdkBuilds.FullName
        $wdkTargets = $wdkBuilds.FullName -replace 'wdk\.props$','wdk.targets'
    }
}

$vcxproj = @"
<?xml version="1.0" encoding="utf-8"?>
<Project DefaultTargets="Build" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">

  <ItemGroup Label="ProjectConfigurations">
    <ProjectConfiguration Include="${Config}|x64">
      <Configuration>${Config}</Configuration>
      <Platform>x64</Platform>
    </ProjectConfiguration>
  </ItemGroup>

  <PropertyGroup Label="Globals">
    <ProjectGuid>{A1B2C3D4-E5F6-7890-ABCD-EF1234567890}</ProjectGuid>
    <TargetName>vport</TargetName>
    <ConfigurationType>Driver</ConfigurationType>
    <DriverType>NDIS</DriverType>
  </PropertyGroup>

  <Import Project="`$(VCTargetsPath)\Microsoft.Cpp.Default.props" />

  <PropertyGroup Condition="'`$(Configuration)|`$(Platform)'=='${Config}|x64'">
    <DriverType>NDIS</DriverType>
    <TargetOsVersion>10.0.26100</TargetOsVersion>
    <PlatformToolset>WindowsKernelModeDriver10.0</PlatformToolset>
  </PropertyGroup>

  <Import Project="`$(VCTargetsPath)\Microsoft.Cpp.props" />
  <Import Project="$wdkProps" Condition="Exists('$wdkProps')" />

  <ItemDefinitionGroup>
    <ClCompile>
      <AdditionalIncludeDirectories>$srcDir;%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories>
      <PreprocessorDefinitions>NDIS_MINIPORT_DRIVER;_NDIS_=1;%(PreprocessorDefinitions)</PreprocessorDefinitions>
      <WarningLevel>Level4</WarningLevel>
      <!-- Treat warnings as errors except W4127 (constant conditional) -->
      <TreatWarningAsError>true</TreatWarningAsError>
    </ClCompile>
    <Link>
      <AdditionalDependencies>ndis.lib;%(AdditionalDependencies)</AdditionalDependencies>
    </Link>
  </ItemDefinitionGroup>

  <ItemGroup>
    <ClCompile Include="$srcDir\vport.c" />
  </ItemGroup>

  <ItemGroup>
    <None Include="$srcDir\vport.h" />
    <None Include="$srcDir\vport.inf" />
  </ItemGroup>

  <Import Project="`$(VCTargetsPath)\Microsoft.Cpp.targets" />
  <Import Project="$wdkTargets" Condition="Exists('$wdkTargets')" />

</Project>
"@

$vcxproj | Set-Content -Path $projFile -Encoding UTF8
Write-Host "Generated: $projFile"

# ── Build ─────────────────────────────────────────────────────────────────────

$msbuild = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" `
    -latest -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe |
    Select-Object -First 1

if (-not $msbuild -or -not (Test-Path $msbuild)) {
    # Fallback: search common paths
    $msbuild = Get-Command msbuild.exe -ErrorAction SilentlyContinue | Select-Object -Expand Source
}
if (-not $msbuild) {
    throw "MSBuild not found. Open a 'Developer PowerShell for VS 2026'."
}

Write-Host "Building with MSBuild ($Config | x64)..."
& $msbuild $projFile /p:Configuration=$Config /p:Platform=x64 `
    /p:OutDir="$outDir\\" /p:IntDir="$outDir\obj\\" /nologo /m

if ($LASTEXITCODE -ne 0) {
    throw "MSBuild failed (exit $LASTEXITCODE)."
}

# Copy INF next to the .sys for easy pnputil install
Copy-Item "$srcDir\vport.inf" -Destination $outDir -Force

Write-Host ""
Write-Host "Build succeeded:" -ForegroundColor Green
Write-Host "  $outDir\vport.sys"
Write-Host "  $outDir\vport.inf"
Write-Host ""
Write-Host "Install with (elevated PowerShell):"
Write-Host "  .\sim\vport\scripts\Install-VPort.ps1 -Action Install"
