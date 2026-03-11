// Copyright (c) 2015 The Chromium Embedded Framework Authors. All rights
// reserved. Use of this source code is governed by a BSD-style license that
// can be found in the LICENSE file.

#include "tests/shared/common/client_app_other.h"

#include "include/cef_command_line.h"
#include "tests/cefclient/hostclr/HostCLR.h"

namespace client {

ClientAppOther::ClientAppOther() = default;

void ClientAppOther::OnBeforeCommandLineProcessing(
    const CefString& process_type,
    CefRefPtr<CefCommandLine> command_line) {
  if (on_before_command_line_processing_fptr) {
    int pt = static_cast<int>(ClientApp::GetProcessType(command_line));
    on_before_command_line_processing_fptr(pt, command_line.get());
  }
}

}  // namespace client
