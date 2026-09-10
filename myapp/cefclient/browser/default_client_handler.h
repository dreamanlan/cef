// Copyright (c) 2023 The Chromium Embedded Framework Authors. All rights
// reserved. Use of this source code is governed by a BSD-style license that
// can be found in the LICENSE file.

#ifndef CEF_TESTS_CEFCLIENT_BROWSER_DEFAULT_CLIENT_HANDLER_H_
#define CEF_TESTS_CEFCLIENT_BROWSER_DEFAULT_CLIENT_HANDLER_H_
#pragma once

#include <optional>

#include "include/cef_display_handler.h"
#include "myapp/cefclient/browser/base_client_handler.h"
#if !defined(OS_LINUX)
#include "myapp/cefclient/hostclr/js_dialog_handler.h"
#endif

namespace client {

// Default client handler for unmanaged browser windows. Used with Chrome
// style only.
class DefaultClientHandler : public BaseClientHandler,
                             public CefDisplayHandler,
                             public CefPermissionHandler {
 public:
  // If |use_alloy_style| is nullopt the global default will be used.
  explicit DefaultClientHandler(
      std::optional<bool> use_alloy_style = std::nullopt,
      const std::string& startup_url = std::string());

  DefaultClientHandler(const DefaultClientHandler&) = delete;
  DefaultClientHandler& operator=(const DefaultClientHandler&) = delete;

  // Returns the DefaultClientHandler for |client|, or nullptr if |client| is
  // not a DefaultClientHandler.
  static CefRefPtr<DefaultClientHandler> GetForClient(
      CefRefPtr<CefClient> client);

  // CefClient methods. Route Display / Permission / JSDialog back to us so
  // chrome-UI-created (unmanaged) windows forward the same C# / DSL callbacks
  // that ClientHandler exposes on managed windows.
  CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
  CefRefPtr<CefPermissionHandler> GetPermissionHandler() override {
    return this;
  }
#if !defined(OS_LINUX)
  CefRefPtr<CefJSDialogHandler> GetJSDialogHandler() override {
    return managed_js_dialog_handler_;
  }
#endif

 protected:
  bool OnBeforePopup(
      CefRefPtr<CefBrowser> browser,
      CefRefPtr<CefFrame> frame,
      int popup_id,
      const CefString& target_url,
      const CefString& target_frame_name,
      CefLifeSpanHandler::WindowOpenDisposition target_disposition,
      bool user_gesture,
      const CefPopupFeatures& popupFeatures,
      CefWindowInfo& windowInfo,
      CefRefPtr<CefClient>& client,
      CefBrowserSettings& settings,
      CefRefPtr<CefDictionaryValue>& extra_info,
      bool* no_javascript_access) override;
  void OnBeforePopupAborted(CefRefPtr<CefBrowser> browser,
                            int popup_id) override;
  void OnBeforeClose(CefRefPtr<CefBrowser> browser) override;

  // CefDisplayHandler methods. Forward console output to the shared C# / DSL
  // callback; unlike ClientHandler this handler owns no console log file.
  bool OnConsoleMessage(CefRefPtr<CefBrowser> browser,
                        cef_log_severity_t level,
                        const CefString& message,
                        const CefString& source,
                        int line) override;

  // CefPermissionHandler methods
  bool OnRequestMediaAccessPermission(
      CefRefPtr<CefBrowser> browser,
      CefRefPtr<CefFrame> frame,
      const CefString& requesting_origin,
      uint32_t requested_permissions,
      CefRefPtr<CefMediaAccessCallback> callback) override;
  bool OnShowPermissionPrompt(
      CefRefPtr<CefBrowser> browser,
      uint64_t prompt_id,
      const CefString& requesting_origin,
      uint32_t requested_permissions,
      CefRefPtr<CefPermissionPromptCallback> callback) override;

  // CefRequestHandler methods
  bool GetAuthCredentials(CefRefPtr<CefBrowser> browser,
                          const CefString& origin_url,
                          bool isProxy,
                          const CefString& host,
                          int port,
                          const CefString& realm,
                          const CefString& scheme,
                          CefRefPtr<CefAuthCallback> callback) override;
  bool OnCertificateError(CefRefPtr<CefBrowser> browser,
                          ErrorCode cert_error,
                          const CefString& request_url,
                          CefRefPtr<CefSSLInfo> ssl_info,
                          CefRefPtr<CefCallback> callback) override;

 private:
  // Used to determine the object type.
  virtual const void* GetTypeKey() const override { return &kTypeKey; }
  static constexpr int kTypeKey = 0;

#if !defined(OS_LINUX)
  // Forwards JS dialogs (alert/confirm/prompt/beforeunload) to managed code,
  // same as ClientHandler on managed windows.
  CefRefPtr<ClientJSDialogHandler> managed_js_dialog_handler_;
#endif

  const bool use_alloy_style_;

  IMPLEMENT_REFCOUNTING(DefaultClientHandler);
};

}  // namespace client

#endif  // CEF_TESTS_CEFCLIENT_BROWSER_DEFAULT_CLIENT_HANDLER_H_
