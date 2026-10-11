#Requires -RunAsAdministrator
<#
  Installs a locally built, test-signed driver on this machine, for
  development. Run from an elevated PowerShell:

    powershell -ExecutionPolicy Bypass -File scripts\dev-install.ps1

  It trusts the build's test certificate (LocalMachine Root and
  TrustedPublisher), so Windows accepts the catalog without test-signing
  mode, then creates the device and installs the driver with vddctl.
  scripts\dev-uninstall.ps1 undoes all of it.
#>
param(
    [string]$Configuration = 'Release',
    [string]$Platform = 'x64'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$package = Join-Path $root "$Platform\$Configuration\UConnectVDD"
$cert = Join-Path $root "$Platform\$Configuration\UConnectVDD.cer"
$vddctl = Join-Path $root 'tools\vddctl\vddctl.exe'

foreach ($path in @("$package\UConnectVDD.inf", "$package\uconnectvdd.cat", $cert, $vddctl)) {
    if (-not (Test-Path $path)) { throw "Missing ${path}: build the driver and vddctl first." }
}

foreach ($store in 'Root', 'TrustedPublisher') {
    Import-Certificate -FilePath $cert -CertStoreLocation "Cert:\LocalMachine\$store" | Out-Null
}
Write-Output "Trusted the test certificate $((Get-PfxCertificate $cert).Thumbprint)."

& $vddctl install "$package\UConnectVDD.inf"
if ($LASTEXITCODE -ne 0) { throw "vddctl install failed ($LASTEXITCODE)." }

Get-PnpDevice -Class Display | Where-Object { $_.HardwareID -contains 'Root\uConnectVDD' } |
    Format-Table Status, FriendlyName, InstanceId -AutoSize
