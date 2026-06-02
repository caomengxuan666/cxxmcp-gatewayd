param(
  [string]$Name = "cxxmcp-gatewayd",
  [string]$ExePath = "$PSScriptRoot\..\..\bin\cxxmcp-gatewayd.exe",
  [string]$ConfigPath = "$PSScriptRoot\..\..\share\cxxmcp-gatewayd\examples\gatewayd.example.json"
)

$resolvedExe = Resolve-Path -LiteralPath $ExePath
$resolvedConfig = Resolve-Path -LiteralPath $ConfigPath
$binaryPath = "`"$resolvedExe`" run --config `"$resolvedConfig`""

if (Get-Service -Name $Name -ErrorAction SilentlyContinue) {
  throw "Service '$Name' already exists"
}

New-Service `
  -Name $Name `
  -DisplayName "cxxmcp gateway daemon" `
  -Description "Local MCP middleware daemon" `
  -BinaryPathName $binaryPath `
  -StartupType Manual

Write-Host "Installed service '$Name'"
Write-Host "Start with: Start-Service $Name"
