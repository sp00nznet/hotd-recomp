# One-click setup for the House of the Dead static recompilation. Run it by
# double-clicking Setup.cmd in the repo folder; the README's "Step by step" is
# the same thing by hand.
#
# It checks the tools, copies your copy of the game into game\, finds every
# function, lifts thotd.exe to C, builds build\hotd.exe and leaves a launcher
# beside this folder's README. Each step is skipped when its output already
# exists (-Force redoes them), so a rerun after a failure resumes. Everything
# it does goes to setup.log.
#
#   -GameDir <folder or .zip>   the installed game (the folder with thotd.exe); -Game works too
#   -Yes                        answer yes to every install question
#
# The toolkit is ..\pcrecomp (PCRECOMP overrides it). Under recomp-launcher
# (RECOMP_LAUNCHER=1) it never waits for Enter, and exit code 0 means the
# launcher is there.
param([switch]$Force, [switch]$Yes, [Alias('Game')][string]$GameDir = "")
$Game = $GameDir

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$Toolkit = $env:PCRECOMP
if (-not $Toolkit) { $Toolkit = Join-Path (Split-Path -Parent $Root) 'pcrecomp' }
$env:PCRECOMP = $Toolkit   # the Python steps read it too
$Log = Join-Path $Root 'setup.log'
Set-Location $Root
function Log($t) { Add-Content -Path $Log -Value $t -Encoding UTF8 }
Log "==== setup $(Get-Date -Format s)"

function Say($t, $c = 'Gray') { Write-Host $t -ForegroundColor $c; Log $t }
function Step($n, $t) { Write-Host ""; Say "[$n/6] $t" 'Cyan' }
function Pause-End { if (-not $Yes -and $env:RECOMP_LAUNCHER -ne '1') { Read-Host "Press Enter to close" | Out-Null } }
function Fail($t) {
  Say ""; Say "Setup stopped: $t" 'Red'
  Say "The details are in $Log. Fix that and run Setup.cmd again; finished steps are skipped." 'Yellow'
  Pause-End; exit 1
}
trap { Fail "$_" }
function Ask($q) { if ($Yes) { Say "$q yes"; return $true }; $a = Read-Host "$q [Y/n]"; return -not ($a -match '^[nN]') }
function Refresh-Path {
  $env:Path = [Environment]::GetEnvironmentVariable('Path', 'Machine') + ';' + [Environment]::GetEnvironmentVariable('Path', 'User')
}
# Run a program with its output in the log. Windows PowerShell turns a native
# program's stderr into errors, so 'Stop' is off while it runs.
function Exec([string[]]$cmd) {
  $old = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
  $rest = @($cmd | Select-Object -Skip 1)   # @(): a one-element slice would splat as characters
  & $cmd[0] @rest 2>&1 | ForEach-Object { Log "$_" }
  $code = $LASTEXITCODE
  $ErrorActionPreference = $old
  return $code
}
function Run($what, [string[]]$cmd) {
  Say "  $what..."
  $code = Exec $cmd
  if ($code -ne 0) { Fail "$what failed (exit code $code)." }
}

if ($env:RECOMP_LAUNCHER -ne '1') { Clear-Host }
Say "The House of the Dead: static recompilation setup" 'White'
Say "You need your own copy of the game (the installed folder with thotd.exe, or a .zip of it) and about 2 GB free."

# ---------------------------------------------------------------- tools
Step 1 "Checking the tools"
# A Python that answers "Python 3.x". The Store's placeholder "python" (the
# one that opens the Store) answers nothing, so asking is the reliable test.
function Find-Python {
  $user = @(Get-ChildItem "$env:LOCALAPPDATA\Programs\Python\Python3*\python.exe" -ErrorAction SilentlyContinue | Sort-Object FullName -Descending | ForEach-Object { ,@($_.FullName) })
  foreach ($c in (@(@('py', '-3'), @('python'), @('python3')) + $user)) {
    if (-not (Get-Command $c[0] -ErrorAction SilentlyContinue)) { continue }
    $old = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    $rest = @($c | Select-Object -Skip 1)
    $v = (& $c[0] @rest --version 2>&1 | Out-String).Trim()
    $ErrorActionPreference = $old
    if ($v -match '^Python 3\.(\d+)' -and [int]$Matches[1] -ge 10) { return ,$c }
  }
  return $null
}
$pyargs = Find-Python
if (-not $pyargs) {
  Say "  Python 3.10 or newer is not installed."
  if (-not (Ask "  Install Python 3.12 now (winget, for your user only, about 30 MB)?")) { Fail "Python 3 is required." }
  if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
    Fail "winget is missing. Install 'App Installer' from the Microsoft Store, or install Python yourself."
  }
  Exec @('winget', 'install', '-e', '--id', 'Python.Python.3.12', '--scope', 'user', '--accept-package-agreements', '--accept-source-agreements') | Out-Null
  Refresh-Path
  $pyargs = Find-Python
  if (-not $pyargs) { Fail "Python installed, but it can't be found. Install Python 3.12 from python.org yourself." }
}
Say "  Python: $($pyargs -join ' ')"

if ((Exec ($pyargs + @('-c', 'import capstone'))) -ne 0) {
  Say "  The Python package capstone is missing (about 10 MB)."
  if (-not (Ask "  Install it now (pip, for your user only)?")) { Fail "capstone is required." }
  Run "Installing capstone" ($pyargs + @('-m', 'pip', 'install', '--user', 'capstone'))
}

if (-not (Test-Path (Join-Path $Toolkit 'tools\lift\lift32.py'))) {
  Say "  The pcrecomp toolkit is not beside this folder ($Toolkit)."
  if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
    if (-not (Ask "  git is needed to fetch it. Install Git for Windows now (winget, about 60 MB)?")) { Fail "git is needed to fetch pcrecomp." }
    if (-not (Get-Command winget -ErrorAction SilentlyContinue)) { Fail "winget is missing. Install Git for Windows yourself." }
    Exec @('winget', 'install', '-e', '--id', 'Git.Git', '--accept-package-agreements', '--accept-source-agreements') | Out-Null
    Refresh-Path
    if (-not (Get-Command git -ErrorAction SilentlyContinue)) { Fail "Git did not install. Install Git for Windows yourself." }
  }
  if (-not (Ask "  Download it now (git, about 20 MB)?")) { Fail "pcrecomp is required at $Toolkit." }
  Run "Cloning pcrecomp" @('git', 'clone', '--depth', '1', 'https://github.com/sp00nznet/pcrecomp', $Toolkit)
}
# The toolkit changes this game needs, each by a line it adds.
$need = @(
  @('pcrecomp#129 (zero fill is not a function)', 'tools\disasm\disasm32.py', 'zero fill is not code'),
  @('pcrecomp#130 (guest setjmp/longjmp)', 'runtime\native32\eh32.c', 'recomp_eh_resume'))
function Missing { @($need | Where-Object {
  $f = Join-Path $Toolkit $_[1]
  -not ((Test-Path $f) -and (Select-String -Path $f -Pattern $_[2] -SimpleMatch -Quiet)) } | ForEach-Object { $_[0] }) }
$missing = Missing
$head = Join-Path $Toolkit '.git\HEAD'
if ($missing -and (Test-Path $head) -and (Get-Content $head) -eq 'ref: refs/heads/main') {
  if (Ask "  Your pcrecomp is missing $($missing -join ', '). Update it (git pull)?") {
    Run "Updating pcrecomp" @('git', '-C', $Toolkit, 'pull', '--ff-only')
    $missing = Missing
  }
}
if ($missing) {
  Fail "your pcrecomp at $Toolkit is missing $($missing -join ', '). Update it (git -C `"$Toolkit`" pull), or point PCRECOMP at a checkout that has them."
}
Say "  pcrecomp: $Toolkit"

# Visual Studio 2022 or its Build Tools, with the C++ x86 compiler; CMake and
# Ninja ship inside that workload, so build.cmd needs nothing else.
function Find-VS {
  foreach ($p in @("${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe",
                   "$env:ProgramFiles\Microsoft Visual Studio\Installer\vswhere.exe")) {
    if (Test-Path $p) {
      $vs = & $p -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
      if ($vs) { return $vs }
    }
  }
  return $null
}
if (-not (Find-VS)) {
  Say "  Visual Studio 2022 with the C++ tools is not installed. The recompiled game is C, and this is the compiler."
  if (-not (Ask "  Install the Visual Studio 2022 Build Tools with the C++ workload now (winget, about 3 GB)?")) { Fail "a C compiler (Visual Studio 2022 C++) is required." }
  if (-not (Get-Command winget -ErrorAction SilentlyContinue)) { Fail "winget is missing. Install Visual Studio 2022 Build Tools with 'Desktop development with C++' yourself." }
  Exec @('winget', 'install', '-e', '--id', 'Microsoft.VisualStudio.2022.BuildTools', '--accept-package-agreements', '--accept-source-agreements',
         '--override', '--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended') | Out-Null
  if (-not (Find-VS)) { Fail "the Build Tools did not install. Install Visual Studio 2022 with 'Desktop development with C++' yourself." }
}
Say "  C compiler: $(Find-VS)"

# ---------------------------------------------------------------- the game
Step 2 "Copying your copy of the game into game\"
if ((Test-Path 'game\thotd.exe') -and (Test-Path 'work\thotd.exe') -and -not $Force) {
  Say "  Already there (skipping)."
} else {
  $Game = $Game.Trim('"', ' ')
  if (-not $Game -and $Yes) { Fail "The game wasn't given. Pass its folder (the one with thotd.exe) or a .zip of it with -GameDir." }
  while (-not $Game) {
    $Game = (Read-Host "  Where is The House of the Dead? The folder with thotd.exe, or a .zip of it").Trim('"', ' ')
  }
  if ($Game -match '\.zip$') {
    $tmp = Join-Path $Root 'work\unzip'
    if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
    Say "  Unpacking $Game..."
    Expand-Archive -Path $Game -DestinationPath $tmp
    $exe = Get-ChildItem $tmp -Recurse -Filter 'thotd.exe' | Select-Object -First 1
    if (-not $exe) { Fail "$Game has no thotd.exe in it." }
    $Game = $exe.DirectoryName
  } elseif (-not (Test-Path (Join-Path $Game 'thotd.exe'))) {
    $exe = Get-ChildItem $Game -Recurse -Filter 'thotd.exe' -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $exe) { Fail "there is no thotd.exe in $Game." }
    $Game = $exe.DirectoryName
  }
  if (-not (Test-Path (Join-Path $Game 'art'))) { Fail "$Game has thotd.exe but no art folder: it needs the installed game's whole folder." }
  New-Item -ItemType Directory -Force 'game', 'work' | Out-Null
  Say "  Copying the game from $Game (about 125 MB)..."
  $code = Exec @('robocopy', $Game, (Join-Path $Root 'game'), '/E', '/NFL', '/NDL', '/NJH', '/NJS', '/NP')
  if ($code -ge 8) { Fail "copying the game failed (robocopy $code)." }
  Copy-Item (Join-Path $Game 'thotd.exe') 'work\thotd.exe' -Force
  if (Test-Path (Join-Path $Root 'work\unzip')) { Remove-Item -Recurse -Force (Join-Path $Root 'work\unzip') }
}

# ---------------------------------------------------------------- catalog
Step 3 "Finding every function (about 3 minutes)"
if ((Test-Path 'work\functions.json') -and -not $Force) { Say "  Already done (skipping)." }
else { Run "Disassembling thotd.exe" ($pyargs + @("$Toolkit\tools\disasm\disasm32.py", 'work\thotd.exe', '-o', 'work\functions.json')) }

# ---------------------------------------------------------------- lift
Step 4 "Recompiling thotd.exe to C (about a minute)"
if ((Test-Path 'src\recomp\gen\recomp_dispatch.c') -and -not $Force) { Say "  Already done (skipping)." }
else { Run "Lifting" ($pyargs + @('run_lift.py', '--all')) }

# ---------------------------------------------------------------- build
Step 5 "Building build\hotd.exe (5 to 15 minutes)"
if ((Test-Path 'build\hotd.exe') -and -not $Force) { Say "  Already built (skipping)." }
else {
  $env:CMAKE_ARGS = "-DPCRECOMP=$($Toolkit -replace '\\', '/')"
  Run "Compiling" @('cmd', '/d', '/c', "`"$Root\build.cmd`"")
}

# ---------------------------------------------------------------- launcher
Step 6 "Making the launcher"
$cmdf = Join-Path $Root 'The House of the Dead.cmd'
Set-Content -Path $cmdf -Encoding ASCII -Value "@echo off`r`ncd /d `"%~dp0`"`r`nstart `"`" build\hotd.exe --run %*`r`n"
Say "  $cmdf"
if (-not (Test-Path $cmdf)) { Fail "the launcher was not made." }
Write-Host ""
Say "Done. Double-click 'The House of the Dead' in this folder to play. F10 in the game has the cheats and online play." 'Green'
Pause-End
exit 0
