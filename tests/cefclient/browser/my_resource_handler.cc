// Copyright (c) 2026 The Chromium Embedded Framework Authors. All rights
// reserved. Use of this source code is governed by a BSD-style license that
// can be found in the LICENSE file.

#include "tests/cefclient/browser/my_resource_handler.h"

#include <map>

#include "include/base/cef_callback.h"
#include "include/cef_cookie.h"
#include "include/wrapper/cef_closure_task.h"
#include "include/wrapper/cef_helpers.h"
#include "tests/cefclient/browser/cookie_list_bridge.h"
#include "tests/cefclient/hostclr/HostCLR.h"

// Cookie-snapshot budget (IO thread only). See HostCLR.h.
int g_cookie_query_issued = 0;
int g_cookie_query_generation = 0;

namespace {

// Filter status values returned by on_response_content_filter_fptr.
// Must match CEF's cef_response_filter_status_t.
[[maybe_unused]] constexpr int kFilterDone = 0;
[[maybe_unused]] constexpr int kFilterNeedMoreData = 1;
constexpr int kFilterError = 2;

// Output staging buffer capacity. Decouples DSL output size from chromium's
// read size: DSL may produce up to this many bytes per filter call; Read()
// drains staging to chromium in whatever chunk sizes chromium asks for.
// This is an upper bound only; staging is allocated lazily (and for
// pass-through, only as large as the current chunk) by EnsureOutputBuffer().
constexpr size_t kOutputBufferSize = 4 * 1024 * 1024;  // 4MB

// Total unfiltered bytes allowed to wait for the browser-side reader. The
// upstream SimpleURLLoader resumes immediately after OnDownloadData, so this
// bound prevents an unconsumed large/streaming response from growing memory
// forever. Normal documents/XHR responses remain far below it.
constexpr size_t kMaxBufferedBodyBytes = 32 * 1024 * 1024;  // 32MB

// Interval for polling the upstream response for available headers. Only
// matters for responses whose body lags their headers; when the body follows
// immediately, OnDownloadData resolves the open decision first and this poll
// never gets a chance to run.
constexpr int64_t kHeaderPollIntervalMs = 8;

// ---------------------------------------------------------------------------
// Diagnostics for the "authenticated SPA reloads forever" investigation.
// The app-level 401 responses carry no WWW-Authenticate header, so the loop is
// driven by the SPA reacting to unauthenticated API calls. The helper below
// answers the request-side question: did the forwarded CefURLRequest carry
// the browser's cookies? (The jar-side question is answered by the
// DSL-driven cookie snapshot mechanism, see IssueCookieSnapshotQuery.)
// ---------------------------------------------------------------------------

// Cap the detailed diagnostics: a looping SPA produces hundreds of 401s and we
// only need the first few to tell "jar empty" from "jar populated but not sent".
constexpr int kMaxAuthDiagnostics = 20;
int g_auth_diag_count = 0;  // IO thread only.

// Forwarded CefURLRequests have no browser/frame association, so CEF cannot
// fall back to the browser's request handler when an HTTP auth challenge
// occurs. Keep their CefAuthCallback instances in one IO-owned registry until
// the managed/DSL/app UI flow resolves, cancels, or times out each challenge.
constexpr int kMaxAuthChallengesPerRequest = 3;
constexpr size_t kMaxPendingAuthChallenges = 64;
constexpr int64_t kAuthChallengeTimeoutMs = 30000;

struct PendingAuthChallenge {
  CefRefPtr<CefAuthCallback> callback;
  const MyResourceHandler* owner = nullptr;
};

// Intentionally heap-allocated local static: CEF/Chromium forbids exit-time
// destructors because static teardown may run after CEF shutdown.
std::map<uint64_t, PendingAuthChallenge>& GetPendingAuthChallenges() {
  static auto* challenges = new std::map<uint64_t, PendingAuthChallenge>();
  return *challenges;
}

uint64_t g_next_auth_challenge_id = 1;

void ResolvePendingAuthChallengeOnIOThread(uint64_t challenge_id,
                                           bool accepted,
                                           std::string username,
                                           std::string password) {
  CEF_REQUIRE_IO_THREAD();
  auto it = GetPendingAuthChallenges().find(challenge_id);
  if (it == GetPendingAuthChallenges().end()) {
    return;
  }

  CefRefPtr<CefAuthCallback> callback = it->second.callback;
  GetPendingAuthChallenges().erase(it);
  if (accepted) {
    callback->Continue(username, password);
  } else {
    callback->Cancel();
  }
}

void ExpirePendingAuthChallengeOnIOThread(uint64_t challenge_id) {
  CEF_REQUIRE_IO_THREAD();
  auto it = GetPendingAuthChallenges().find(challenge_id);
  if (it == GetPendingAuthChallenges().end()) {
    return;
  }
  printf_log(LOG_SEVERITY_WARNING,
             "[MyResourceHandler] HTTP auth challenge timed out: id=%llu",
             static_cast<unsigned long long>(challenge_id));
  ResolvePendingAuthChallengeOnIOThread(challenge_id, false, std::string(),
                                        std::string());
}

void CancelPendingAuthChallengesForOwnerOnIOThread(
    const MyResourceHandler* owner) {
  CEF_REQUIRE_IO_THREAD();
  for (auto it = GetPendingAuthChallenges().begin();
       it != GetPendingAuthChallenges().end();) {
    if (it->second.owner != owner) {
      ++it;
      continue;
    }
    CefRefPtr<CefAuthCallback> callback = it->second.callback;
    it = GetPendingAuthChallenges().erase(it);
    callback->Cancel();
  }
}

bool RegisterPendingAuthChallengeOnIOThread(
    const MyResourceHandler* owner,
    void* browser,
    const std::string& url,
    bool is_proxy,
    const CefString& host,
    int port,
    const CefString& realm,
    const CefString& scheme,
    CefRefPtr<CefAuthCallback> callback) {
  CEF_REQUIRE_IO_THREAD();
  if (!on_resource_auth_challenge_fptr || !callback ||
      GetPendingAuthChallenges().size() >= kMaxPendingAuthChallenges) {
    return false;
  }

  uint64_t challenge_id = g_next_auth_challenge_id++;
  if (challenge_id == 0) {
    challenge_id = g_next_auth_challenge_id++;
  }
  GetPendingAuthChallenges().emplace(
      challenge_id, PendingAuthChallenge{callback, owner});

  // Keep UTF-8 metadata alive for the full managed callback. Only challenge
  // metadata crosses this boundary; credentials flow in the opposite reply.
  const std::string host_string = host.ToString();
  const std::string realm_string = realm.ToString();
  const std::string scheme_string = scheme.ToString();
  const bool handled = on_resource_auth_challenge_fptr(
      browser, challenge_id, url.c_str(), is_proxy ? 1 : 0,
      host_string.c_str(), port, realm_string.c_str(), scheme_string.c_str());
  if (!handled) {
    // C# declined to own the challenge. Erase the registry entry before
    // returning false so CEF immediately cancels the unaffiliated request.
    GetPendingAuthChallenges().erase(challenge_id);
    return false;
  }

  CefPostDelayedTask(
      TID_IO,
      CefCreateClosureTask(base::BindOnce(
          &ExpirePendingAuthChallengeOnIOThread, challenge_id)),
      kAuthChallengeTimeoutMs);
  return true;
}

std::string ToLowerAscii(const CefString& value) {
  std::string result = value.ToString();
  for (auto& c : result) {
    c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
  }
  return result;
}

// Look up a header by case-insensitive name. The template accepts both
// CefRequest::HeaderMap and CefResponse::HeaderMap without implying that this
// helper only applies to request headers.
template <typename HeaderMap>
std::string FindHeader(const HeaderMap& headers, const std::string& name_lower) {
  for (const auto& pair : headers) {
    if (ToLowerAscii(pair.first) == name_lower) {
      return pair.second.ToString();
    }
  }
  return std::string();
}

bool IsRedirectStatus(int status) {
  return status == 301 || status == 302 || status == 303 || status == 307 ||
         status == 308;
}

// UR_FLAG_STOP_ON_REDIRECT makes CefURLRequest publish the upstream 3xx
// response and then finish with ERR_ABORTED. Treat only that exact combination
// (valid 3xx plus Location) as a successful redirect response. The original
// status and Location are subsequently returned unchanged to Chromium, which
// preserves 301/302/303/307/308 method semantics and resolves relative URLs.
bool IsStoppedRedirectResponse(CefRefPtr<CefResponse> response) {
  if (!response || !IsRedirectStatus(response->GetStatus())) {
    return false;
  }
  CefResponse::HeaderMap headers;
  response->GetHeaderMap(headers);
  return !FindHeader(headers, "location").empty();
}

void DeliverCookieSnapshotOnIOThread(CefRefPtr<CookieListBridge> snapshot) {
  CEF_REQUIRE_IO_THREAD();
  if (on_resource_cookie_list_fptr) {
    // The raw pointer is valid only during this call. |snapshot| keeps the
    // ref-counted bridge alive until the C# callback returns.
    on_resource_cookie_list_fptr(snapshot.get());
  }
}

// Fills a CookieListBridge snapshot during VisitUrlCookies enumeration and
// hands it to C# exactly once. Enumeration completion has no dedicated
// callback: the manager releases the visitor after the last Visit(), so the
// destructor is the one place that runs exactly once with the full data -
// including the empty-jar case (Visit() is never called then, which is
// itself a meaningful signal for the DSL side).
class CookieSnapshotVisitor : public CefCookieVisitor {
 public:
  explicit CookieSnapshotVisitor(CefRefPtr<CookieListBridge> snapshot)
      : m_Snapshot(std::move(snapshot)) {}

  ~CookieSnapshotVisitor() override {
    // CefCookieVisitor runs on the UI thread. Keep DSL resource callbacks on
    // their established IO thread by carrying the completed snapshot back via
    // a task. This also handles a synchronous VisitUrlCookies rejection (the
    // visitor then dies on IO and still posts asynchronously to IO).
    CefPostTask(
        TID_IO,
        CefCreateClosureTask(base::BindOnce(&DeliverCookieSnapshotOnIOThread,
                                            std::move(m_Snapshot))));
  }

  bool Visit(const CefCookie& cookie,
             int count,
             int total,
             bool& deleteCookie) override {
    CookieListBridge::Entry e;
    e.name = CefString(&cookie.name).ToString();
    e.value = CefString(&cookie.value).ToString();
    e.domain = CefString(&cookie.domain).ToString();
    e.path = CefString(&cookie.path).ToString();
    e.secure = cookie.secure;
    e.httponly = cookie.httponly;
    e.same_site = static_cast<int>(cookie.same_site);
    e.creation = static_cast<int64_t>(cookie.creation.val);
    e.last_access = static_cast<int64_t>(cookie.last_access.val);
    m_Snapshot->entries.push_back(std::move(e));
    return true;
  }

 private:
  CefRefPtr<CookieListBridge> m_Snapshot;

  IMPLEMENT_REFCOUNTING(CookieSnapshotVisitor);
};

}  // namespace

void ReplyResourceAuthCredentials(uint64_t challenge_id,
                                  bool accepted,
                                  const std::string& username,
                                  const std::string& password) {
  // The HostApi may be called from managed UI/application code on any thread.
  // Copy credentials into the task closure, then resolve exactly once on IO.
  CefPostTask(
      TID_IO,
      CefCreateClosureTask(base::BindOnce(&ResolvePendingAuthChallengeOnIOThread,
                                          challenge_id, accepted, username,
                                          password)));
}

MyResourceHandler::MyResourceHandler(
    CefRefPtr<CefRequest> upstream_request,
    CefRefPtr<CefResponse> response_override,
    CefRefPtr<CefRequestContext> request_context,
    CefRefPtr<CefFrame> frame,
    bool replace_content,
    int cookie_snapshot_limit,
    int cookie_snapshot_generation,
    CefRefPtr<CefBrowser> browser)
    : m_UpstreamRequest(upstream_request),
      m_ResponseOverride(response_override),
      m_RequestContext(request_context),
      m_Frame(frame),
      m_ReplaceContent(replace_content),
      m_CookieSnapshotLimit(cookie_snapshot_limit),
      m_CookieSnapshotGeneration(cookie_snapshot_generation),
      m_Browser(browser) {}

bool MyResourceHandler::Open(CefRefPtr<CefRequest> request,
                             bool& handle_request,
                             CefRefPtr<CefCallback> callback) {
  // Called on a worker sequence, not the IO thread ("in sequence but not
  // from a dedicated thread"). Frame-associated URL request creation requires
  // a valid CEF task runner, so defer the open decision to IO: |callback| will
  // be executed (or canceled) from the IO thread. The callback wrapper
  // provided by libcef is safe to execute from any thread.
  // |request| is ignored: the upstream request (already possibly edited by
  // the DSL) was captured in the constructor as m_UpstreamRequest.
  handle_request = false;
  CefPostTask(TID_IO,
              base::BindOnce(&MyResourceHandler::CreateRequestOnIOThread,
                             CefRefPtr<MyResourceHandler>(this), callback));
  return true;
}

void MyResourceHandler::CreateRequestOnIOThread(
    CefRefPtr<CefCallback> callback) {
  CEF_REQUIRE_IO_THREAD();

  // The request may have been canceled while the posted task was pending.
  if (m_Canceled) {
    callback->Cancel();
    return;
  }

  // Forward through the originating frame. Unlike CefURLRequest::Create,
  // CefFrame::CreateURLRequest creates a frame-associated BrowserURLRequest,
  // preserving the frame's URL loader factory and network observer path while
  // still returning data through this ResourceHandler for CSP/header/body
  // transformation. Do not fall back to unaffiliated Create(): that would
  // hide whether frame association fixes the authenticated SPA flow.
  if (!m_Frame || !m_Frame->IsValid()) {
    callback->Cancel();
    return;
  }
  m_Request = m_Frame->CreateURLRequest(m_UpstreamRequest, this);
  if (!m_Request) {
    callback->Cancel();
    return;
  }

  // Defer the open decision until the upstream response headers are
  // available. CefURLRequestClient provides no notification for that: libcef's
  // OnResponseStarted fills its internal response object without invoking any
  // client callback, and OnDownloadProgress is only dispatched once body bytes
  // have been received. Waiting for the first body chunk alone deadlocks on
  // responses whose headers arrive long before their first byte (SSE /
  // text/event-stream, long-poll): chromium never reaches GetResponseHeaders
  // and the request stays pending forever ("Provisional headers are shown").
  // Poll the upstream response instead; OnDownloadData / OnRequestComplete
  // still resolve the open decision first whenever the body follows
  // immediately, so the fast path costs nothing.
  m_OpenCallback = callback;
  PollResponseHeadersOnIOThread();
}

void MyResourceHandler::PollResponseHeadersOnIOThread() {
  CEF_REQUIRE_IO_THREAD();

  // Nothing left to unblock: the open decision was already resolved (by this
  // poll, OnDownloadData or OnRequestComplete) or the request was canceled.
  // Stop reposting, otherwise the task chain keeps this handler alive forever.
  if (!m_OpenCallback || m_Canceled || !m_Request) {
    return;
  }

  // GetResponse() is valid before completion (libcef only verifies the calling
  // thread) and returns libcef's internal response object, whose status line is
  // filled as soon as upstream headers arrive. A non-zero status is the same
  // "headers available" predicate libcef uses internally; GetError() covers
  // failures that never produced a status line.
  CefRefPtr<CefResponse> upstream = m_Request->GetResponse();
  if (upstream &&
      (upstream->GetStatus() != 0 || upstream->GetError() != ERR_NONE)) {
    m_OpenCallback->Continue();
    m_OpenCallback = nullptr;
    return;
  }

  CefPostDelayedTask(
      TID_IO,
      base::BindOnce(&MyResourceHandler::PollResponseHeadersOnIOThread,
                     CefRefPtr<MyResourceHandler>(this)),
      kHeaderPollIntervalMs);
}

void MyResourceHandler::GetResponseHeaders(CefRefPtr<CefResponse> response,
                                           int64_t& response_length,
                                           CefString& redirectUrl) {
  CEF_REQUIRE_IO_THREAD();
  // Deliberately preserve the original 3xx status and Location header rather
  // than using this CEF helper (which rewrites every redirect as 307).
  (void)redirectUrl;

  // Copy upstream response metadata into the chromium-provided response.
  CefRefPtr<CefResponse> upstream = m_Request ? m_Request->GetResponse() : nullptr;

  // UR_FLAG_STOP_ON_REDIRECT publishes a complete 3xx response and then
  // finishes CefURLRequest with ERR_ABORTED. That is a control-flow signal,
  // not a network failure. Return the original 3xx + Location HEADER (do not
  // set |redirectUrl|: CEF would replace it with a synthetic 307) so Chromium
  // follows it with the original method semantics and URL resolution.
  const bool stopped_redirect =
      upstream && upstream->GetError() == ERR_ABORTED &&
      IsStoppedRedirectResponse(upstream);

  // Propagate genuine upstream errors (DNS failure, timeout, ...) instead of
  // delivering a blank status-0 response. libcef checks GetError() first and
  // fails the request with this error code.
  if (upstream && upstream->GetError() != ERR_NONE && !stopped_redirect) {
    response->SetError(upstream->GetError());
    return;
  }

  if (upstream) {
    const int status = upstream->GetStatus();
    response->SetStatus(status);
    response->SetStatusText(upstream->GetStatusText());
    response->SetMimeType(upstream->GetMimeType());
    response->SetCharset(upstream->GetCharset());

    CefResponse::HeaderMap headers;
    upstream->GetHeaderMap(headers);

    // DSL-requested cookie-jar snapshot: issued here because the jar has
    // ingested this response's Set-Cookie headers by now and the upstream
    // status is known. GetResponseHeaders runs exactly once per request.
    if (m_CookieSnapshotLimit > g_cookie_query_issued) {
      IssueCookieSnapshotQuery(status);
    }

    // The app returns a bare 401/403 for unauthenticated API calls (no
    // challenge header), which is what makes the SPA reload. Dump the
    // request-side picture for the first few of them.
    if (status == 401 || status == 403) {
      LogAuthFailureDiagnostics(status);
    }

    // The body delivered by CefURLRequest is already content-decoded by the
    // network service, but upstream headers still describe the wire format.
    // Drop headers that would make chromium decode again or enforce the
    // (compressed) wire length.
    for (auto it = headers.begin(); it != headers.end();) {
      std::string nameLower = it->first.ToString();
      for (auto& c : nameLower) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
      if (nameLower == "content-encoding" || nameLower == "content-length" ||
          nameLower == "transfer-encoding") {
        it = headers.erase(it);
      } else {
        ++it;
      }
    }

    response->SetHeaderMap(headers);
  }

  // Apply DSL-provided header overrides (merge: same-name overwrite).
  if (m_ResponseOverride) {
    CefResponse::HeaderMap overrides;
    m_ResponseOverride->GetHeaderMap(overrides);
    for (const auto& pair : overrides) {
      const CefString& name = pair.first;
      const CefString& value = pair.second;
      if (value.empty()) {
        // Empty value in override means "delete this header".
        // CefResponse has no RemoveHeaderByName; emulate via full-map replace.
        CefResponse::HeaderMap current;
        response->GetHeaderMap(current);
        std::string nameLower = name.ToString();
        for (auto& c : nameLower) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        for (auto it = current.begin(); it != current.end();) {
          std::string keyLower = it->first.ToString();
          for (auto& c : keyLower) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
          if (keyLower == nameLower) {
            it = current.erase(it);
          } else {
            ++it;
          }
        }
        response->SetHeaderMap(current);
      } else {
        response->SetHeaderByName(name, value, true);
      }
    }
  }

  // Streaming: we don't know the total body length in advance (the body
  // filter may also change the length). Use -1 so chromium keeps calling
  // Read() until a read completes with 0 bytes.
  response_length = -1;
}

bool MyResourceHandler::Read(void* data_out,
                             int bytes_to_read,
                             int& bytes_read,
                             CefRefPtr<CefResourceReadCallback> callback) {
  // Called on the same worker sequence as Open(). All mutable state is owned
  // by the IO thread, so defer the read: |data_out| remains valid until
  // |callback| is executed (per the CefResourceHandler contract), and the
  // callback wrapper provided by libcef is safe to execute from any thread.
  bytes_read = 0;
  CefPostTask(TID_IO,
              base::BindOnce(&MyResourceHandler::ReadOnIOThread,
                             CefRefPtr<MyResourceHandler>(this), data_out,
                             bytes_to_read, callback));
  return true;
}

void MyResourceHandler::ReadOnIOThread(
    void* data_out,
    int bytes_to_read,
    CefRefPtr<CefResourceReadCallback> callback) {
  CEF_REQUIRE_IO_THREAD();

  // The request may have been canceled while the posted task was pending.
  // Do not Continue after Chromium canceled the handler; releasing this local
  // callback wrapper lets libcef cancel the corresponding stream operation.
  if (m_Canceled) {
    return;
  }

  // Refill staging from buffered input, or flush pending filter state after
  // upstream completion.
  if (m_OutputOffset >= m_OutputBufferSize) {
    const int filtered = FillOutputStaging();
    if (filtered < 0) {
      callback->Continue(-2);  // ERR_FAILED
      return;
    }
  }

  // Serve from staging.
  int served = ServeFromStaging(data_out, bytes_to_read);
  if (served > 0) {
    callback->Continue(served);
    return;
  }

  if (m_Completed) {
    // All body data has been delivered. Propagate upstream failures instead
    // of reporting a clean EOF for a truncated body.
    callback->Continue(m_UpstreamError != ERR_NONE ? -2 : 0);
    return;
  }

  // No buffered data yet; defer until OnDownloadData or OnRequestComplete.
  // Chromium will not issue another Read() until |callback| is executed.
  DCHECK(!m_ReadCallback);
  m_ReadCallback = callback;
  m_ReadDataOut = data_out;
  m_ReadBytesToRead = bytes_to_read;
}

bool MyResourceHandler::Skip(int64_t bytes_to_skip,
                             int64_t& bytes_skipped,
                             CefRefPtr<CefResourceSkipCallback> callback) {
  // Called on the same worker sequence as Open()/Read(). Defer to the IO
  // thread like Read(); the skip completes via |callback|. Skip() may be
  // called again for the remainder if we report a partial skip.
  bytes_skipped = 0;
  CefPostTask(TID_IO,
              base::BindOnce(&MyResourceHandler::SkipOnIOThread,
                             CefRefPtr<MyResourceHandler>(this), bytes_to_skip,
                             callback));
  return true;
}

void MyResourceHandler::SkipOnIOThread(
    int64_t bytes_to_skip,
    CefRefPtr<CefResourceSkipCallback> callback) {
  CEF_REQUIRE_IO_THREAD();

  // As with queued Read(), releasing this callback after cancellation lets
  // libcef cancel the already-abandoned skip operation.
  if (m_Canceled) {
    return;
  }

  int64_t skipped = DiscardOutput(bytes_to_skip);
  if (skipped < 0) {
    callback->Continue(0);  // Body filter error: fail the range request.
    return;
  }
  if (skipped >= bytes_to_skip) {
    callback->Continue(skipped);
    return;
  }
  if (m_Completed) {
    // Not enough data to satisfy the skip: fail the range request.
    callback->Continue(0);
    return;
  }

  // Wait for more upstream data; OnDownloadData resumes the skip.
  DCHECK(!m_SkipCallback);
  m_SkipCallback = callback;
  m_SkipTotal = skipped;
  m_SkipRemaining = bytes_to_skip - skipped;
}

void MyResourceHandler::Cancel() {
  CEF_REQUIRE_IO_THREAD();
  m_Canceled = true;

  // CefURLRequest::Cancel can synchronously invoke OnRequestComplete. Clear
  // all pending browser callbacks before calling it, otherwise that re-entry
  // can Continue a callback after Chromium has canceled this handler.
  m_OpenCallback = nullptr;
  m_ReadCallback = nullptr;
  m_ReadDataOut = nullptr;
  m_ReadBytesToRead = 0;
  m_SkipCallback = nullptr;
  m_SkipRemaining = 0;
  m_SkipTotal = 0;
  CancelPendingAuthChallengesForOwnerOnIOThread(this);

  CefRefPtr<CefURLRequest> request = m_Request;
  m_Request = nullptr;
  if (request) {
    request->Cancel();
  }
}

void MyResourceHandler::OnRequestComplete(CefRefPtr<CefURLRequest> request) {
  CEF_REQUIRE_IO_THREAD();
  if (m_Canceled || m_Completed) {
    return;
  }

  // Mark completion before canceling any residual auth callbacks: callback
  // cancellation can synchronously re-enter this method on some CEF paths.
  m_Completed = true;
  // A completed request cannot legitimately consume any still-pending auth
  // callback. Cancel it now so delayed UI replies become harmless no-ops.
  CancelPendingAuthChallengesForOwnerOnIOThread(this);
  CefRefPtr<CefResponse> done_response = request->GetResponse();
  const int done_status = done_response ? done_response->GetStatus() : 0;
  const int request_error = request->GetRequestError();
  m_UpstreamError =
      m_StreamError ? ERR_FAILED
                    : (request_error == ERR_ABORTED &&
                               IsStoppedRedirectResponse(done_response)
                           ? ERR_NONE
                           : request_error);
  // Log abnormal completions only: aborted/failed requests (err) never reach
  // GetResponseHeaders, so this is their only record; error statuses are the
  // reload-loop signal. Healthy completions stay silent.
  if (m_UpstreamError != ERR_NONE || done_status >= 400) {
    printf_log(LOG_SEVERITY_WARNING,
               "[MyResourceHandler] complete: status=%d err=%d body=%zu url=%s",
               done_status, m_UpstreamError, m_BodyBuffer.size(),
               m_UpstreamRequest->GetURL().ToString().c_str());
  }

  // If Open() is still waiting (empty-body case), unblock chromium now.
  if (m_OpenCallback) {
    m_OpenCallback->Continue();
    m_OpenCallback = nullptr;
  }

  // If a Skip() is pending, make its final filtered-stream progress attempt
  // first. A buffering filter may still emit output during completion flush.
  if (m_SkipCallback) {
    auto callback = m_SkipCallback;
    const int64_t skipped = DiscardOutput(m_SkipRemaining);
    m_SkipCallback = nullptr;
    if (skipped > 0) {
      m_SkipTotal += skipped;
      m_SkipRemaining -= skipped;
    }
    callback->Continue(skipped >= 0 && m_SkipRemaining <= 0 ? m_SkipTotal : 0);
  }

  // If a Read() is pending, make a final delivery attempt over remaining
  // input or a pending empty-input filter flush, then signal completion.
  if (m_ReadCallback) {
    auto callback = m_ReadCallback;
    void* data_out = m_ReadDataOut;
    const int bytes_to_read = m_ReadBytesToRead;
    m_ReadCallback = nullptr;
    m_ReadDataOut = nullptr;
    m_ReadBytesToRead = 0;

    if (m_OutputOffset >= m_OutputBufferSize) {
      const int filtered = FillOutputStaging();
      if (filtered < 0) {
        callback->Continue(-2);
        return;
      }
    }
    const int served = ServeFromStaging(data_out, bytes_to_read);
    if (served > 0) {
      callback->Continue(served);
    } else {
      // Propagate upstream/filter failures instead of reporting a clean EOF
      // for a truncated body.
      callback->Continue(m_UpstreamError != ERR_NONE ? -2 : 0);
    }
  }
}

void MyResourceHandler::OnDownloadData(CefRefPtr<CefURLRequest> request,
                                       const void* data,
                                       size_t data_length) {
  CEF_REQUIRE_IO_THREAD();

  // Fast path for the common case where the body follows the headers
  // immediately: the first body chunk implies headers are available, so unblock
  // Open() here instead of waiting for the next header poll.
  if (m_OpenCallback) {
    m_OpenCallback->Continue();
    m_OpenCallback = nullptr;
  }

  if (m_Canceled || m_StreamError) {
    return;
  }
  if (data_length > 0 &&
      (m_BodyBuffer.size() >= kMaxBufferedBodyBytes ||
       data_length > kMaxBufferedBodyBytes - m_BodyBuffer.size())) {
    // SimpleURLLoader resumes immediately after this callback. Without a
    // bound, a stalled browser-side reader or endless response would retain
    // the entire stream in m_BodyBuffer.
    m_StreamError = true;
    printf_log(LOG_SEVERITY_WARNING,
               "[MyResourceHandler] body buffer limit exceeded: buffered=%zu "
               "incoming=%zu url=%s",
               m_BodyBuffer.size(), data_length,
               m_UpstreamRequest->GetURL().ToString().c_str());
    if (m_Request) {
      m_Request->Cancel();
    }
    return;
  }
  m_BodyBuffer.append(static_cast<const char*>(data), data_length);

  // Resume a pending Skip() now that we have more data. Skip and Read are
  // never pending at the same time (chromium sequences stream operations).
  if (m_SkipCallback) {
    int64_t skipped = DiscardOutput(m_SkipRemaining);
    if (skipped > 0) {
      m_SkipTotal += skipped;
      m_SkipRemaining -= skipped;
    }
    if (skipped < 0 || m_SkipRemaining <= 0) {
      auto callback = m_SkipCallback;
      m_SkipCallback = nullptr;
      // Complete only when the full skip finished; otherwise fail.
      callback->Continue(m_SkipRemaining <= 0 ? m_SkipTotal : 0);
    }
    return;
  }

  // Satisfy any pending Read() now that we have data.
  if (m_ReadCallback) {
    auto callback = m_ReadCallback;
    void* data_out = m_ReadDataOut;
    int bytes_to_read = m_ReadBytesToRead;
    m_ReadCallback = nullptr;
    m_ReadDataOut = nullptr;
    m_ReadBytesToRead = 0;

    // Refill staging from the newly buffered input, then serve.
    if (m_OutputOffset >= m_OutputBufferSize) {
      const int filtered = FillOutputStaging();
      if (filtered < 0) {
        callback->Continue(-2);
        return;
      }
    }
    int served = ServeFromStaging(data_out, bytes_to_read);
    if (served > 0) {
      callback->Continue(served);
    } else {
      // Still no data (shouldn't happen right after OnDownloadData, but be safe).
      m_ReadCallback = callback;
      m_ReadDataOut = data_out;
      m_ReadBytesToRead = bytes_to_read;
    }
  }
}

int MyResourceHandler::FilterBodyChunk(bool flush) {
  CEF_REQUIRE_IO_THREAD();

  if (!flush && m_BodyBuffer.empty()) {
    return 0;
  }

  // Cap each DSL invocation to staging capacity. Remaining input stays in
  // m_BodyBuffer for the next call. Completion flushes use empty input.
  int input_size = flush ? 0 : static_cast<int>(m_BodyBuffer.size());
  if (input_size > static_cast<int>(kOutputBufferSize)) {
    input_size = static_cast<int>(kOutputBufferSize);
  }

  // Allocate staging lazily. FilterBodyChunk is called only while staging is
  // drained, so growing the vector cannot clobber pending output.
  const bool filtering = m_ReplaceContent && on_response_content_filter_fptr;
  EnsureOutputBuffer(filtering ? kOutputBufferSize
                               : static_cast<size_t>(input_size));
  const int output_capacity = static_cast<int>(m_OutputBuffer.size());

  // If body filtering is disabled or absent, pass input through unchanged.
  // There is no filter state to flush in this mode.
  if (!filtering) {
    m_FilterNeedsFlush = false;
    if (flush) {
      return 0;
    }
    const int copy = input_size < output_capacity ? input_size : output_capacity;
    memcpy(m_OutputBuffer.data(), m_BodyBuffer.data(), copy);
    m_OutputBufferSize = static_cast<size_t>(copy);
    m_OutputOffset = 0;
    m_BodyBuffer.erase(0, copy);
    return copy;
  }

  int data_in_read = 0;
  int data_out_written = 0;
  int status = kFilterDone;
  const void* data_in = input_size > 0 ? m_BodyBuffer.data() : nullptr;
  const bool handled = on_response_content_filter_fptr(
      data_in, input_size, m_OutputBuffer.data(), output_capacity,
      data_in_read, data_out_written, status);

  // DSL did not handle this input. Empty completion input has nothing to pass
  // through and therefore completes the pending flush.
  if (!handled) {
    m_FilterNeedsFlush = false;
    if (flush) {
      return 0;
    }
    const int copy = input_size < output_capacity ? input_size : output_capacity;
    memcpy(m_OutputBuffer.data(), m_BodyBuffer.data(), copy);
    m_OutputBufferSize = static_cast<size_t>(copy);
    m_OutputOffset = 0;
    m_BodyBuffer.erase(0, copy);
    return copy;
  }

  if (status != kFilterDone && status != kFilterNeedMoreData &&
      status != kFilterError) {
    return -2;
  }
  if (status == kFilterError) {
    return -2;
  }

  // Clamp reported sizes to actual buffers.
  if (data_in_read < 0) data_in_read = 0;
  if (data_in_read > input_size) data_in_read = input_size;
  if (data_out_written < 0) data_out_written = 0;
  if (data_out_written > output_capacity) data_out_written = output_capacity;

  if (!flush && data_in_read == 0 && data_out_written == 0 &&
      status != kFilterNeedMoreData) {
    // A handled DONE response made no progress. Retrying would spin forever
    // over the same buffered input, so fail rather than truncate/hang.
    return -2;
  }
  if (flush && data_out_written == 0 && status == kFilterNeedMoreData) {
    // No more upstream data can arrive. A filter that still asks for input
    // would otherwise be silently truncated at EOF.
    return -2;
  }

  if (!flush) {
    m_BodyBuffer.erase(0, data_in_read);
  }
  m_FilterNeedsFlush = status == kFilterNeedMoreData;
  m_OutputBufferSize = static_cast<size_t>(data_out_written);
  m_OutputOffset = 0;
  return data_out_written;
}

int MyResourceHandler::FillOutputStaging() {
  CEF_REQUIRE_IO_THREAD();

  while (m_OutputOffset >= m_OutputBufferSize) {
    const bool flush = m_Completed && m_BodyBuffer.empty() &&
                       m_FilterNeedsFlush;
    if (m_BodyBuffer.empty() && !flush) {
      return 0;
    }

    const size_t buffered_before = m_BodyBuffer.size();
    const int filtered = FilterBodyChunk(flush);
    if (filtered < 0) {
      return filtered;
    }
    if (filtered > 0) {
      return filtered;
    }
    if (flush || m_BodyBuffer.size() == buffered_before) {
      // A non-completed filter may wait for a larger upstream buffer. At EOF,
      // that same no-progress NEED_MORE_DATA state would discard the remaining
      // body, so fail it rather than report a clean EOF.
      if (!flush && m_Completed && m_FilterNeedsFlush) {
        return -2;
      }
      return 0;
    }
    // The filter consumed input without output. Continue immediately: more
    // bytes are already buffered and waiting for this same Read/Skip.
  }
  return static_cast<int>(m_OutputBufferSize - m_OutputOffset);
}

void MyResourceHandler::EnsureOutputBuffer(size_t needed) {
  CEF_REQUIRE_IO_THREAD();

  if (m_OutputBuffer.size() < needed) {
    m_OutputBuffer.resize(needed);
  }
}

int MyResourceHandler::ServeFromStaging(void* data_out, int bytes_to_read) {
  CEF_REQUIRE_IO_THREAD();

  if (m_OutputOffset >= m_OutputBufferSize) return 0;

  size_t avail = m_OutputBufferSize - m_OutputOffset;
  size_t to_read = static_cast<size_t>(bytes_to_read) < avail
                       ? static_cast<size_t>(bytes_to_read)
                       : avail;
  memcpy(data_out, m_OutputBuffer.data() + m_OutputOffset, to_read);
  m_OutputOffset += to_read;

  // Reset staging when fully drained.
  if (m_OutputOffset >= m_OutputBufferSize) {
    m_OutputBufferSize = 0;
    m_OutputOffset = 0;
  }

  return static_cast<int>(to_read);
}

int64_t MyResourceHandler::DiscardOutput(int64_t count) {
  CEF_REQUIRE_IO_THREAD();

  int64_t discarded = 0;
  while (discarded < count) {
    // Refill staging from buffered input, or drain completion-flush output.
    if (m_OutputOffset >= m_OutputBufferSize) {
      const int filtered = FillOutputStaging();
      if (filtered < 0) return -1;
      if (filtered == 0) break;  // Wait for upstream input, or EOF.
    }

    size_t avail = m_OutputBufferSize - m_OutputOffset;
    size_t drop = avail < static_cast<size_t>(count - discarded)
                      ? avail
                      : static_cast<size_t>(count - discarded);
    m_OutputOffset += drop;
    discarded += static_cast<int64_t>(drop);

    // Reset staging when fully drained (same as ServeFromStaging).
    if (m_OutputOffset >= m_OutputBufferSize) {
      m_OutputBufferSize = 0;
      m_OutputOffset = 0;
    }
  }
  return discarded;
}

void MyResourceHandler::IssueCookieSnapshotQuery(int status) {
  CEF_REQUIRE_IO_THREAD();

  // Recheck the cap at issue time. Several handlers can receive the same DSL
  // cap while waiting for headers; only the first |limit| that actually reach
  // this point consume the global budget. Requests canceled before headers do
  // not consume it.
  if (m_CookieSnapshotGeneration != g_cookie_query_generation ||
      m_CookieSnapshotLimit <= g_cookie_query_issued) {
    return;
  }

  CefRefPtr<CefCookieManager> manager =
      m_RequestContext ? m_RequestContext->GetCookieManager(nullptr)
                       : CefCookieManager::GetGlobalManager(nullptr);
  if (!manager) {
    return;
  }
  ++g_cookie_query_issued;
  m_CookieSnapshotLimit = 0;

  CefRefPtr<CookieListBridge> snapshot = new CookieListBridge();
  snapshot->url = m_UpstreamRequest->GetURL().ToString();
  snapshot->status = status;
  // The visitor posts the completed snapshot to IO, including the empty-jar
  // case. A rejected VisitUrlCookies still releases the visitor, so the same
  // delivery path reports the empty snapshot.
  manager->VisitUrlCookies(snapshot->url, true,
                           new CookieSnapshotVisitor(snapshot));
}

void MyResourceHandler::LogAuthFailureDiagnostics(int status) {
  CEF_REQUIRE_IO_THREAD();

  if (g_auth_diag_count >= kMaxAuthDiagnostics) {
    return;
  }
  ++g_auth_diag_count;

  const std::string url = m_UpstreamRequest->GetURL().ToString();

  CefRequest::HeaderMap req_headers;
  m_UpstreamRequest->GetHeaderMap(req_headers);
  const std::string cookie = FindHeader(req_headers, "cookie");
  const std::string authorization = FindHeader(req_headers, "authorization");
  const std::string origin = FindHeader(req_headers, "origin");

  // UR_FLAG_ALLOW_STORED_CREDENTIALS must be set for the network stack to
  // attach jar cookies; site_for_cookies being empty makes the forward look
  // cross-site, which drops SameSite=Lax/Strict cookies even when the flag is
  // present. Both are the prime suspects for the unauthenticated forward.
  printf_log(LOG_SEVERITY_WARNING,
             "[MyResourceHandler] AUTH FAIL status=%d flags=0x%x "
             "site_for_cookies=%s referrer=%s origin=%s "
             "cookie_len=%zu authorization_len=%zu url=%s",
             status, static_cast<unsigned int>(m_UpstreamRequest->GetFlags()),
             m_UpstreamRequest->GetFirstPartyForCookies().ToString().c_str(),
             m_UpstreamRequest->GetReferrerURL().ToString().c_str(),
             origin.c_str(), cookie.size(), authorization.size(), url.c_str());
}

// ---------------------------------------------------------------------------
// GetAuthCredentials: called on the IO thread only for a real HTTP/proxy auth
// challenge (401/407 + challenge header). Forwarded CefURLRequests have no
// browser/frame association, so returning false would immediately cancel the
// request instead of falling back to the browser's handler. Retain the CEF
// callback in the shared IO registry and hand its non-secret metadata to the
// managed/DSL/application UI flow. That flow must resolve the one-shot ID via
// HostApi::ReplyResourceAuthCredentials before timeout. No request rebuilding,
// header polling, or manual Authorization injection is involved.
// ---------------------------------------------------------------------------
bool MyResourceHandler::GetAuthCredentials(
    bool isProxy,
    const CefString& host,
    int port,
    const CefString& realm,
    const CefString& scheme,
    CefRefPtr<CefAuthCallback> callback) {
  CEF_REQUIRE_IO_THREAD();

  if (m_Canceled || m_Completed ||
      m_AuthChallengeCount >= kMaxAuthChallengesPerRequest) {
    printf_log(LOG_SEVERITY_WARNING,
               "[MyResourceHandler] HTTP auth challenge rejected: proxy=%d "
               "host=%s port=%d realm=%s scheme=%s attempts=%d url=%s",
               isProxy ? 1 : 0, host.ToString().c_str(), port,
               realm.ToString().c_str(), scheme.ToString().c_str(),
               m_AuthChallengeCount,
               m_UpstreamRequest->GetURL().ToString().c_str());
    return false;
  }

  ++m_AuthChallengeCount;
  const bool registered = RegisterPendingAuthChallengeOnIOThread(
      this, m_Browser.get(), m_UpstreamRequest->GetURL().ToString(), isProxy,
      host, port, realm, scheme, callback);
  if (!registered) {
    printf_log(LOG_SEVERITY_WARNING,
               "[MyResourceHandler] HTTP auth challenge not handled: proxy=%d "
               "host=%s port=%d realm=%s scheme=%s url=%s",
               isProxy ? 1 : 0, host.ToString().c_str(), port,
               realm.ToString().c_str(), scheme.ToString().c_str(),
               m_UpstreamRequest->GetURL().ToString().c_str());
  }
  return registered;
}
