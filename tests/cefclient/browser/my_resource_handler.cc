// Copyright (c) 2026 The Chromium Embedded Framework Authors. All rights
// reserved. Use of this source code is governed by a BSD-style license that
// can be found in the LICENSE file.

#include "tests/cefclient/browser/my_resource_handler.h"

#include "include/base/cef_callback.h"
#include "include/cef_parser.h"
#include "include/wrapper/cef_closure_task.h"
#include "include/wrapper/cef_helpers.h"
#include "tests/cefclient/hostclr/HostCLR.h"

namespace {

// Filter status values returned by on_response_content_filter_fptr.
// Must match CEF's cef_response_filter_status_t.
[[maybe_unused]] constexpr int kFilterDone = 0;
[[maybe_unused]] constexpr int kFilterNeedMoreData = 1;
constexpr int kFilterError = 2;

// Output staging buffer capacity. Decouples DSL output size from chromium's
// read size: DSL may produce up to this many bytes per filter call; Read()
// drains staging to chromium in whatever chunk sizes chromium asks for.
constexpr size_t kOutputBufferSize = 4 * 1024 * 1024;  // 4MB

}  // namespace

MyResourceHandler::MyResourceHandler(
    CefRefPtr<CefResponse> response_override,
    CefRefPtr<CefRequestContext> request_context,
    bool replace_content)
    : m_ResponseOverride(response_override),
      m_RequestContext(request_context),
      m_ReplaceContent(replace_content),
      m_OutputBuffer(kOutputBufferSize) {}

bool MyResourceHandler::Open(CefRefPtr<CefRequest> request,
                             bool& handle_request,
                             CefRefPtr<CefCallback> callback) {
  // Called on a worker sequence, not the IO thread ("in sequence but not
  // from a dedicated thread"). CefURLRequest::Create requires the IO thread,
  // so defer the open decision: |callback| will be executed (or canceled)
  // from the IO thread. The callback wrapper provided by libcef is safe to
  // execute from any thread.
  handle_request = false;
  CefPostTask(TID_IO,
              base::BindOnce(&MyResourceHandler::CreateRequestOnIOThread,
                             CefRefPtr<MyResourceHandler>(this), request,
                             callback));
  return true;
}

void MyResourceHandler::CreateRequestOnIOThread(
    CefRefPtr<CefRequest> request,
    CefRefPtr<CefCallback> callback) {
  CEF_REQUIRE_IO_THREAD();

  // The request may have been canceled while the posted task was pending.
  if (m_Canceled) {
    callback->Cancel();
    return;
  }

  // CefURLRequest delivers the body exactly as received (no transparent
  // decompression), while chromium treats our synthesized response as
  // already-decoded. Forwarding the browser's Accept-Encoding would let the
  // upstream send gzip/br bytes that reach the renderer undecoded (garbled
  // page). The incoming request is read-only, so build a copy that asks for
  // identity encoding instead.
  CefRefPtr<CefRequest> newRequest = CefRequest::Create();
  newRequest->SetURL(request->GetURL());
  newRequest->SetMethod(request->GetMethod());
  newRequest->SetReferrer(request->GetReferrerURL(),
    request->GetReferrerPolicy());
  // UR_FLAG_ALLOW_STORED_CREDENTIALS is required for cookies to be sent AND
  // for upstream Set-Cookie headers to be ingested into the shared cookie
  // jar (see libcef/common/request_impl.cc: without this flag,
  // credentials_mode is forced to kOmit). CefRequestImpl::Set(ResourceRequest)
  // never copies flags from the incoming ResourceRequest, so the flags read
  // here are always UR_FLAG_NONE; without this OR the forwarding path
  // silently drops all session cookies, breaking SSO redirect chains
  // (evaluation.woa.com main-doc, gemini.google.com ERR_TOO_MANY_REDIRECTS).
  newRequest->SetFlags(request->GetFlags() | UR_FLAG_ALLOW_STORED_CREDENTIALS);
  newRequest->SetFirstPartyForCookies(request->GetFirstPartyForCookies());

  CefRequest::HeaderMap headerMap;
  request->GetHeaderMap(headerMap);
  for (auto it = headerMap.begin(); it != headerMap.end();) {
    std::string nameLower = it->first.ToString();
    for (auto& c : nameLower) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    if (nameLower == "accept-encoding") {
      it = headerMap.erase(it);
    } else {
      ++it;
    }
  }
  headerMap.insert(std::make_pair("Accept-Encoding", "identity"));
  newRequest->SetHeaderMap(headerMap);

  // Post data (single element, guaranteed by the GetResourceHandler
  // pre-check) is shared with the original request.
  if (CefRefPtr<CefPostData> postData = request->GetPostData()) {
    newRequest->SetPostData(postData);
  }

  // Forward the request via CefURLRequest. The browser's request context
  // keeps the upstream cookie store in sync with the browser (SSO redirect
  // chains converge upstream).
  m_Request = CefURLRequest::Create(newRequest, this, m_RequestContext);
  if (!m_Request) {
    callback->Cancel();
    return;
  }

  // Defer the open decision until we receive the first body chunk (or the
  // request completes with an empty body). Chromium will call
  // GetResponseHeaders after we run |callback|.
  m_OpenCallback = callback;
}

void MyResourceHandler::GetResponseHeaders(CefRefPtr<CefResponse> response,
                                           int64_t& response_length,
                                           CefString& redirectUrl) {
  CEF_REQUIRE_IO_THREAD();

  // Copy upstream response metadata into the chromium-provided response.
  CefRefPtr<CefResponse> upstream = m_Request ? m_Request->GetResponse() : nullptr;

  // Propagate upstream network errors (DNS failure, timeout, ...) instead of
  // delivering a blank status-0 response. libcef checks GetError() first and
  // fails the request with this error code.
  if (upstream && upstream->GetError() != ERR_NONE) {
    response->SetError(upstream->GetError());
    return;
  }

  // Redirects are followed inside CefURLRequest; chromium sees a single
  // response from the original URL. Do NOT surface upstream redirects via
  // |redirectUrl|: that turns server-side redirect chains (e.g. SSO flows
  // with cross-origin hops or JS-driven steps) into browser-level
  // navigations that reload the page in a loop.
  if (upstream) {
    response->SetStatus(upstream->GetStatus());
    response->SetStatusText(upstream->GetStatusText());
    response->SetMimeType(upstream->GetMimeType());
    response->SetCharset(upstream->GetCharset());

    CefResponse::HeaderMap headers;
    upstream->GetHeaderMap(headers);

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
  if (m_Canceled) {
    callback->Continue(-2);  // ERR_FAILED
    return;
  }

  // Refill staging if empty and upstream body is available.
  if (m_OutputOffset >= m_OutputBufferSize && !m_BodyBuffer.empty()) {
    int filtered = FilterBodyChunk();
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

  if (m_Canceled) {
    callback->Continue(0);  // Fails with ERR_REQUEST_RANGE_NOT_SATISFIABLE.
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
  if (m_Request) {
    m_Request->Cancel();
    m_Request = nullptr;
  }
  // Release pending callbacks; the libcef callback wrappers auto-cancel/fail
  // the underlying chromium callbacks when destroyed.
  m_OpenCallback = nullptr;
  m_ReadCallback = nullptr;
  m_ReadDataOut = nullptr;
  m_SkipCallback = nullptr;
}

void MyResourceHandler::OnRequestComplete(CefRefPtr<CefURLRequest> request) {
  CEF_REQUIRE_IO_THREAD();
  m_Completed = true;
  m_UpstreamError = request->GetRequestError();

  // If Open() is still waiting (empty-body case), unblock chromium now.
  if (m_OpenCallback) {
    m_OpenCallback->Continue();
    m_OpenCallback = nullptr;
  }

  // If a Skip() is pending and no more data will arrive, the remaining bytes
  // cannot be skipped: fail the range request.
  if (m_SkipCallback) {
    auto callback = m_SkipCallback;
    m_SkipCallback = nullptr;
    callback->Continue(0);
  }

  // If a Read() is pending, make a final delivery attempt over the remaining
  // body (a buffering DSL filter may still produce output), then signal
  // completion.
  if (m_ReadCallback) {
    auto callback = m_ReadCallback;
    void* data_out = m_ReadDataOut;
    int bytes_to_read = m_ReadBytesToRead;
    m_ReadCallback = nullptr;
    m_ReadDataOut = nullptr;

    if (m_OutputOffset >= m_OutputBufferSize && !m_BodyBuffer.empty()) {
      // Ignore filter errors here; fall through to the error/EOF below.
      FilterBodyChunk();
    }
    int served = ServeFromStaging(data_out, bytes_to_read);
    if (served > 0) {
      callback->Continue(served);
    } else {
      // Propagate upstream failures instead of reporting a clean EOF for a
      // truncated body.
      callback->Continue(m_UpstreamError != ERR_NONE ? -2 : 0);
    }
  }
}

void MyResourceHandler::OnDownloadData(CefRefPtr<CefURLRequest> request,
                                       const void* data,
                                       size_t data_length) {
  CEF_REQUIRE_IO_THREAD();

  // First body chunk implies response headers are available; unblock Open().
  if (m_OpenCallback) {
    m_OpenCallback->Continue();
    m_OpenCallback = nullptr;
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

    // Refill staging if empty, then serve.
    if (m_OutputOffset >= m_OutputBufferSize && !m_BodyBuffer.empty()) {
      int filtered = FilterBodyChunk();
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

int MyResourceHandler::FilterBodyChunk() {
  CEF_REQUIRE_IO_THREAD();

  if (m_BodyBuffer.empty()) return 0;

  // Cap input to staging capacity: DSL sees at most kOutputBufferSize bytes
  // per call. Remaining input stays in m_BodyBuffer for the next call. This
  // keeps memory bounded when upstream body is large and DSL consumes slowly.
  int input_size = static_cast<int>(m_BodyBuffer.size());
  if (input_size > static_cast<int>(kOutputBufferSize)) {
    input_size = static_cast<int>(kOutputBufferSize);
  }
  int output_capacity = static_cast<int>(m_OutputBuffer.size());

  // If body filtering is disabled (m_ReplaceContent=false) or no DSL body
  // filter is registered, pass through unchanged (capped to staging capacity;
  // any overflow stays in m_BodyBuffer for the next call).
  if (!m_ReplaceContent || !on_response_content_filter_fptr) {
    int copy = input_size < output_capacity ? input_size : output_capacity;
    memcpy(m_OutputBuffer.data(), m_BodyBuffer.data(), copy);
    m_OutputBufferSize = static_cast<size_t>(copy);
    m_OutputOffset = 0;
    m_BodyBuffer.erase(0, copy);
    return copy;
  }

  int data_in_read = 0;
  int data_out_written = 0;
  int status = 0;
  bool handled = on_response_content_filter_fptr(
      m_BodyBuffer.data(), input_size,
      m_OutputBuffer.data(), output_capacity,
      data_in_read, data_out_written, status);

  // DSL did not handle this chunk: pass through unchanged (capped to staging
  // capacity; any overflow stays in m_BodyBuffer for the next call).
  if (!handled) {
    int copy = input_size < output_capacity ? input_size : output_capacity;
    memcpy(m_OutputBuffer.data(), m_BodyBuffer.data(), copy);
    m_OutputBufferSize = static_cast<size_t>(copy);
    m_OutputOffset = 0;
    m_BodyBuffer.erase(0, copy);
    return copy;
  }

  if (status == kFilterError) {
    return -2;
  }

  // Clamp reported sizes to actual buffers.
  if (data_in_read < 0) data_in_read = 0;
  if (data_in_read > input_size) data_in_read = input_size;
  if (data_out_written < 0) data_out_written = 0;
  if (data_out_written > output_capacity) data_out_written = output_capacity;

  m_BodyBuffer.erase(0, data_in_read);
  m_OutputBufferSize = static_cast<size_t>(data_out_written);
  m_OutputOffset = 0;
  return data_out_written;
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
    // Refill staging if empty and upstream body is available.
    if (m_OutputOffset >= m_OutputBufferSize) {
      if (m_BodyBuffer.empty()) break;
      int filtered = FilterBodyChunk();
      if (filtered < 0) return -1;
      if (filtered == 0) break;  // Filter made no progress; retry later.
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
