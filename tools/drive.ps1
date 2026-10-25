# Run the host with a timeline of input-mailbox commands, for headless checks.
#
#   tools\drive.ps1 -Script tools\scripts\arcade.txt -Out work\run [-Seconds 50] [-- extra host args]
#
# The script has one command a line: "<seconds> <mailbox command>", seconds
# from the start of the run ("10 0x20 400" holds Space for 400 ms at 10 s,
# "14 m 318 220" moves the cursor, "15 0x01 200" clicks). Lines starting with
# # are comments. Leaves <Out>.log and <Out>.mp4.
[CmdletBinding(PositionalBinding = $false)]
param([Parameter(Mandatory)][string]$Script, [Parameter(Mandatory)][string]$Out, [int]$Seconds = 50,
      [string]$Exe = 'build\hotd.exe', [Parameter(ValueFromRemainingArguments)][string[]]$Rest)
$Root = Split-Path -Parent $PSScriptRoot
Set-Location $Root
if (-not [IO.Path]::IsPathRooted($Out)) { $Out = Join-Path $Root $Out }
$pad = "$Out.pad"
Set-Content $pad '' -NoNewline
$steps = Get-Content $Script | Where-Object { $_ -match '^\s*[\d.]+\s+\S' }
$job = Start-Job -ArgumentList $pad, $steps -ScriptBlock {
  param($p, $steps)
  $t0 = Get-Date
  foreach ($s in $steps) {
    $at, $cmd = $s.Trim() -split '\s+', 2
    while (((Get-Date) - $t0).TotalSeconds -lt [double]$at) { Start-Sleep -Milliseconds 20 }
    while ((Get-Item $p).Length -gt 0) { Start-Sleep -Milliseconds 20 }   # the last one landed
    Set-Content $p $cmd -NoNewline
  }
}
$hostArgs = @('--run', '--headless', '--watchdog', "$Seconds", '--pad', $pad, '--record', "$Out.mp4") + $Rest
# Through cmd, so the log is the program's own bytes (PowerShell 5's > writes UTF-16).
$line = (@($Exe -replace '/', '\') + $hostArgs | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }) -join ' '
cmd /d /c "$line > `"$Out.log`" 2>&1"
$code = $LASTEXITCODE
Stop-Job $job; Remove-Job $job
Remove-Item $pad -ErrorAction SilentlyContinue
"exit $code"
