// Copyright (c) 2026 The Chromium Embedded Framework Authors. All rights
// reserved. Use of this source code is governed by a BSD-style license that
// can be found in the LICENSE file.

#ifndef CEF_TESTS_CEFCLIENT_BROWSER_MY_RESOURCE_HANDLER_H_
#define CEF_TESTS_CEFCLIENT_BROWSER_MY_RESOURCE_HANDLER_H_
#pragma once

#include <vector>

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
class MyResourceHandler : public CefResourceHandler,
                          public CefURLRequestClient {
 public:
  // |response_override| is a native-created writable CefResponse carrying
  // header overrides from the DSL side. May be empty (no overrides).
  // |request_context| is the browser's request context so the forwarded
  // CefURLRequest shares the browser's cookie store; may be empty (falls
  // back to the global request context).
  // |replace_content| enables body filtering via on_response_content_filter;
  // when false, the handler only applies header overrides and passes the body
  // through unchanged.
  MyResourceHandler(CefRefPtr<CefResponse> response_override,
                    CefRefPtr<CefRequestContext> request_context,
                    bool replace_content);

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
                          CefRefPtr<CefAuthCallback> callback) override {
    return false;
  }

 private:
  // Run the DSL body filter over the pending upstream buffer, filling the
  // output staging buffer. Input is NOT capped to chromium's read size (DSL
  // sees the full buffered chunk); output is staged in m_OutputBuffer (4MB).
  // Returns number of bytes written to staging (>=0) or a negative value on
  // error (matches CefResourceHandler::Read error convention).
  int FilterBodyChunk();

  // Serve up to |bytes_to_read| bytes from the output staging buffer into
  // |data_out|. Returns number of bytes served (>=0). Resets staging when
  // fully drained.
  int ServeFromStaging(void* data_out, int bytes_to_read);

  // IO-thread body of Open(). Posted from Open() because CefURLRequest::Create
  // requires the IO thread while Open() runs on a worker sequence.
  void CreateRequestOnIOThread(CefRefPtr<CefRequest> request,
                               CefRefPtr<CefCallback> callback);

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

  CefRefPtr<CefResponse> m_ResponseOverride;
  CefRefPtr<CefRequestContext> m_RequestContext;
  CefRefPtr<CefURLRequest> m_Request;
  CefRefPtr<CefCallback> m_OpenCallback;

  // Whether to run the body filter (on_response_content_filter) over the
  // response body. When false, the handler only applies header overrides and
  // passes the body through unchanged.
  bool m_ReplaceContent = true;

  // Streaming body state.
  std::string m_BodyBuffer;
  bool m_Completed = false;

  // Upstream error at completion (ERR_NONE on success). Used to fail reads
  // instead of reporting a clean EOF for a truncated body.
  int m_UpstreamError = 0;  // cef_errorcode_t

  // Set by Cancel() (IO thread). Tasks posted from the worker sequence may
  // run after Cancel(); they check this flag and bail out.
  bool m_Canceled = false;

  // Output staging buffer: decouples DSL output size from chromium's read
  // size. DSL writes up to m_OutputBuffer.size() bytes into staging; Read()
  // drains it to chromium in whatever chunk sizes chromium asks for.
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

#endif  // CEF_TESTS_CEFCLIENT_BROWSER_MY_RESOURCE_HANDLER_H_
