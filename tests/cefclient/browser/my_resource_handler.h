// Copyright (c) 2026 The Chromium Embedded Framework Authors. All rights
// reserved. Use of this source code is governed by a BSD-style license that
// can be found in the LICENSE file.

#ifndef CEF_TESTS_CEFCLIENT_BROWSER_MY_RESOURCE_HANDLER_H_
#define CEF_TESTS_CEFCLIENT_BROWSER_MY_RESOURCE_HANDLER_H_
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "include/cef_browser.h"
#include "include/cef_frame.h"
#include "include/cef_resource_handler.h"
#include "include/cef_urlrequest.h"

// Generic resource handler that forwards the upstream request via CefURLRequest
// and lets the DSL side (via on_resource_response_filter / on_response_content_filter)
// override response headers and filter the response body. Streaming: body data is
// buffered incrementally and passed through the body filter chunk by chunk.
// Not CSP-specific.
// Threading: Open()/Read()/Skip() are called on a worker sequence (not a
// dedicated thread) and defer to the IO thread via posted tasks. Everything
// else, including all mutable state below, is IO-thread only.
// Cookie-snapshot budget shared with base_client_handler (IO thread only).
// The generation invalidates handlers that were armed before DSL reset the
// budget (io_want_cookies <= 0). See HostCLR.h for the full contract.
extern int g_cookie_query_issued;
extern int g_cookie_query_generation;

class MyResourceHandler : public CefResourceHandler,
                          public CefURLRequestClient {
 public:
  // |upstream_request| is the mutable upstream request built by the caller
  // (base_client_handler): DSL may have edited its headers / referrer via
  // CefRequest setters before this handler is constructed. This copy is
  // forwarded directly via CefURLRequest without any further copying.
  // |response_override| is a native-created writable CefResponse carrying
  // header overrides from the DSL side. May be empty (no overrides).
  // |request_context| is retained for cookie-jar snapshot queries.
  // |frame| associates the forwarded URL request with its originating browser
  // frame. CreateRequestOnIOThread uses CefFrame::CreateURLRequest rather
  // than unaffiliated CefURLRequest::Create so the upstream load takes the
  // frame-associated URL loader/network observer path.
  // |replace_content| enables body filtering via on_response_content_filter;
  // when false, the handler only applies header overrides and passes the body
  // through unchanged.
  // |cookie_snapshot_limit| is the DSL-selected global snapshot cap. A
  // positive value requests a snapshot for this response only while the
  // shared issued-count remains below the cap; zero disables snapshots.
  // The query is issued when the upstream response headers arrive (the jar
  // has ingested the response's Set-Cookie by then) and the result is
  // delivered to C# via on_resource_cookie_list_fptr.
  // |browser| is retained only to provide a valid CefBrowser pointer during a
  // synchronous HTTP-auth challenge callback; no raw browser pointer is kept
  // in pending auth state after that callback returns.
  MyResourceHandler(CefRefPtr<CefRequest> upstream_request,
                    CefRefPtr<CefResponse> response_override,
                    CefRefPtr<CefRequestContext> request_context,
                    CefRefPtr<CefFrame> frame,
                    bool replace_content,
                    int cookie_snapshot_limit,
                    int cookie_snapshot_generation,
                    CefRefPtr<CefBrowser> browser);

  // CefResourceHandler methods.
  bool Open(CefRefPtr<CefRequest> request,
            bool& handle_request,
            CefRefPtr<CefCallback> callback) override;
  void GetResponseHeaders(CefRefPtr<CefResponse> response,
                          int64_t& response_length,
                          CefString& redirectUrl) override;
  bool Read(void* data_out,
            int bytes_to_read,
            int& bytes_read,
            CefRefPtr<CefResourceReadCallback> callback) override;
  bool Skip(int64_t bytes_to_skip,
            int64_t& bytes_skipped,
            CefRefPtr<CefResourceSkipCallback> callback) override;
  void Cancel() override;

  // CefURLRequestClient methods.
  void OnRequestComplete(CefRefPtr<CefURLRequest> request) override;
  void OnUploadProgress(CefRefPtr<CefURLRequest> request,
                        int64_t current,
                        int64_t total) override {}
  void OnDownloadProgress(CefRefPtr<CefURLRequest> request,
                          int64_t current,
                          int64_t total) override {}
  void OnDownloadData(CefRefPtr<CefURLRequest> request,
                      const void* data,
                      size_t data_length) override;
  bool GetAuthCredentials(bool isProxy,
                          const CefString& host,
                          int port,
                          const CefString& realm,
                          const CefString& scheme,
                          CefRefPtr<CefAuthCallback> callback) override;

 private:
  // Run the DSL body filter over pending upstream input, or with empty input
  // when |flush| is true at upstream completion. Fills m_OutputBuffer and
  // returns staged bytes (>=0) or a negative value on error. A flush that
  // returns NEED_MORE_DATA without output is a filter protocol error: no more
  // input can arrive, so accepting it would silently truncate the response.
  int FilterBodyChunk(bool flush);

  // Fill empty output staging from pending input or, after upstream completion,
  // drain any filter output requested by RESPONSE_FILTER_NEED_MORE_DATA.
  // Returns staged bytes (>=0) or a negative value on error.
  int FillOutputStaging();

  // Serve up to |bytes_to_read| bytes from the output staging buffer into
  // |data_out|. Returns number of bytes served (>=0). Resets staging when
  // fully drained.
  int ServeFromStaging(void* data_out, int bytes_to_read);

  // IO-thread body of Open(). Posted from Open() because URL request creation
  // requires a valid CEF task runner while Open() runs on a worker sequence.
  void CreateRequestOnIOThread(CefRefPtr<CefCallback> callback);

  // Poll the upstream response until its headers are available, then resolve
  // the pending Open() callback. CefURLRequestClient has no "response headers
  // available" notification in streaming mode, so polling is the only way to
  // learn about headers before the first body byte arrives (see the comment
  // in CreateRequestOnIOThread). Reposts itself on the IO thread until the
  // open decision is resolved, and must stop then: the posted task holds a
  // reference that would otherwise keep this handler alive forever.
  void PollResponseHeadersOnIOThread();

  // Grow the output staging buffer to at least |needed| bytes. Staging is
  // allocated lazily so that intercepting a resource does not cost a full
  // kOutputBufferSize allocation up front.
  void EnsureOutputBuffer(size_t needed);

  // IO-thread body of Read(). Posted from Read() so that all mutable state is
  // only ever touched on the IO thread; completes via |callback|.
  void ReadOnIOThread(void* data_out,
                      int bytes_to_read,
                      CefRefPtr<CefResourceReadCallback> callback);

  // IO-thread body of Skip(). Posted from Skip() for the same reason as
  // Read(); completes via |callback|.
  void SkipOnIOThread(int64_t bytes_to_skip,
                      CefRefPtr<CefResourceSkipCallback> callback);

  // Discard up to |count| bytes from the output stream: drains the staging
  // buffer, refilling it from the upstream body via FilterBodyChunk() as
  // needed. Returns the number of bytes discarded, or -1 on body filter
  // error. Skipping works on the FILTERED stream because chromium's skip
  // count refers to the body it sees.
  int64_t DiscardOutput(int64_t count);

  // Diagnostics for the authenticated-SPA reload loop: dumps what the
  // forwarded request actually carried (flags / site-for-cookies / Cookie
  // header). Called when the upstream status indicates the request was
  // rejected as unauthenticated. (The cookie-jar side moved to the
  // DSL-driven snapshot mechanism, see m_CookieSnapshotLimit.)
  void LogAuthFailureDiagnostics(int status);

  // Issue the DSL-requested cookie-jar snapshot query for the upstream URL.
  // Called once from GetResponseHeaders (upstream headers available, jar
  // has ingested the response's Set-Cookie).
  void IssueCookieSnapshotQuery(int status);

  CefRefPtr<CefRequest> m_UpstreamRequest;
  CefRefPtr<CefResponse> m_ResponseOverride;
  CefRefPtr<CefRequestContext> m_RequestContext;
  CefRefPtr<CefFrame> m_Frame;
  CefRefPtr<CefURLRequest> m_Request;
  CefRefPtr<CefCallback> m_OpenCallback;

  // Whether to run the body filter (on_response_content_filter) over the
  // response body. When false, the handler only applies header overrides and
  // passes the body through unchanged.
  bool m_ReplaceContent = true;

  // DSL-requested global cap for cookie-jar snapshots. The cap is checked
  // immediately before query issue, so concurrent intercepted requests cannot
  // overshoot it while waiting for upstream response headers.
  int m_CookieSnapshotLimit = 0;
  int m_CookieSnapshotGeneration = 0;

  // Keeps the originating browser alive until this handler is canceled or
  // completes. Its raw pointer is passed to C# only while
  // on_resource_auth_challenge_fptr is executing.
  CefRefPtr<CefBrowser> m_Browser;

  // Streaming body state. m_BodyBuffer has a fixed upper bound enforced in
  // OnDownloadData so a stalled browser-side reader or an endless response
  // cannot grow memory without limit.
  std::string m_BodyBuffer;
  bool m_Completed = false;
  bool m_FilterNeedsFlush = false;
  bool m_StreamError = false;

  // Upstream error at completion (ERR_NONE on success). Used to fail reads
  // instead of reporting a clean EOF for a truncated body. A stopped 3xx
  // redirect reports ERR_ABORTED upstream by design; it is normalized to
  // success because its original status and Location are returned to Chromium.
  int m_UpstreamError = 0;  // cef_errorcode_t

  // Set by Cancel() (IO thread). Tasks posted from the worker sequence may
  // run after Cancel(); they check this flag and bail out.
  bool m_Canceled = false;

  // CEF may re-challenge after invalid credentials. Limit each forwarded
  // request to a small number of prompts so wrong credentials cannot create
  // an unbounded UI/retry loop.
  int m_AuthChallengeCount = 0;

  // Output staging buffer: decouples DSL output size from chromium's read
  // size. DSL writes up to m_OutputBuffer.size() bytes into staging; Read()
  // drains it to chromium in whatever chunk sizes chromium asks for.
  // Allocated lazily by EnsureOutputBuffer(): pass-through only needs room
  // for the current chunk, while an active DSL body filter gets the full
  // kOutputBufferSize so that it can expand the content.
  std::vector<char> m_OutputBuffer;
  size_t m_OutputBufferSize = 0;  // Valid bytes currently in staging.
  size_t m_OutputOffset = 0;      // Read offset within staging.

  // Pending Read() request waiting for body data.
  CefRefPtr<CefResourceReadCallback> m_ReadCallback;
  void* m_ReadDataOut = nullptr;
  int m_ReadBytesToRead = 0;

  // Pending Skip() request waiting for body data. Skip and Read are never
  // pending at the same time (chromium sequences stream operations).
  CefRefPtr<CefResourceSkipCallback> m_SkipCallback;
  int64_t m_SkipRemaining = 0;  // Bytes left to skip for the current Skip().
  int64_t m_SkipTotal = 0;      // Bytes skipped so far for the current Skip().

  IMPLEMENT_REFCOUNTING(MyResourceHandler);
  DISALLOW_COPY_AND_ASSIGN(MyResourceHandler);
};

// Thread-safe native entry point used by the C# HostApi bridge. It copies
// arguments and resolves the matching pending forwarded-request HTTP auth
// challenge on the CEF IO thread. Unknown, expired, canceled, or previously
// resolved IDs are intentionally ignored.
void ReplyResourceAuthCredentials(uint64_t challenge_id,
                                  bool accepted,
                                  const std::string& username,
                                  const std::string& password);

#endif  // CEF_TESTS_CEFCLIENT_BROWSER_MY_RESOURCE_HANDLER_H_
