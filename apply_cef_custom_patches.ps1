param(
    [string]$PythonExecutable = "",
    [switch]$ForceGenerate
)

# Apply custom CEF source patches used by this project:
#   1. OnBeforeResourceResponse: a new CefResourceRequestHandler callback that
#      lets clients inspect/modify response headers before CEF processes them.
#      This one introduces a brand-new CEF API, so it is handled by bespoke
#      logic below (insert the added=next versioned method, then run the
#      official CEF generation tools). It cannot be a static unified diff
#      because the generation tools rewrite added=next into a concrete,
#      environment-determined API version number.
#   2. Every *.patch file under myapp/patch/: static unified diffs that do not
#      introduce any CEF API (e.g. the use-chrome-window window.open->tab
#      merge, the response-header override plumbing in the .cc files). These
#      are applied with `git apply` and are idempotent: if a patch already
#      reverse-applies cleanly it is considered applied and skipped.
#
# CEF has no built-in mechanism to auto-apply patches against its own source
# (only against Chromium), so this script performs the edits directly.
#
# To add a new static patch later, just drop a *.patch file into myapp/patch/
# (e.g. `git diff -- path/to/file > myapp/patch/NN-name.patch`). No code change
# is needed here or in the Python script.

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$cefRoot = $PSScriptRoot
$patchDir = Join-Path $cefRoot "myapp/patch"
$headerPath = Join-Path $cefRoot "include/cef_resource_request_handler.h"
$translatorPath = Join-Path $cefRoot "tools/translator.py"
$versionManagerPath = Join-Path $cefRoot "tools/version_manager.py"

$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
$sourceChanged = $false
$introducedNext = $false

function Get-NormalizedText {
    param([string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Required file does not exist: $Path"
    }

    return [System.IO.File]::ReadAllText($Path).Replace("`r`n", "`n")
}

function Set-NormalizedText {
    param(
        [string]$Path,
        [string]$Text
    )

    [System.IO.File]::WriteAllText($Path, $Text, $script:utf8NoBom)
}

function Get-OccurrenceCount {
    param(
        [string]$Text,
        [string]$Value
    )

    $count = 0
    $offset = 0
    while (($index = $Text.IndexOf(
        $Value,
        $offset,
        [System.StringComparison]::Ordinal)) -ge 0) {
        $count++
        $offset = $index + $Value.Length
    }

    return $count
}

function Replace-Exact {
    param(
        [string]$Path,
        [string]$OldText,
        [string]$NewText,
        [string]$Description
    )

    $OldText = $OldText.Replace("`r`n", "`n").Replace("`r", "`n")
    $NewText = $NewText.Replace("`r`n", "`n").Replace("`r", "`n")
    $text = Get-NormalizedText $Path
    if ($text.Contains($NewText)) {
        Write-Host "Already applied: $Description"
        return $false
    }

    $count = Get-OccurrenceCount $text $OldText
    if ($count -ne 1) {
        throw "Expected exactly one source anchor for '$Description', found $count in $Path"
    }

    $text = $text.Replace($OldText, $NewText)
    Set-NormalizedText $Path $text
    Write-Host "Applied: $Description"
    return $true
}

function Resolve-PythonExecutable {
    if (-not [string]::IsNullOrWhiteSpace($script:PythonExecutable)) {
        return $script:PythonExecutable
    }

    foreach ($candidate in @("python3", "python", "py")) {
        $command = Get-Command $candidate -ErrorAction SilentlyContinue
        if ($null -ne $command) {
            return $command.Source
        }
    }

    throw "Python was not found. Pass -PythonExecutable with an explicit path."
}

function Invoke-PythonTool {
    param(
        [string]$ToolPath,
        [string[]]$Arguments
    )

    Write-Host "Running: $script:PythonExecutable $ToolPath $($Arguments -join ' ')"
    & $script:PythonExecutable $ToolPath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Python tool failed with exit code $LASTEXITCODE`: $ToolPath"
    }
}

function Test-GitApply {
    param([string[]]$ExtraArgs)

    Push-Location $script:cefRoot
    try {
        # --ignore-whitespace makes git apply tolerate line-ending (CRLF vs LF)
        # differences. On Windows core.autocrlf checks out source as CRLF while
        # the *.patch files are LF, and without this the context lines fail to
        # match. It is a no-op where line endings already match.
        & git apply --ignore-whitespace @ExtraArgs 2>$null
        return ($LASTEXITCODE -eq 0)
    } finally {
        Pop-Location
    }
}

function Invoke-PatchFile {
    # Idempotently apply a single unified-diff patch via git apply.
    # Returns $true when freshly applied, $false when already applied.
    param([string]$PatchPath)

    $description = Split-Path -Leaf $PatchPath

    if (Test-GitApply @("--reverse", "--check", $PatchPath)) {
        Write-Host "Already applied: $description"
        return $false
    }

    if (-not (Test-GitApply @("--check", $PatchPath))) {
        throw "Patch does not apply and is not already applied: $description"
    }

    Push-Location $script:cefRoot
    try {
        & git apply --ignore-whitespace $PatchPath
        if ($LASTEXITCODE -ne 0) {
            throw "git apply failed for $description"
        }
    } finally {
        Pop-Location
    }

    Write-Host "Applied: $description"
    return $true
}

function Invoke-StaticPatches {
    if (-not (Test-Path -LiteralPath $script:patchDir -PathType Container)) {
        throw "Patch directory does not exist: $script:patchDir"
    }

    $patchFiles = Get-ChildItem -LiteralPath $script:patchDir -Filter "*.patch" -File |
        Sort-Object Name
    if ($patchFiles.Count -eq 0) {
        Write-Host "No static patches found under $script:patchDir"
        return $false
    }

    $changed = $false
    foreach ($patchFile in $patchFiles) {
        if (Invoke-PatchFile $patchFile.FullName) {
            $changed = $true
        }
    }
    return $changed
}

# 1. The OnBeforeResourceResponse API edit (bespoke; may trigger CEF generation
#    tools below). Generation rewrites added=next into a concrete version
#    number, which a static diff cannot express, so it stays in-script.
$headerMethodBody = @'
  ///
  /// Called on the IO thread after response headers are received and before
  /// they are processed by CEF. The |browser| and |frame| values represent the
  /// source of the request, and may be NULL for requests originating from
  /// service workers or CefURLRequest. The |request| object cannot be modified
  /// in this callback. The |response| object may be modified in this callback
  /// to change the status code, status text, MIME type, charset or response
  /// headers. Changes to other response properties will be ignored. The
  /// |response| object will be read-only after this callback returns.
  ///
  /// For responses received from the network this callback is executed before
  /// CORS validation. Cached responses may be delivered after CORS validation
  /// and modifications are therefore not guaranteed to affect CORS handling.
  ///
  /*--cef(optional_param=browser,optional_param=frame)--*/
  virtual void OnBeforeResourceResponse(CefRefPtr<CefBrowser> browser,
                                        CefRefPtr<CefFrame> frame,
                                        CefRefPtr<CefRequest> request,
                                        CefRefPtr<CefResponse> response) {}

'@

$versionedHeaderMethodBody = @'
#if CEF_API_ADDED(CEF_NEXT)
  ///
  /// Called on the IO thread after response headers are received and before
  /// they are processed by CEF. The |browser| and |frame| values represent the
  /// source of the request, and may be NULL for requests originating from
  /// service workers or CefURLRequest. The |request| object cannot be modified
  /// in this callback. The |response| object may be modified in this callback
  /// to change the status code, status text, MIME type, charset or response
  /// headers. Changes to other response properties will be ignored. The
  /// |response| object will be read-only after this callback returns.
  ///
  /// For responses received from the network this callback is executed before
  /// CORS validation. Cached responses may be delivered after CORS validation
  /// and modifications are therefore not guaranteed to affect CORS handling.
  ///
  /*--cef(optional_param=browser,optional_param=frame,added=next)--*/
  virtual void OnBeforeResourceResponse(CefRefPtr<CefBrowser> browser,
                                        CefRefPtr<CefFrame> frame,
                                        CefRefPtr<CefRequest> request,
                                        CefRefPtr<CefResponse> response) {}
#endif

'@

$headerText = Get-NormalizedText $headerPath
if (-not $headerText.Contains("OnBeforeResourceResponse")) {
    $resourceResponseAnchor = @'
  ///
  /// Called on the IO thread when a resource response is received. The
'@

    if (Replace-Exact `
        $headerPath `
        $resourceResponseAnchor `
        ($versionedHeaderMethodBody + $resourceResponseAnchor) `
        "public API method") {
        $sourceChanged = $true
    }
    $introducedNext = $true
} elseif ($headerText.Contains($headerMethodBody)) {
    if (Replace-Exact `
        $headerPath `
        $headerMethodBody `
        $versionedHeaderMethodBody `
        "public API version metadata") {
        $sourceChanged = $true
    }
    $introducedNext = $true
} else {
    $guardedMethodPattern = '(?s)#if CEF_API_ADDED\((?:CEF_NEXT|\d+)\)\n.*?/\*--cef\([^\r\n]*added=(?:next|\d+)[^\r\n]*\)--\*/\n\s*virtual void OnBeforeResourceResponse\(.*?\n#endif'
    if (-not [System.Text.RegularExpressions.Regex]::IsMatch(
        $headerText,
        $guardedMethodPattern)) {
        throw "OnBeforeResourceResponse exists but does not have recognized CEF API version metadata."
    }

    if ($headerText.Contains("CEF_API_ADDED(CEF_NEXT)") -and
        $headerText.Contains("added=next")) {
        $introducedNext = $true
        Write-Host "Already applied: public API method with NEXT metadata"
    } else {
        Write-Host "Already applied: public API method with an exact API version"
    }
}

# 2. Every static unified diff under myapp/patch/. These never introduce a CEF
#    API, so they never set $introducedNext.
if (Invoke-StaticPatches) {
    $sourceChanged = $true
}

if ($introducedNext -or $ForceGenerate) {
    $PythonExecutable = Resolve-PythonExecutable

    Push-Location $cefRoot
    try {
        Invoke-PythonTool $translatorPath @(
            "--root-dir",
            $cefRoot,
            "--classes",
            "CefResourceRequestHandler"
        )

        if ($introducedNext) {
            Invoke-PythonTool $versionManagerPath @(
                "-a",
                "--replace-next"
            )
        } else {
            Invoke-PythonTool $versionManagerPath @("-u")
        }
    } finally {
        Pop-Location
    }
} else {
    Write-Host "No NEXT metadata was introduced. Official generation was not required."
}

$finalHeader = Get-NormalizedText $headerPath
if (-not $finalHeader.Contains("OnBeforeResourceResponse")) {
    throw "Final verification failed: public API method is missing."
}
if ($finalHeader.Contains("CEF_API_ADDED(CEF_NEXT)") -or
    $finalHeader.Contains("added=next")) {
    throw "Final verification failed: NEXT metadata was not replaced."
}

$capiPath = Join-Path `
    $cefRoot `
    "include/capi/cef_resource_request_handler_capi.h"
$capiText = Get-NormalizedText $capiPath
if (-not $capiText.Contains("on_before_resource_response")) {
    throw "Final verification failed: generated C API callback is missing."
}

Write-Host "Patch completed successfully."
Write-Host "Source changed: $sourceChanged"
Write-Host "Build was not started."
