# The one-shot collector is now the live agent. Keep this file beside sentinel-agent.ps1.
$agent = Join-Path $PSScriptRoot "sentinel-agent.ps1"
if (-not (Test-Path $agent)) {
  Write-Host "Download sentinel-agent.ps1 into this folder, then run it."
  exit 1
}
& $agent
