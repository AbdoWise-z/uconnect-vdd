#Requires -RunAsAdministrator
<#
  Undoes scripts\dev-install.ps1: removes the device, the driver package and
  the trusted test certificate.

    powershell -ExecutionPolicy Bypass -File scripts\dev-uninstall.ps1
#>
param(
    [string]$Configuration = 'Release',
    [string]$Platform = 'x64'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$cert = Join-Path $root "$Platform\$Configuration\UConnectVDD.cer"
$vddctl = Join-Path $root 'tools\vddctl\vddctl.exe'

& $vddctl uninstall

if (Test-Path $cert) {
    $thumbprint = (Get-PfxCertificate $cert).Thumbprint
    foreach ($store in 'Root', 'TrustedPublisher') {
        Get-ChildItem "Cert:\LocalMachine\$store" | Where-Object Thumbprint -eq $thumbprint | Remove-Item
    }
    Write-Output "Removed the test certificate $thumbprint."
}
