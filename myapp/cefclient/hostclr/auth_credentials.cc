// Copyright (c) 2024 The Chromium Embedded Framework Authors. All rights
// reserved. Use of this source code is governed by a BSD-style license that
// can be found in the LICENSE file.

#include "myapp/cefclient/hostclr/auth_credentials.h"

#include <set>
#include <string>

#include "include/base/cef_bind.h"
#include "include/base/cef_callback.h"
#include "include/cef_task.h"
#include "include/wrapper/cef_closure_task.h"
#include "include/wrapper/cef_helpers.h"
#include "myapp/cefclient/hostclr/HostCLR.h"
#include "myapp/cefclient/hostclr/credui_prompt.h"
#include "myapp/cefclient/hostclr/native_callbacks.h"

namespace {

// --- Auth state tracking for the CredUI fallback ---------------------------
//
// A target key here identifies a (proxy vs origin, host, port, realm) tuple.
// The set records which targets already had credentials supplied at least
// once in the current process. A second GetAuthCredentials call for the same
// target therefore means "previous credentials failed": we treat that as a
// retry and let the credui fallback purge any stale saved entry before
// prompting again.
//
// All access is on the CEF IO thread (GetAuthCredentials and the native
// callback closure both run there), so no locking is needed.

std::set<std::string>& GetAuthSuppliedSet() {
  static auto* s_set = new std::set<std::string>();
  return *s_set;
}

std::string MakeAuthTargetKey(bool is_proxy,
                              const std::string& host,
                              int port,
                              const std::string& realm) {
  std::string key = is_proxy ? "CefClientProxyAuth:" : "CefClientOriginAuth:";
  key += host;
  key += ":";
  key += std::to_string(port);
  if (!is_proxy) {
    key += ":";
    key += realm;
  }
  return key;
}

bool HasAuthBeenSupplied(const std::string& key) {
  const auto& s = GetAuthSuppliedSet();
  return s.find(key) != s.end();
}

void MarkAuthSupplied(const std::string& key) {
  GetAuthSuppliedSet().insert(key);
}

// Trampoline that captures the browser (to grab the HWND on the UI thread)
// and then dispatches to RunCredUIFallback. Runs on the CEF UI thread.
void CredUIFallbackOnUiThread(CefRefPtr<CefBrowser> browser,
                              int64_t handle,
                              std::string target_key,
                              bool is_proxy,
                              std::string host,
                              int port,
                              std::string realm,
                              int attempt) {
  CEF_REQUIRE_UI_THREAD();
  void* hwnd = nullptr;
  if (browser) {
    auto host_ref = browser->GetHost();
    if (host_ref) {
      hwnd = reinterpret_cast<void*>(host_ref->GetWindowHandle());
    }
  }
  RunCredUIFallback(handle, target_key, is_proxy, host, port, realm, attempt,
                    hwnd);
}

}  // namespace

bool HandleGetAuthCredentials(CefRefPtr<CefBrowser> browser,
                              const CefString& origin_url,
                              bool isProxy,
                              const CefString& host,
                              int port,
                              const CefString& realm,
                              const CefString& scheme,
                              CefRefPtr<CefAuthCallback> callback) {
  CEF_REQUIRE_IO_THREAD();

  const std::string hostStr = host.ToString();
  const std::string realmStr = realm.ToString();
  const std::string schemeStr = scheme.ToString();
  const std::string originStr = origin_url.ToString();
  const std::string target_key =
      MakeAuthTargetKey(isProxy, hostStr, port, realmStr);
  const int attempt = HasAuthBeenSupplied(target_key) ? 1 : 0;
  const int browser_id = browser ? browser->GetIdentifier() : 0;

  printf_log(LOG_SEVERITY_INFO,
            "GetAuthCredentials: isProxy=%d host=%s port=%d realm=%s "
            "scheme=%s origin=%s attempt=%d target=%s",
            isProxy ? 1 : 0, hostStr.c_str(), port, realmStr.c_str(),
            schemeStr.c_str(), originStr.c_str(), attempt,
            target_key.c_str());

  // Park the CefAuthCallback in the generic native-callback registry so that
  // either the managed side (asynchronous DSL takeover) or our own credui
  // fallback (on the UI thread) can complete it later. The closure runs on
  // the IO thread which is where CefAuthCallback::Continue/Cancel must be
  // called.
  const std::string target_for_closure = target_key;
  const int64_t handle = RegisterNativeCallback(
      browser_id, TID_IO,
      [callback, target_for_closure](bool ok, const std::string& data,
                                     int /*code*/) {
        if (ok) {
          std::string user;
          std::string pass;
          const auto pos = data.find('\n');
          if (pos != std::string::npos) {
            user = data.substr(0, pos);
            pass = data.substr(pos + 1);
          } else {
            user = data;
          }
          MarkAuthSupplied(target_for_closure);
          callback->Continue(user, pass);
        } else {
          callback->Cancel();
        }
      },
      kJsDialogTimeoutMs);

  // Ask the C#/DSL layer for credentials (configured via set_web_auth).
  // Three-state contract (see HostCLR.h):
  //   handled == false                 -> DSL declined; run credui fallback.
  //   handled == true, user_len == 0   -> DSL took ownership; managed side
  //                                       will complete |handle| later via
  //                                       native_callback_complete.
  //   handled == true, user_len > 0    -> DSL supplied credentials
  //                                       synchronously; discard |handle|
  //                                       and Continue directly.
  if (on_get_auth_credentials_fptr) {
    const int kMaxAuthLen = 256;
    char user_buf[kMaxAuthLen + 1];
    char pass_buf[kMaxAuthLen + 1];
    int user_len = kMaxAuthLen;
    int pass_len = kMaxAuthLen;
    const bool handled = on_get_auth_credentials_fptr(
        browser.get(), /*frame=*/nullptr, isProxy, hostStr.c_str(), port,
        realmStr.c_str(), schemeStr.c_str(), originStr.c_str(), user_buf,
        user_len, pass_buf, pass_len, handle, attempt);
    if (handled) {
      if (user_len > 0 && user_len < kMaxAuthLen && pass_len >= 0 &&
          pass_len < kMaxAuthLen) {
        // Synchronous: use the buffers directly. Discard the parked handle.
        user_buf[user_len] = '\0';
        pass_buf[pass_len] = '\0';
        printf_log(LOG_SEVERITY_INFO,
                  "GetAuthCredentials: using credentials from C# layer "
                  "(user=%s, pass_len=%d)",
                  user_buf, pass_len);
        DiscardNativeCallback(handle);
        MarkAuthSupplied(target_key);
        callback->Continue(user_buf, pass_buf);
        return true;
      }
      // Asynchronous takeover: managed side owns |handle| now.
      printf_log(LOG_SEVERITY_INFO,
                "GetAuthCredentials: DSL took over handle=%lld",
                static_cast<long long>(handle));
      return true;
    }
    printf_log(LOG_SEVERITY_INFO,
              "GetAuthCredentials: DSL declined, running native credui "
              "fallback (attempt=%d)",
              attempt);
  } else {
    printf_log(LOG_SEVERITY_INFO,
              "GetAuthCredentials: no managed handler, running native credui "
              "fallback (attempt=%d)",
              attempt);
  }

  // Fallback: Credential Manager + Windows credui prompt on the UI thread.
  CefPostTask(TID_UI,
              base::BindOnce(&CredUIFallbackOnUiThread, browser, handle,
                             target_key, isProxy, hostStr, port, realmStr,
                             attempt));
  return true;
}
