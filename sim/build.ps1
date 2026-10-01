# Build the SimDLL on Windows: the PowerShell equivalent of build.sh, beside it.
# This does NOT install it over the game.
#
#   powershell -ExecutionPolicy Bypass -File sim\build.ps1 -OniGame "C:\...\OxygenNotIncluded"
#
# Needs a MinGW-w64 g++ on PATH (WinLibs: winget install BrechtSanders.WinLibs.POSIX.UCRT) and,
# for the first build only, Python 3 and the game's install folder (ONI_GAME or -OniGame).
[CmdletBinding()]
param(
  # The Oxygen Not Included install folder, the one that contains OxygenNotIncluded_Data.
  # Overrides the ONI_GAME environment variable.
  [string]$OniGame
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
# ---------------------------------------------------------------- helpers (Windows PowerShell 5.1)

function Fail([string]$Message) {
  [Console]::Error.WriteLine("build.ps1: $Message")
  exit 1
}

# Run a native program, passing its stdout through and its stderr to stderr as plain text, and
# stop on a non-zero exit code. ErrorActionPreference is relaxed for the call itself because
# Windows PowerShell 5.1 turns a native program's stderr lines into errors when output is
# redirected, which would stop the build at the first compiler warning.
function Invoke-Native([string]$What, [string]$Exe, [string[]]$Arguments) {
  $saved = $ErrorActionPreference
  $ErrorActionPreference = 'Continue'
  try {
    & $Exe @Arguments 2>&1 | ForEach-Object {
      if ($_ -is [System.Management.Automation.ErrorRecord]) { [Console]::Error.WriteLine($_.ToString()) }
      else { $_ }
    }
    $code = $LASTEXITCODE
  } finally {
    $ErrorActionPreference = $saved
  }
  if ($code -ne 0) { Fail "$What failed (exit code $code)" }
}

# Write text exactly: UTF-8 without a byte order mark, and only the line endings in $Text.
function Write-Exact([string]$Path, [string]$Text) {
  $full = Join-Path (Get-Location).ProviderPath $Path
  [System.IO.File]::WriteAllText($full, $Text, (New-Object System.Text.UTF8Encoding $false))
}

# The game folder from -OniGame or ONI_GAME, checked to be the one holding OxygenNotIncluded_Data.
function Get-OniGame([string]$Given) {
  $game = $Given
  if (-not $game) { $game = $env:ONI_GAME }
  if (-not $game) {
    Fail ("ONI_GAME is not set. Pass -OniGame or set `$env:ONI_GAME to your Oxygen Not Included " +
          "install folder, the one that contains OxygenNotIncluded_Data (for Steam usually " +
          "C:\Program Files (x86)\Steam\steamapps\common\OxygenNotIncluded).")
  }
  $game = $game.TrimEnd('\', '/')
  if (-not (Test-Path -LiteralPath (Join-Path $game 'OxygenNotIncluded_Data') -PathType Container)) {
    Fail ("'$game' has no OxygenNotIncluded_Data folder. ONI_GAME must be the Oxygen Not Included " +
          "install folder, the one that contains OxygenNotIncluded_Data.")
  }
  return $game
}

# Python 3.8 or later: 'py -3' if it works, else 'python'. A python.exe under WindowsApps that
# does not run is the Microsoft Store placeholder, not an installed Python.
function Find-Python {
  $tries = @()
  if (Get-Command py -CommandType Application -ErrorAction SilentlyContinue) { $tries += , @('py', '-3') }
  if (Get-Command python -CommandType Application -ErrorAction SilentlyContinue) { $tries += , @('python') }
  foreach ($t in $tries) {
    $exe = $t[0]
    $pre = @($t | Select-Object -Skip 1)
    $saved = $ErrorActionPreference
    $v = $null
    $code = 1
    $ErrorActionPreference = 'Continue'
    try {
      $v = & $exe @pre -c "import sys; print('%d %d' % sys.version_info[:2])" 2>$null
      $code = $LASTEXITCODE
    } catch {
      $code = 1
    } finally {
      $ErrorActionPreference = $saved
    }
    if ($code -eq 0 -and "$v" -match '^(\d+) (\d+)$') {
      if ([int]$Matches[1] -eq 3 -and [int]$Matches[2] -ge 8) { return , $t }
    }
  }
  $p = Get-Command python -CommandType Application -ErrorAction SilentlyContinue
  if ($p -and $p.Source -like '*\WindowsApps\*') {
    Fail ("'python' on PATH is the Microsoft Store placeholder ($($p.Source)), not an installed " +
          "Python. Install Python with:  winget install Python.Python.3.12  and open a new PowerShell window.")
  }
  Fail ("Python 3.8 or later was not found ('py -3' and 'python' both failed). Install it with:  " +
        "winget install Python.Python.3.12  and open a new PowerShell window.")
}

# A MinGW-w64 g++ that targets 64-bit Windows: x86_64-w64-mingw32-g++ if present, else g++.
function Find-Gxx {
  foreach ($name in @('x86_64-w64-mingw32-g++', 'g++')) {
    $c = Get-Command $name -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $c) { continue }
    $m = $null
    $code = 1
    $saved = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { $m = & $c.Source -dumpmachine 2>$null; $code = $LASTEXITCODE } finally { $ErrorActionPreference = $saved }
    if ($code -eq 0 -and "$m" -match '^x86_64-.*mingw') { return $c.Source }
    Fail ("$($c.Source) targets '$m', not 64-bit Windows (x86_64-w64-mingw32). Install WinLibs:  " +
          "winget install BrechtSanders.WinLibs.POSIX.UCRT  and put its mingw64\bin first on PATH.")
  }
  Fail ("No g++ on PATH. Install WinLibs:  winget install BrechtSanders.WinLibs.POSIX.UCRT  and " +
        "open a new PowerShell window.")
}

# Hand the arguments to g++ in a response file (@file), so quotes and spaces reach the compiler
# unchanged: Windows PowerShell 5.1 strips the quotes inside -DNAME="value" on a command line.
function Invoke-Gxx([string]$Gxx, [string]$ResponseFile, [string[]]$Arguments) {
  $lines = foreach ($a in $Arguments) { '"' + (($a -replace '\\', '\\') -replace '"', '\"') + '"' }
  Write-Exact $ResponseFile (($lines -join "`n") + "`n")
  Invoke-Native 'g++' $Gxx @('@' + $ResponseFile)
}

# ---------------------------------------------------------------- build

Push-Location -LiteralPath $PSScriptRoot
try {
  New-Item -ItemType Directory -Force -Path build | Out-Null

  # abi/sim_abi.h describes the game's own message structs, so it is generated from the player's
  # install rather than distributed. ONI_GAME (or -OniGame) is needed for the first build.
  if (-not (Test-Path -LiteralPath '..\abi\sim_abi.h' -PathType Leaf)) {
    $game = Get-OniGame $OniGame
    $py = Find-Python
    $pyArgs = @($py | Select-Object -Skip 1) + @('..\tools\gen_sim_abi.py', $game, '-o', '..\abi\sim_abi.h')
    Invoke-Native 'gen_sim_abi.py' $py[0] $pyArgs
  }

  # The flags of build.sh, which documents why each one is there (-ffp-contract=off is a guard:
  # keep it if -march is ever widened), plus -static: the WinLibs compiler otherwise links
  # libwinpthread-1.dll dynamically, and the game does not ship it, so the DLL would not load.
  $gxx = Find-Gxx
  $gxxArgs = @('-O3', '-march=x86-64-v2', '-ffp-contract=off', '-std=c++17', '-shared',
               '-static-libgcc', '-static-libstdc++', '-static',
               '-Wall', '-Wextra',
               '-o', 'build/SimDLL.dll', 'simdll.cpp',
               '-Wl,--out-implib,build/libSimDLL.a',
               '-lws2_32')
  Invoke-Gxx $gxx 'build\gcc-args.rsp' $gxxArgs

  Write-Output "built sim/build/SimDLL.dll"
} finally {
  Pop-Location
}
