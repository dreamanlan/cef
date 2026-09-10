// Copyright (c) 2024 The Chromium Embedded Framework Authors. All rights
// reserved. Use of this source code is governed by a BSD-style license that
// can be found in the LICENSE file.

#include "myapp/cefclient/browser/default_client_handler.h"

#include "myapp/cefclient/browser/main_context.h"
#include "myapp/cefclient/browser/root_window_manager.h"
#include "myapp/cefclient/hostclr/HostCLR.h"
#include "myapp/cefclient/hostclr/auth_credentials.h"

namespace client {

DefaultClientHandler::DefaultClientHandler(std::optional<bool> use_alloy_style,
                                           const std::string& startup_url)
    : BaseClientHandler(startup_url),
      use_alloy_style_(
          use_alloy_style.value_or(MainContext::Get()->UseAlloyStyleGlobal())) {
#if !defined(OS_LINUX)
  managed_js_dialog_handler_ = new ClientJSDialogHandler();
#endif
}

// static
CefRefPtr<DefaultClientHandler> DefaultClientHandler::GetForClient(
    CefRefPtr<CefClient> client) {
  auto base = BaseClientHandler::GetForClient(client);
  if (base && base->GetTypeKey() == &kTypeKey) {
    return static_cast<DefaultClientHandler*>(base.get());
  }
  return nullptr;
}

bool DefaultClientHandler::OnBeforePopup(
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
    bool* no_javascript_access) {
  CEF_REQUIRE_UI_THREAD();

  if (target_disposition == CEF_WOD_NEW_PICTURE_IN_PICTURE) {
    // Use default handling for document picture-in-picture popups.
    client = nullptr;
    return false;
  }

  // --use-chrome-window: the initial window is a Chrome self-created native
  // (tabstrip) window (see RootWindowManager::CreateChromeWindow). Popups /
  // window.open must replicate that recipe instead of falling into the native
  // RootWindow popup path, which would host the Chrome browser inside a bare
  // Win32 window (ugly). Mirror the initial-window config: no client RootWindow,
  // Chrome runtime style, and a DefaultClientHandler (chrome style) so the popup
  // joins the same C# / DSL browser-query system. Let Chrome create the window.
  if (MainContext::Get()->UseChromeWindowGlobal()) {
    windowInfo.runtime_style = CEF_RUNTIME_STYLE_CHROME;
    if (!DefaultClientHandler::GetForClient(client)) {
      client = new DefaultClientHandler(/*use_alloy_style=*/false);
    }
    return false;
  }

  // Used to configure default values.
  RootWindowConfig config(/*command_line=*/nullptr);

  // Potentially create a new RootWindow for the popup browser that will be
  // created asynchronously.
  MainContext::Get()->GetRootWindowManager()->CreateRootWindowAsPopup(
      config.use_views, use_alloy_style_, config.with_controls,
      /*is_osr=*/false, browser->GetIdentifier(), popup_id,
      /*is_devtools=*/false, popupFeatures, windowInfo, client, settings);

  // Allow popup creation.
  return false;
}

void DefaultClientHandler::OnBeforePopupAborted(CefRefPtr<CefBrowser> browser,
                                                int popup_id) {
  CEF_REQUIRE_UI_THREAD();
  MainContext::Get()->GetRootWindowManager()->AbortOrClosePopup(
      browser->GetIdentifier(), popup_id);
}

void DefaultClientHandler::OnBeforeClose(CefRefPtr<CefBrowser> browser) {
  CEF_REQUIRE_UI_THREAD();

  // Close all popups that have this browser as the opener.
  //
  // This opener-close -> cascade-close-children behavior is only appropriate
  // for dependent popups (e.g. `--use-default-popup`). Under
  // `--use-chrome-window`, window.open produces a full Chrome-managed
  // tab/window that the user can drag out into its own top-level window; just
  // like real Chrome, closing the opener must NOT force-close those. Doing so
  // also mis-fires because the opener->child ownership map is not updated when
  // a tab is detached, so a dragged-out window would be wrongly closed and can
  // drive other_browser_ct_ to 0, terminating the whole browser.
  if (!MainContext::Get()->UseChromeWindowGlobal()) {
    OnBeforePopupAborted(browser, /*popup_id=*/-1);
  }

  BaseClientHandler::OnBeforeClose(browser);
}

bool DefaultClientHandler::OnShowPermissionPrompt(
    CefRefPtr<CefBrowser> browser,
    uint64_t prompt_id,
    const CefString& requesting_origin,
    uint32_t requested_permissions,
    CefRefPtr<CefPermissionPromptCallback> callback) {
  CEF_REQUIRE_UI_THREAD();
  // Delegate to the shared DSL bridge (same as ClientHandler) so unmanaged /
  // chrome-style popup / overlay windows share the same policy source.
  return BaseClientHandler::MaybeHandlePermissionPromptViaDSL(
      browser, prompt_id, requesting_origin, requested_permissions, callback);
}

bool DefaultClientHandler::OnConsoleMessage(CefRefPtr<CefBrowser> browser,
                                            cef_log_severity_t level,
                                            const CefString& message,
                                            const CefString& source,
                                            int line) {
  CEF_REQUIRE_UI_THREAD();

  // Forward console output to the shared C# / DSL callback so chrome-UI-created
  // (unmanaged) windows report the same way managed windows do. Unlike
  // ClientHandler this handler owns no console log file, so it only forwards
  // and always falls through to default CEF handling.
  if (on_console_log_fptr) {
    int max_log_size = 128 * 1024;
    std::string msg_str = message.ToString();
    std::string src_str = source.ToString();
    // CEF does not provide a frame here; pass browser->GetMainFrame() so C#
    // can set NativeApi context with the same (browser, frame) convention.
    CefRefPtr<CefFrame> main_frame =
        browser ? browser->GetMainFrame() : nullptr;
    on_console_log_fptr(browser.get(), main_frame.get(), (int)level,
                        msg_str.c_str(), src_str.c_str(), line, max_log_size);
  }

  return false;
}

bool DefaultClientHandler::OnRequestMediaAccessPermission(
    CefRefPtr<CefBrowser> browser,
    CefRefPtr<CefFrame> frame,
    const CefString& requesting_origin,
    uint32_t requested_permissions,
    CefRefPtr<CefMediaAccessCallback> callback) {
  CEF_REQUIRE_UI_THREAD();

  // Forward to the shared C# / DSL media-permission callback. Unmanaged windows
  // have no UI kill-switch, so |media_handling_disabled| is passed as false;
  // when the DSL declines, fall through to the Chromium (chrome style) prompt.
  if (on_request_media_access_permission_fptr) {
    std::string originStr = requesting_origin.ToString();
    uint32_t allowed = 0;
    bool handled = on_request_media_access_permission_fptr(
        browser.get(), frame.get(), originStr.c_str(), requested_permissions,
        /*media_handling_disabled=*/false, &allowed);
    if (handled) {
      // Clamp to the requested set; CEF ignores extra bits but be defensive.
      callback->Continue(allowed & requested_permissions);
      return true;
    }
  }

  return false;
}

bool DefaultClientHandler::GetAuthCredentials(
    CefRefPtr<CefBrowser> browser,
    const CefString& origin_url,
    bool isProxy,
    const CefString& host,
    int port,
    const CefString& realm,
    const CefString& scheme,
    CefRefPtr<CefAuthCallback> callback) {
  // Shared with ClientHandler so managed and unmanaged windows behave
  // identically. See hostclr/auth_credentials.cc.
  return HandleGetAuthCredentials(browser, origin_url, isProxy, host, port,
                                  realm, scheme, callback);
}

bool DefaultClientHandler::OnCertificateError(
    CefRefPtr<CefBrowser> browser,
    ErrorCode cert_error,
    const CefString& request_url,
    CefRefPtr<CefSSLInfo> ssl_info,
    CefRefPtr<CefCallback> callback) {
  CEF_REQUIRE_UI_THREAD();

  // Same policy as ClientHandler: the C# / DSL override has highest priority.
  if (on_certificate_error_fptr) {
    std::string urlStr = request_url.ToString();
    int action = 0;
    // CEF does not provide a frame here; pass browser->GetMainFrame() so C#
    // can set NativeApi context with the same (browser, frame) convention.
    CefRefPtr<CefFrame> main_frame =
        browser ? browser->GetMainFrame() : nullptr;
    bool handled = on_certificate_error_fptr(
        browser.get(), main_frame.get(), static_cast<int>(cert_error),
        urlStr.c_str(), &action);
    if (handled) {
      if (action == 1) {           // Continue: silently proceed.
        callback->Continue();
        return true;
      }
      if (action == 2) {           // Cancel: silently cancel.
        callback->Cancel();
        return true;
      }
      // action == 0 or unknown -> fall through to the default interstitial.
    }
  }

  // Let Chromium show its default certificate-error interstitial.
  return false;
}

}  // namespace client
