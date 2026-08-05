// Copyright (c) 2026 The Chromium Embedded Framework Authors. All rights
// reserved. Use of this source code is governed by a BSD-style license that
// can be found in the LICENSE file.

#include "tests/cefclient/browser/my_resource_handler.h"

#include "include/cef_parser.h"
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

MyResourceHandler::MyResourceHandler(CefRefPtr<CefResponse> response_override,
                                     bool replace_content)
    : m_ResponseOverride(response_override),
      m_ReplaceContent(replace_content),
      m_OutputBuffer(kOutputBufferSize) {}

bool MyResourceHandler::Open(CefRefPtr<CefRequest> request,
                             bool& handle_request,
                             CefRefPtr<CefCallback> callback) {
  CEF_REQUIRE_IO_THREAD();

  // Forward the request via CefURLRequest. The same CefRequest object is
  // reused so cookies / auth / headers carry over automatically.
  m_Request = CefURLRequest::Create(request, this, nullptr);
  if (!m_Request) {
    handle_request = false;
    return false;
  }

  // Defer the open decision until we receive the first body chunk (or the
  // request completes with an empty body). Chromium will call
  // GetResponseHeaders after we run |callback|.
  m_OpenCallback = callback;
  handle_request = false;
  return true;
}

void MyResourceHandler::GetResponseHeaders(CefRefPtr<CefResponse> response,
                                           int64_t& response_length,
                                           CefString& redirectUrl) {
  CEF_REQUIRE_IO_THREAD();

  // Copy upstream response metadata into the chromium-provided response.
  CefRefPtr<CefResponse> upstream = m_Request ? m_Request->GetResponse() : nullptr;
  if (upstream) {
    response->SetStatus(upstream->GetStatus());
    response->SetStatusText(upstream->GetStatusText());
    response->SetMimeType(upstream->GetMimeType());
    response->SetCharset(upstream->GetCharset());

    CefResponse::HeaderMap headers;
    upstream->GetHeaderMap(headers);
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
  // Read() until we return false.
  response_length = -1;
}

bool MyResourceHandler::Read(void* data_out,
                             int bytes_to_read,
                             int& bytes_read,
                             CefRefPtr<CefResourceReadCallback> callback) {
  CEF_REQUIRE_IO_THREAD();

  // Refill staging if empty and upstream body is available.
  if (m_OutputOffset >= m_OutputBufferSize && !m_BodyBuffer.empty()) {
    int filtered = FilterBodyChunk();
    if (filtered < 0) {
      bytes_read = -2;  // ERR_FAILED
      return false;
    }
  }

  // Serve from staging.
  int served = ServeFromStaging(data_out, bytes_to_read);
  if (served > 0) {
    bytes_read = served;
    return true;
  }

  if (m_Completed) {
    // All body data has been delivered.
    bytes_read = 0;
    return false;
  }

  // No buffered data yet; defer until OnDownloadData or OnRequestComplete.
  bytes_read = 0;
  m_ReadCallback = callback;
  m_ReadDataOut = data_out;
  m_ReadBytesToRead = bytes_to_read;
  return true;
}

void MyResourceHandler::Cancel() {
  CEF_REQUIRE_IO_THREAD();
  if (m_Request) {
    m_Request->Cancel();
    m_Request = nullptr;
  }
  m_ReadCallback = nullptr;
  m_ReadDataOut = nullptr;
}

void MyResourceHandler::OnRequestComplete(CefRefPtr<CefURLRequest> request) {
  CEF_REQUIRE_IO_THREAD();
  m_Completed = true;

  // If Open() is still waiting (empty-body case), unblock chromium now.
  if (m_OpenCallback) {
    m_OpenCallback->Continue();
    m_OpenCallback = nullptr;
  }

  // If a Read() is pending and no more data will arrive, signal completion.
  if (m_ReadCallback && m_BodyBuffer.empty() &&
      m_OutputOffset >= m_OutputBufferSize) {
    auto callback = m_ReadCallback;
    m_ReadCallback = nullptr;
    m_ReadDataOut = nullptr;
    callback->Continue(0);
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
