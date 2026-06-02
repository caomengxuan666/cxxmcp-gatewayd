param(
  [string]$Name = "cxxmcp-gatewayd"
)

$service = Get-Service -Name $Name -ErrorAction SilentlyContinue
if (-not $service) {
  Write-Host "Service '$Name' is not installed"
  exit 0
}

if ($service.Status -ne "Stopped") {
  Stop-Service -Name $Name
}

sc.exe delete $Name | Out-Null
Write-Host "Removed service '$Name'"
