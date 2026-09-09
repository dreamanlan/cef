// Copyright (c) 2026 The Chromium Embedded Framework Authors. All rights
// reserved. Use of this source code is governed by a BSD-style license that
// can be found in the LICENSE file.

#ifndef CEF_TESTS_CEFCLIENT_BROWSER_MY_RESPONSE_FILTER_H_
#define CEF_TESTS_CEFCLIENT_BROWSER_MY_RESPONSE_FILTER_H_
#pragma once

#include "include/cef_response_filter.h"

// CefResponseFilter implementation that forwards each body chunk to the DSL
// side via on_response_content_filter_fptr for transformation. Registered
// when on_resource_response_filter (inspection mode) returns true.
class MyResponseFilter : public CefResponseFilter {
 public:
  MyResponseFilter() = default;

  // CefResponseFilter methods.
  bool InitFilter() override { return true; }
  FilterStatus Filter(void* data_in,
                      size_t data_in_size,
                      size_t& data_in_read,
                      void* data_out,
                      size_t data_out_size,
                      size_t& data_out_written) override;

 private:
  IMPLEMENT_REFCOUNTING(MyResponseFilter);
  DISALLOW_COPY_AND_ASSIGN(MyResponseFilter);
};

#endif  // CEF_TESTS_CEFCLIENT_BROWSER_MY_RESPONSE_FILTER_H_
