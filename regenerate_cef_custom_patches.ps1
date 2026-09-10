# Regenerate myapp/patch/*.patch from the current working tree.
#
# Use this AFTER upgrading CEF and re-running apply_cef_custom_patches.ps1, to
# refresh each patch's line numbers and context against the new upstream source
# (i.e. fix line-number drift). Each patch is re-exported via `git diff` for the
# exact file(s) that patch touches.
#
# Typical upgrade workflow:
#   1. Upgrade / re-sync the CEF source tree.
#   2. pwsh -File ./apply_cef_custom_patches.ps1
#      (git apply --ignore-whitespace tolerates drifted line numbers and still
#       applies the changes onto the new source.)
#   3. pwsh -File ./regenerate_cef_custom_patches.ps1
#      (re-export every patch so its hunks match the new source exactly.)
#   4. Review `git diff -- myapp/patch` and commit the refreshed patches.
#
# Safety:
#   * A patch is only regenerated when it is currently applied (its reverse diff
#     applies cleanly, ignoring whitespace) AND `git diff` for its files is
#     non-empty. Otherwise the patch file is left untouched, so an un-applied
#     patch is never clobbered with empty content.
#   * The OnBeforeResourceResponse API header edit is NOT a static patch (it is
#     handled by apply_cef_custom_patches.ps1 plus the official CEF generators),
#     so it is intentionally ignored here.

param(
    [string]$CefRoot = ""
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

if ([string]::IsNullOrWhiteSpace($CefRoot)) {
    $CefRoot = $PSScriptRoot
}
$CefRoot = (Resolve-Path -LiteralPath $CefRoot).Path
$patchDir = Join-Path $CefRoot "myapp/patch"
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)

function Get-TargetPaths {
    param([string]$PatchText)

    $paths = New-Object System.Collections.Generic.List[string]
    foreach ($line in ($PatchText -split "`n")) {
        $match = [regex]::Match($line, '^diff --git a/.+? b/(.+)$')
        if ($match.Success) {
            $path = $match.Groups[1].Value
            if (-not $paths.Contains($path)) {
                $paths.Add($path)
            }
        }
    }
    return $paths
}

function Test-PatchApplied {
    param([string]$PatchPath)

    Push-Location $script:CefRoot
    try {
        & git apply --ignore-whitespace --reverse --check $PatchPath 2>$null
        return ($LASTEXITCODE -eq 0)
    } finally {
        Pop-Location
    }
}

function Export-Diff {
    param([string[]]$Paths)

    Push-Location $script:CefRoot
    try {
        # Do NOT merge stderr (no 2>&1): git may emit warnings such as
        # "LF will be replaced by CRLF" that would otherwise be injected into
        # the patch body. Only stdout (the diff) is captured here.
        $output = & git --no-pager diff -- @Paths
        if ($LASTEXITCODE -ne 0) {
            throw "git diff failed for: $($Paths -join ', ')"
        }
    } finally {
        Pop-Location
    }

    if ($null -eq $output) {
        return ""
    }
    # git output arrives as an array of lines; rejoin with LF and normalize.
    $text = ($output -join "`n")
    return $text.Replace("`r`n", "`n")
}

if (-not (Test-Path -LiteralPath $patchDir -PathType Container)) {
    throw "Patch directory does not exist: $patchDir"
}

$patchFiles = Get-ChildItem -LiteralPath $patchDir -Filter "*.patch" -File |
    Sort-Object Name
if ($patchFiles.Count -eq 0) {
    Write-Host "No patches found under $patchDir"
    return
}

$updated = 0
$unchanged = 0
$skipped = 0

foreach ($patchFile in $patchFiles) {
    $name = $patchFile.Name
    $oldText = [System.IO.File]::ReadAllText($patchFile.FullName).Replace("`r`n", "`n")
    $targetPaths = @(Get-TargetPaths $oldText)

    if ($targetPaths.Count -eq 0) {
        Write-Host "Skipped (no 'diff --git' target found): $name"
        $skipped++
        continue
    }

    if (-not (Test-PatchApplied $patchFile.FullName)) {
        Write-Host "Skipped (not currently applied, won't overwrite): $name"
        $skipped++
        continue
    }

    $newText = Export-Diff $targetPaths
    if ([string]::IsNullOrWhiteSpace($newText)) {
        Write-Host "Skipped (git diff is empty, won't overwrite): $name"
        $skipped++
        continue
    }

    # git diff output has no trailing newline; the original patch files end
    # with one. Match that so identical content compares equal.
    if (-not $newText.EndsWith("`n")) {
        $newText += "`n"
    }

    if ($newText -eq $oldText) {
        Write-Host "Unchanged: $name"
        $unchanged++
        continue
    }

    [System.IO.File]::WriteAllText($patchFile.FullName, $newText, $utf8NoBom)
    Write-Host "Regenerated: $name"
    $updated++
}

Write-Host ""
Write-Host "Done. Regenerated: $updated, unchanged: $unchanged, skipped: $skipped."
if ($skipped -gt 0) {
    Write-Host "Review skipped patches manually (a skipped patch usually means it was not applied; run apply_cef_custom_patches.ps1 first)."
}
if ($updated -gt 0) {
    Write-Host "Review ``git diff -- myapp/patch`` and commit the refreshed patches."
}
