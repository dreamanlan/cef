// Copyright (c) 2026 The Chromium Embedded Framework Authors. All rights
// reserved. Use of this source code is governed by a BSD-style license that
// can be found in the LICENSE file.

#include "tests/cefclient/browser/my_response_filter.h"

#include <cstring>

#include "include/wrapper/cef_helpers.h"
#include "tests/cefclient/hostclr/HostCLR.h"

namespace {

// Must match cef_response_filter_status_t.
[[maybe_unused]] constexpr int kFilterDone = 0;
constexpr int kFilterNeedMoreData = 1;
constexpr int kFilterError = 2;

}  // namespace

MyResponseFilter::FilterStatus MyResponseFilter::Filter(
    void* data_in,
    size_t data_in_size,
    size_t& data_in_read,
    void* data_out,
    size_t data_out_size,
    size_t& data_out_written) {
  CEF_REQUIRE_IO_THREAD();

  data_in_read = 0;
  data_out_written = 0;

  // No DSL filter registered: pass through unchanged.
  if (!on_response_content_filter_fptr) {
    size_t copy = data_in_size < data_out_size ? data_in_size : data_out_size;
    if (copy > 0 && data_in && data_out) {
      memcpy(data_out, data_in, copy);
    }
    data_in_read = data_in_size;
    data_out_written = copy;
    return copy == data_in_size ? RESPONSE_FILTER_DONE : RESPONSE_FILTER_NEED_MORE_DATA;
  }

  int in_read = 0;
  int out_written = 0;
  int status = 0;
  bool handled = on_response_content_filter_fptr(
      data_in, static_cast<int>(data_in_size),
      data_out, static_cast<int>(data_out_size),
      in_read, out_written, status);

  // DSL did not handle this chunk: pass through unchanged.
  if (!handled) {
    size_t copy = data_in_size < data_out_size ? data_in_size : data_out_size;
    if (copy > 0 && data_in && data_out) {
      memcpy(data_out, data_in, copy);
    }
    data_in_read = data_in_size;
    data_out_written = copy;
    return copy == data_in_size ? RESPONSE_FILTER_DONE
                                : RESPONSE_FILTER_NEED_MORE_DATA;
  }

  if (status == kFilterError) {
    return RESPONSE_FILTER_ERROR;
  }

  data_in_read = static_cast<size_t>(in_read);
  data_out_written = static_cast<size_t>(out_written);

  return status == kFilterNeedMoreData ? RESPONSE_FILTER_NEED_MORE_DATA
                                       : RESPONSE_FILTER_DONE;
}
