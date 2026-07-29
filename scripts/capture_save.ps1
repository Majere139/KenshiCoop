<#
.SYNOPSIS
  Promote a live save (%LOCALAPPDATA%\kenshi\save\<name>) into the repo fixture
  store (fixtures\saves\<name>). This is the ONLY sanctioned way to change a
  tracked fixture.

.DESCRIPTION
  Fixtures are pristine, version-controlled saves that the harness restores before
  every run (see deploy_saves.ps1). To deliberately update or add one, edit/bake the
  save in-game, then capture it here. The script mirrors the AppData copy over the
  repo copy with robocopy /MIR and prints the resulting git status so the change is
  reviewed before committing.

  Prefer bake_scene.ps1 -Promote for baked fixtures (duel1, bedcage1, pole1), which
  calls this automatically after a successful bake.

.EXAMPLE
  # After deliberately re-making the 'sync' town save:
  powershell -ExecutionPolicy Bypass -File scripts\capture_save.ps1 -Save sync
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$Save,
    # Override the live save root (defaults to the per-user AppData folder).
    [string]$SaveRoot = (Join-Path $env:LOCALAPPDATA "kenshi\save")
)

$ErrorActionPreference = "Stop"
$scriptDir   = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot    = Split-Path -Parent $scriptDir
$fixturesDir = Join-Path $repoRoot "fixtures\saves"

$src = Join-Path $SaveRoot $Save
if (-not (Test-Path (Join-Path $src "quick.save"))) {
    throw "Live save '$Save' not found (no quick.save) at $src"
}

# Don't capture a save the game might be mid-writing.
$live = @(Get-Process -Name "Kenshi_x64", "kenshi_x64" -ErrorAction SilentlyContinue)
if ($live.Count -gt 0) {
    throw "Kenshi is running ($($live.Count) process(es)); close it before capturing '$Save'."
}

New-Item -ItemType Directory -Force -Path $fixturesDir | Out-Null
$dst = Join-Path $fixturesDir $Save

Write-Host "Capturing '$Save':"
Write-Host "  from: $src"
Write-Host "  to:   $dst"
& robocopy $src $dst /MIR /R:1 /W:1 /NFL /NDL /NP /NJH /NJS | Out-Null
$rc = $LASTEXITCODE
if ($rc -ge 8) { throw "robocopy failed capturing '$Save' (rc=$rc)." }

Write-Host ""
Write-Host "Captured. Review before committing:"
& git -C $repoRoot status --short -- (Join-Path "fixtures\saves" $Save)
Write-Host ""
Write-Host "Commit with e.g.:"
Write-Host "  git add fixtures/saves/$Save && git commit -m `"fixture: refresh $Save`""
exit 0
