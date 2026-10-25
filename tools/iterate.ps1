# Run, and while a run names a guest callback that isn't lifted, add it to
# RUN_SEEDS-to-be (work\run_seeds.json), relift, rebuild and run again.
#   tools\iterate.ps1 [-Script tools\scripts\arcade.txt] [-Seconds 40] [-Tries 8]
param([string]$Script = 'tools\scripts\arcade.txt', [int]$Seconds = 40, [int]$Tries = 8)
$Root = Split-Path -Parent $PSScriptRoot
Set-Location $Root
$exe = if ($env:BUILD_DIR) { "$($env:BUILD_DIR)\hotd.exe" } else { 'build\hotd.exe' }
for ($i = 0; $i -lt $Tries; $i++) {
  & .\tools\drive.ps1 -Script $Script -Out work\iter -Seconds $Seconds -Exe $exe | Out-Null
  $new = Select-String -Path work\iter.log -Pattern 'Windows called guest (0x[0-9A-F]+), which is not lifted|unresolved VA (0x[0-9A-F]+)' |
    ForEach-Object { $_.Matches[0].Groups[1].Value + $_.Matches[0].Groups[2].Value } | Sort-Object -Unique
  if (-not $new) { "clean after $i rounds"; Select-String -Path work\iter.log -Pattern 'fault|frame 1000 presented' | ForEach-Object { $_.Line }; return }
  "round $i new: $($new -join ' ')"
  $seeds = @()
  if (Test-Path work\run_seeds.json) { $seeds = @(Get-Content work\run_seeds.json | ConvertFrom-Json) }
  $seeds = @($seeds + ($new | ForEach-Object { [Convert]::ToInt64($_, 16) }) | Sort-Object -Unique)
  ConvertTo-Json @($seeds) | Set-Content work\run_seeds.json
  py -3 run_lift.py --all | Select-Object -Last 3 | Select-Object -First 1
  & .\build.cmd | Select-Object -Last 1
}
"still finding new ones after $Tries rounds"
