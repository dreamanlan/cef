param(
    [string]$PythonExecutable = "",
    [switch]$ForceGenerate
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$cefRoot = $PSScriptRoot
$headerPath = Join-Path $cefRoot "include/cef_resource_request_handler.h"
$wrapperPath = Join-Path $cefRoot "libcef/browser/net_service/resource_request_handler_wrapper.cc"
$proxyPath = Join-Path $cefRoot "libcef/browser/net_service/proxy_url_loader_factory.cc"
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
        $introducedNext = $true
    }
} elseif ($headerText.Contains($headerMethodBody)) {
    if (Replace-Exact `
        $headerPath `
        $headerMethodBody `
        $versionedHeaderMethodBody `
        "public API version metadata") {
        $sourceChanged = $true
        $introducedNext = $true
    }
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

$resetOld = @'
      pending_request_ = pending_request;
      pending_response_ = nullptr;
      request_ = request;
'@
$resetNew = @'
      pending_request_ = pending_request;
      pending_response_ = nullptr;
      override_response_headers_ = nullptr;
      request_ = request;
'@
if (Replace-Exact `
    $wrapperPath `
    $resetOld `
    $resetNew `
    "reset response header override state") {
    $sourceChanged = $true
}

$fieldOld = @'
    CefRefPtr<CefRequestImpl> pending_request_;
    CefRefPtr<CefResponseImpl> pending_response_;
    raw_ptr<network::ResourceRequest> request_;
'@
$fieldNew = @'
    CefRefPtr<CefRequestImpl> pending_request_;
    CefRefPtr<CefResponseImpl> pending_response_;
    scoped_refptr<net::HttpResponseHeaders> override_response_headers_;
    raw_ptr<network::ResourceRequest> request_;
'@
if (Replace-Exact `
    $wrapperPath `
    $fieldOld `
    $fieldNew `
    "store response header override state") {
    $sourceChanged = $true
}

$processOld = @'
    if (!state->handler_) {
      return;
    }

    if (!state->pending_response_) {
      state->pending_response_ = new CefResponseImpl();
    } else {
      state->pending_response_->SetReadOnly(false);
    }

    if (headers) {
      state->pending_response_->SetResponseHeaders(*headers);
    }

    state->pending_response_->SetReadOnly(true);
  }

  void OnRequestResponse(
'@
$processNew = @'
    state->override_response_headers_ = nullptr;

    if (!state->handler_) {
      return;
    }

    if (!state->pending_response_) {
      state->pending_response_ = new CefResponseImpl();
    } else {
      state->pending_response_->SetReadOnly(false);
    }

    if (headers) {
      state->pending_response_->SetResponseHeaders(*headers);
    }

    const auto original_headers =
        state->pending_response_->GetResponseHeaders();

    state->handler_->OnBeforeResourceResponse(
        init_state_->browser_, init_state_->GetFrame(),
        state->pending_request_.get(), state->pending_response_.get());

    const auto modified_headers =
        state->pending_response_->GetResponseHeaders();
    state->pending_response_->SetReadOnly(true);

    if (original_headers && modified_headers &&
        original_headers->raw_headers() != modified_headers->raw_headers()) {
      state->override_response_headers_ = modified_headers;
    }
  }

  void OnRequestResponse(
'@
if (Replace-Exact `
    $wrapperPath `
    $processOld `
    $processNew `
    "invoke response callback and capture modified headers") {
    $sourceChanged = $true
}

$redirectOld = @'
    auto exec_callback = base::BindOnce(
        std::move(callback), ResponseMode::CONTINUE, nullptr, new_url);
'@
$redirectNew = @'
    auto exec_callback = base::BindOnce(
        std::move(callback), ResponseMode::CONTINUE,
        std::move(state->override_response_headers_), new_url);
'@
if (Replace-Exact `
    $wrapperPath `
    $redirectOld `
    $redirectNew `
    "forward redirect response header overrides") {
    $sourceChanged = $true
}

$responseOld = @'
    auto exec_callback =
        base::BindOnce(std::move(callback), response_mode, nullptr, new_url);
'@
$responseNew = @'
    auto exec_callback =
        base::BindOnce(std::move(callback), response_mode,
                       std::move(state->override_response_headers_), new_url);
'@
if (Replace-Exact `
    $wrapperPath `
    $responseOld `
    $responseNew `
    "forward normal response header overrides") {
    $sourceChanged = $true
}

$proxyOld = @'
  override_headers_ = override_headers;
  if (override_headers_ && current_response_) {
    current_response_->headers = override_headers_;
  }
  redirect_url_ = redirect_url;
'@
$proxyNew = @'
  override_headers_ = override_headers;
  if (override_headers_) {
    if (current_response_) {
      // Preserve the override for response paths that do not use
      // OnHeadersReceived.
      current_response_->headers = override_headers_;
    } else {
      // Preserve the override for the subsequent OnReceiveResponse call.
      current_headers_ = override_headers_;
    }
  }
  redirect_url_ = redirect_url;
'@
if (Replace-Exact `
    $proxyPath `
    $proxyOld `
    $proxyNew `
    "preserve overrides without current_response") {
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
