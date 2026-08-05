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
class MyResourceHandler : public CefResourceHandler,
                          public CefURLRequestClient {
 public:
  // |response_override| is a native-created writable CefResponse carrying
  // header overrides from the DSL side. May be empty (no overrides).
  // |replace_content| enables body filtering via on_response_content_filter;
  // when false, the handler only applies header overrides and passes the body
  // through unchanged.
  explicit MyResourceHandler(CefRefPtr<CefResponse> response_override,
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

  CefRefPtr<CefResponse> m_ResponseOverride;
  CefRefPtr<CefURLRequest> m_Request;
  CefRefPtr<CefCallback> m_OpenCallback;

  // Whether to run the body filter (on_response_content_filter) over the
  // response body. When false, the handler only applies header overrides and
  // passes the body through unchanged.
  bool m_ReplaceContent = true;

  // Streaming body state.
  std::string m_BodyBuffer;
  bool m_Completed = false;

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

  IMPLEMENT_REFCOUNTING(MyResourceHandler);
  DISALLOW_COPY_AND_ASSIGN(MyResourceHandler);
};

#endif  // CEF_TESTS_CEFCLIENT_BROWSER_MY_RESOURCE_HANDLER_H_
