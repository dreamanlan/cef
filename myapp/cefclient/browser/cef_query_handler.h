// Copyright (c) 2013 The Chromium Embedded Framework Authors. All rights
// reserved. Use of this source code is governed by a BSD-style license that
// can be found in the LICENSE file.

#ifndef CEF_TESTS_CEFCLIENT_BROWSER_CEF_QUERY_HANDLER_H_
#define CEF_TESTS_CEFCLIENT_BROWSER_CEF_QUERY_HANDLER_H_
#pragma once

#include "myapp/cefclient/browser/test_runner.h"

namespace client::cef_query_handler {

// Structure to hold file copy information
struct FileCopyInfo {
  std::string source;
  std::string dest;
};

// Create message handlers. Called from test_runner.cc.
void CreateMessageHandlers(test_runner::MessageHandlerSet& handlers);

}  // namespace client::cef_query_handler

#endif  // CEF_TESTS_CEFCLIENT_BROWSER_CEF_QUERY_HANDLER_H_
