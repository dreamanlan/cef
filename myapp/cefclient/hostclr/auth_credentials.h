// Copyright (c) 2024 The Chromium Embedded Framework Authors. All rights
// reserved. Use of this source code is governed by a BSD-style license that
// can be found in the LICENSE file.

#pragma once

#include "include/cef_auth_callback.h"
#include "include/cef_base.h"
#include "include/cef_browser.h"

// Shared implementation of CefRequestHandler::GetAuthCredentials. Used by both
// ClientHandler (managed windows) and DefaultClientHandler (chrome-UI-created
// unmanaged windows) so proxy/HTTP authentication behaves identically on all
// windows: the C#/DSL layer (on_get_auth_credentials_fptr) is consulted first,
// with a Windows credui + Credential Manager fallback when it declines.
//
// Contract:
//   * Must be called on the CEF IO thread (that is where GetAuthCredentials is
//     invoked and where CefAuthCallback::Continue/Cancel must run).
//   * Always returns true: the request is either completed synchronously here,
//     taken over asynchronously by managed code, or dispatched to the credui
//     fallback on the UI thread.
bool HandleGetAuthCredentials(CefRefPtr<CefBrowser> browser,
                              const CefString& origin_url,
                              bool isProxy,
                              const CefString& host,
                              int port,
                              const CefString& realm,
                              const CefString& scheme,
                              CefRefPtr<CefAuthCallback> callback);
