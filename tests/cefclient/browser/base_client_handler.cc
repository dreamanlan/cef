// Copyright (c) 2024 The Chromium Embedded Framework Authors. All rights
// reserved. Use of this source code is governed by a BSD-style license that
// can be found in the LICENSE file.

#include "tests/cefclient/browser/base_client_handler.h"

#include <sstream>

#include "include/cef_command_line.h"
#include "include/cef_parser.h"
#include "tests/cefclient/browser/main_context.h"
#include "tests/cefclient/browser/my_resource_handler.h"
#include "tests/cefclient/browser/my_response_filter.h"
#include "tests/cefclient/browser/root_window_manager.h"
#include "tests/cefclient/hostclr/HostCLR.h"
#include "tests/shared/common/client_switches.h"
#include "tests/shared/common/string_util.h"

namespace client {

namespace {

std::string GetDataURI(const std::string& data, const std::string& mime_type) {
  return "data:" + mime_type + ";base64," +
         CefURIEncode(CefBase64Encode(data.data(), data.size()), false)
             .ToString();
}

}  // namespace

// static
bool BaseClientHandler::inject_all_frame_ = false;

BaseClientHandler::BaseClientHandler(const std::string& startup_url)
    : startup_url_(startup_url) {
  resource_manager_ = new CefResourceManager();
  test_runner::SetupResourceManager(resource_manager_, &string_resource_map_);
}

// static
CefRefPtr<BaseClientHandler> BaseClientHandler::GetForBrowser(
    CefRefPtr<CefBrowser> browser) {
  return GetForClient(browser->GetHost()->GetClient());
}

// static
CefRefPtr<BaseClientHandler> BaseClientHandler::GetForClient(
    CefRefPtr<CefClient> client) {
  return static_cast<BaseClientHandler*>(client.get());
}

bool BaseClientHandler::OnProcessMessageReceived(
    CefRefPtr<CefBrowser> browser,
    CefRefPtr<CefFrame> frame,
    CefProcessId source_process,
    CefRefPtr<CefProcessMessage> message) {
  CEF_REQUIRE_UI_THREAD();

  if (message_router_->OnProcessMessageReceived(browser, frame,
                                                source_process, message)) {
    return true;
  }

  if (on_receive_cef_message_fptr) {
    const std::string& message_name = message->GetName();
    size_t size = message->GetArgumentList()->GetSize();
    std::vector<std::string> args_vec;
    std::vector<const char*> args_ptrs;
    for (size_t i = 0; i < size; i++) {
      args_vec.push_back(message->GetArgumentList()->GetString(i).ToString());
    }
    for (const auto& arg : args_vec) {
      args_ptrs.push_back(arg.c_str());
    }
    on_receive_cef_message_fptr(message_name.c_str(),
        args_ptrs.empty() ? nullptr : args_ptrs.data(),
        static_cast<int>(size), browser.get(), frame.get(),
        static_cast<int>(source_process));
    return true;
  }

  return false;
}

bool BaseClientHandler::OnSetFocus(CefRefPtr<CefBrowser> browser,
                                   FocusSource source) {
  return !ShouldRequestFocus();
}

void BaseClientHandler::OnMainFrameChanged(CefRefPtr<CefBrowser> browser,
                                           CefRefPtr<CefFrame> /*old_frame*/,
                                           CefRefPtr<CefFrame> new_frame) {
  CEF_REQUIRE_UI_THREAD();
  // Keep the browser-process ref map's stored main frame in sync with
  // cross-origin navigations and renderer crash recovery. If this fires before
  // OnAfterCreated (allowed on initial creation), browser_ref_update_frame
  // finds no entry and no-ops; browser_ref_add will then capture the current
  // main frame when the entry is created. |new_frame| may be null during final
  // destruction -- that's tolerated (entry gets erased by browser_ref_remove).
  browser_ref_update_frame(browser, new_frame);
}

void BaseClientHandler::OnAfterCreated(CefRefPtr<CefBrowser> browser) {
  CEF_REQUIRE_UI_THREAD();

  // Register the browser in the browser-process ref map BEFORE any C#
  // callback fires, so C# can immediately call browser_is_valid / touch the
  // browser raw pointer without hitting a stale reference.
  browser_ref_add(browser);

  browser_count_++;

  int opener_id = browser->GetHost()->GetOpenerIdentifier();
  const char* browser_type = (opener_id >= 0) ? "POPUP" : "MAIN";

  printf_log(LOG_SEVERITY_INFO,
            "OnAfterCreated: Browser %d created, browser_count_=%d, type=%s, opener_id=%d",
            browser->GetIdentifier(), browser_count_, browser_type, opener_id);

  if (!message_router_) {
    // Create the browser-side router for query handling.
    CefMessageRouterConfig config;
    message_router_ = CefMessageRouterBrowserSide::Create(config);

    // Register handlers with the router.
    test_runner::CreateMessageHandlers(message_handler_set_);
    for (auto* message_handler : message_handler_set_) {
      message_router_->AddHandler(message_handler, false);
    }
  }

  if (track_as_other_browser_) {
    MainContext::Get()->GetRootWindowManager()->OtherBrowserCreated(
        browser->GetIdentifier(), browser->GetHost()->GetOpenerIdentifier());
  }

  if (on_browser_init_fptr) {
    on_browser_init_fptr(browser.get());
  }

  // Auto-register a DevTools message observer for this browser. The
  // registration is released in OnBeforeClose.
  RegisterDevToolsObserver(browser.get());
}

void BaseClientHandler::OnBeforeClose(CefRefPtr<CefBrowser> browser) {
  CEF_REQUIRE_UI_THREAD();

  printf_log(LOG_SEVERITY_INFO, "OnBeforeClose: Browser %d closing",
            browser->GetIdentifier());

  // Release the DevTools observer registration for this browser.
  UnregisterDevToolsObserver(browser.get());

  if (on_browser_finalize_fptr) {
    printf_log(LOG_SEVERITY_INFO,
              "OnBeforeClose: Calling on_browser_finalize_fptr");
    on_browser_finalize_fptr(browser.get());
  }

  if (--browser_count_ == 0) {
    // Remove and delete message router handlers.
    for (auto* message_handler : message_handler_set_) {
      message_router_->RemoveHandler(message_handler);
      delete message_handler;
    }
    message_handler_set_.clear();
    message_router_ = nullptr;
  }

  if (track_as_other_browser_) {
    MainContext::Get()->GetRootWindowManager()->OtherBrowserClosed(
        browser->GetIdentifier(), browser->GetHost()->GetOpenerIdentifier());
  }

  // Unregister from the browser-process ref map AFTER all C# callbacks have
  // fired. From this point on browser_is_valid returns false for this pointer
  // and the CefRefPtr in the map is dropped.
  browser_ref_remove(browser);
}

void BaseClientHandler::OnLoadingStateChange(CefRefPtr<CefBrowser> browser,
                                             bool isLoading,
                                             bool canGoBack,
                                             bool canGoForward) {
  CEF_REQUIRE_UI_THREAD();

  if (!isLoading && initial_navigation_) {
    initial_navigation_ = false;
  }

  printf_log(LOG_SEVERITY_INFO,
            "OnLoadingStateChange: Browser %d isLoading=%d canGoBack=%d "
            "canGoForward=%d",
            browser->GetIdentifier(), isLoading, canGoBack, canGoForward);

  if (on_loading_state_change_fptr) {
    CefRefPtr<CefFrame> frame = browser->GetMainFrame();
    std::string url_str;
    if (frame) {
      url_str = frame->GetURL();
    }
    on_loading_state_change_fptr(browser.get(), frame.get(),
        url_str.empty() ? "" : url_str.c_str(), isLoading, canGoBack,
        canGoForward);
  }
}

bool BaseClientHandler::OnBeforeBrowse(CefRefPtr<CefBrowser> browser,
                                       CefRefPtr<CefFrame> frame,
                                       CefRefPtr<CefRequest> request,
                                       bool user_gesture,
                                       bool is_redirect) {
  CEF_REQUIRE_UI_THREAD();

  // Capture startup_url_ from the first main frame navigation if not provided.
  if (startup_url_.empty() && frame->IsMain()) {
    std::string url = request->GetURL().ToString();
    if (!url.empty()) {
      startup_url_ = url;
      printf_log(LOG_SEVERITY_INFO,
                "OnBeforeBrowse: startup_url_ was empty, set to %s",
                startup_url_.c_str());
    }
  }

  // Redirect chrome://help/ to chrome://settings/help
  {
    std::string url = request->GetURL().ToString();
    if (url == "chrome://help/" || url == "chrome://help") {
      frame->LoadURL("chrome://settings/help");
      return true;
    }
  }

  if (on_before_browse_fptr) {
    bool out_return_value = false;
    if (on_before_browse_fptr(browser.get(), frame.get(), request.get(),
                              user_gesture, is_redirect, &out_return_value)) {
      message_router_->OnBeforeBrowse(browser, frame);
      return out_return_value;
    }
  }

  message_router_->OnBeforeBrowse(browser, frame);
  return false;
}

bool BaseClientHandler::OnRenderProcessUnresponsive(
    CefRefPtr<CefBrowser> browser,
    CefRefPtr<CefUnresponsiveProcessCallback> callback) {
  switch (hang_action_) {
    case HangAction::kDefault:
      return false;
    case HangAction::kWait:
      callback->Wait();
      break;
    case HangAction::kTerminate:
      callback->Terminate();
      break;
  }
  return true;
}

void BaseClientHandler::OnLoadStart(CefRefPtr<CefBrowser> browser,
                                    CefRefPtr<CefFrame> frame,
                                    TransitionType transition_type) {
  // If startup_url_ was not provided, capture it from the first main frame load.
  if (startup_url_.empty() && frame->IsMain()) {
    std::string url = frame->GetURL().ToString();
    if (!url.empty()) {
      startup_url_ = url;
      printf_log(LOG_SEVERITY_INFO,
                "OnLoadStart: startup_url_ was empty, set to %s",
                startup_url_.c_str());
    }
  }

  printf_log(LOG_SEVERITY_INFO,
            "OnLoadStart: Browser %d frame=%s transition_type=%d isMain=%d",
            browser->GetIdentifier(), frame->GetURL().ToString().c_str(),
            static_cast<int>(transition_type), frame->IsMain());

  if (on_load_start_fptr) {
    std::string url = frame->GetURL();
    on_load_start_fptr(browser.get(), frame.get(), url.c_str(),
        static_cast<int>(transition_type), frame->IsMain());
  }
}

void BaseClientHandler::OnLoadEnd(CefRefPtr<CefBrowser> browser,
                                  CefRefPtr<CefFrame> frame,
                                  int httpStatusCode) {
  printf_log(LOG_SEVERITY_INFO,
            "OnLoadEnd: Browser %d frame=%s httpStatusCode=%d "
            "inject_all_frame=%d isMain=%d",
            browser->GetIdentifier(), frame->GetURL().ToString().c_str(),
            httpStatusCode, inject_all_frame_, frame->IsMain());

  const int max_size = 4 * 1024 * 1024;
  if (inject_all_frame_ || frame->IsMain()) {
    char* buf = new char[max_size + 1];
    memset(buf, 0, max_size + 1);

    bool use_custom_code = false;
    if (on_load_end_fptr) {
      std::string url = frame->GetURL().ToString();
      int code_size = max_size;
      use_custom_code = on_load_end_fptr(browser.get(), frame.get(),
          url.c_str(), httpStatusCode, inject_all_frame_, frame->IsMain(),
          buf, code_size);
      if (use_custom_code && code_size > 0) {
        buf[code_size] = '\0';
      }
    }

    if (!use_custom_code) {
#if defined(__APPLE__)
      std::string baseDir = GetMacMainAppDirPath();
      std::string lastDirName = GetMacMainAppDirName();
      if (lastDirName == "cefclientdbg.app") {
        baseDir += "/../cefclient.app/Contents";
      } else {
        baseDir += "/Contents";
      }
#else
      std::string baseDir = GetExeDir();
      std::string lastDirName = GetExeLastDirName();
      if (lastDirName == "cefclientdbg") {
        baseDir += "/../cefclient";
      }
#endif
      std::string file = baseDir + "/managed/inject.js";
      FILE* fp = fopen(file.c_str(), "rb");
      if (fp != NULL) {
        fread(buf, 1, max_size, fp);
        fclose(fp);
      } else {
        std::string error_msg = "Failed to open inject.js from: " + file;
        printf_log(LOG_SEVERITY_ERROR, "%s", error_msg.c_str());
        delete[] buf;
        return;
      }
    }

    if (buf[0] != '\0') {
      frame->ExecuteJavaScript(buf, frame->GetURL(), 0);
    }
    delete[] buf;
  }
}

void BaseClientHandler::OnLoadError(CefRefPtr<CefBrowser> browser,
                                    CefRefPtr<CefFrame> frame,
                                    ErrorCode errorCode,
                                    const CefString& errorText,
                                    const CefString& failedUrl) {
  CEF_REQUIRE_UI_THREAD();

  // Don't display an error for downloaded files.
  if (errorCode == ERR_ABORTED) {
    return;
  }

  // Don't display an error for external protocols that we allow the OS to
  // handle. See OnProtocolExecution().
  if (errorCode == ERR_UNKNOWN_URL_SCHEME) {
    std::string urlStr = frame->GetURL();
    if (urlStr.find("spotify:") == 0) {
      return;
    }
  }

  // Display a load error message using a data: URI.
  std::stringstream ss;
  ss << "<html><body bgcolor=\"white\">"
        "<h2>Failed to load URL "
     << std::string(failedUrl) << " with error " << std::string(errorText)
     << " (" << errorCode << ").</h2></body></html>";
  frame->LoadURL(GetDataURI(ss.str(), "text/html"));

  if (on_load_error_fptr) {
    on_load_error_fptr(browser.get(), frame.get(), errorCode,
        errorText.ToString().c_str(), failedUrl.ToString().c_str());
  }
}

void BaseClientHandler::OnRenderProcessTerminated(
    CefRefPtr<CefBrowser> browser,
    TerminationStatus status,
    int error_code,
    const CefString& error_string) {
  CEF_REQUIRE_UI_THREAD();

  printf_log(LOG_SEVERITY_INFO,
            "OnRenderProcessTerminated: Browser %d, status=%d, error_code=%d, "
            "error_string=%s",
            browser->GetIdentifier(), static_cast<int>(status), error_code,
            error_string.ToString().c_str());

  message_router_->OnRenderProcessTerminated(browser);

  CefRefPtr<CefFrame> frame = browser->GetMainFrame();
  std::string url = frame ? frame->GetURL() : "";

  if (on_render_process_terminated_fptr) {
    const int max_size = 4 * 1024;
    char* buf = new char[max_size + 1];
    memset(buf, 0, max_size + 1);
    int reload_url_size = max_size;
    bool should_reload = on_render_process_terminated_fptr(
        browser.get(), frame.get(), startup_url_.c_str(), url.c_str(),
        static_cast<int>(status), error_code,
        error_string.ToString().c_str(), buf, reload_url_size);
    if (should_reload) {
      std::string target_url;
      if (reload_url_size > max_size) {
        // C# reported required size exceeds our buffer; fallback to startup_url_.
        printf_log(LOG_SEVERITY_ERROR,
                  "OnRenderProcessTerminated: reload_url buffer too small "
                  "(needed=%d, provided=%d), falling back to startup_url_",
                  reload_url_size, max_size);
      } else if (reload_url_size > 0) {
        buf[reload_url_size] = '\0';
        target_url = buf;
      }
      delete[] buf;
      if (target_url.empty()) {
        target_url = startup_url_;
      }
      if (frame && !target_url.empty()) {
        frame->LoadURL(target_url);
      }
      return;
    }
    delete[] buf;
  }

  // Don't reload if there's no start URL, or if the crash URL was specified.
  if (startup_url_.empty() || startup_url_ == "chrome://crash") {
    return;
  }

  // Don't reload if the termination occurred before any URL had successfully
  // loaded.
  if (url.empty()) {
    return;
  }

  // Convert URLs to lowercase for easier comparison.
  url = AsciiStrToLower(url);
  const std::string& start_url = AsciiStrToLower(startup_url_);

  // Don't reload the URL that just resulted in termination.
  if (url.find(start_url) == 0) {
    return;
  }

  frame->LoadURL(startup_url_);
}

cef_return_value_t BaseClientHandler::OnBeforeResourceLoad(
    CefRefPtr<CefBrowser> browser,
    CefRefPtr<CefFrame> frame,
    CefRefPtr<CefRequest> request,
    CefRefPtr<CefCallback> callback) {
  CEF_REQUIRE_IO_THREAD();

  if (on_before_resource_load_fptr) {
    int out_return_value = static_cast<int>(RV_CONTINUE);
    if (on_before_resource_load_fptr(browser.get(), frame.get(), request.get(),
                                     out_return_value)) {
      // DSL has no access to the native callback, so RV_CONTINUE_ASYNC has no
      // async semantic here and would hang the request (nobody holds the
      // callback to Continue/Cancel it later). Coerce anything other than
      // RV_CONTINUE to RV_CANCEL: safe-by-default, cancels instead of hangs.
      if (out_return_value != RV_CONTINUE) {
        out_return_value = RV_CANCEL;
      }
      return static_cast<cef_return_value_t>(out_return_value);
    }
  }

  return resource_manager_->OnBeforeResourceLoad(browser, frame, request,
                                                 callback);
}

CefRefPtr<CefResourceHandler> BaseClientHandler::GetResourceHandler(
    CefRefPtr<CefBrowser> browser,
    CefRefPtr<CefFrame> frame,
    CefRefPtr<CefRequest> request) {
  CEF_REQUIRE_IO_THREAD();

  // CSP bypass hook: ask C# if it wants to intercept this resource.
  // Skip CefURLRequest-initiated forwards (browser is null) to avoid
  // recursion. Also skip requests we cannot forward (hard native
  // limitations, enforced here instead of left to the DSL):
  // - Non-http(s) schemes: CefURLRequest/SimpleURLLoader cannot load them
  //   (chrome-devtools://, chrome://, data:, file:, ...). Intercepting
  //   breaks e.g. the DevTools page.
  // - Multi-element or excluded post bodies: CefURLRequest supports only a
  //   single fully-represented post-data element; forwarding multipart/file
  //   uploads would otherwise silently drop bytes.
  std::string url = request->GetURL().ToString();
  const bool canForwardScheme =
      url.compare(0, 7, "http://") == 0 || url.compare(0, 8, "https://") == 0;
  CefRefPtr<CefPostData> postData = request->GetPostData();
  const bool canForwardBody =
      !postData || (!postData->HasExcludedElements() &&
                    postData->GetElementCount() <= 1);
  if (on_get_resource_handler_filter_fptr && browser && canForwardScheme &&
      canForwardBody) {
    // Build a mutable upstream request copy: DSL may edit its headers /
    // referrer via CefRequest setters, and MyResourceHandler reuses the same
    // copy when creating the forwarded CefURLRequest (no second copy).
    // UR_FLAG_ALLOW_STORED_CREDENTIALS is required for cookies to be sent AND
    // for upstream Set-Cookie headers to be ingested into the shared cookie
    // jar (see libcef/common/request_impl.cc: without this flag,
    // credentials_mode is forced to kOmit). CefRequestImpl::Set(ResourceRequest)
    // never copies flags from the incoming ResourceRequest, so the flags read
    // here are always UR_FLAG_NONE; without this OR the forwarding path
    // silently drops all session cookies, breaking SSO redirect chains
    // (evaluation.woa.com main-doc, gemini.google.com ERR_TOO_MANY_REDIRECTS).
    // UR_FLAG_NO_RETRY_ON_5XX is required for fidelity with the native path:
    // without it libcef applies SetRetryOptions(2, RETRY_ON_5XX |
    // RETRY_ON_NETWORK_CHANGE) to the forwarded request, so an intercepted
    // POST can be silently replayed up to twice (duplicate side effects on
    // non-idempotent endpoints). Chromium's own resource loads never retry
    // 5xx, so forwarding must not either.
    // UR_FLAG_STOP_ON_REDIRECT stops the forward at the first 3xx.
    // MyResourceHandler then returns the ORIGINAL status and Location header
    // to Chromium (without GetResponseHeaders' redirectUrl, which CEF turns
    // into a synthetic 307). Chromium therefore re-issues each hop with its
    // native 301/302/303/307/308 semantics, history and OnResourceRedirect
    // notifications. Set-Cookie is ingested into the shared jar on header
    // receipt before CefURLRequest reports its expected ERR_ABORTED.
    CefRefPtr<CefRequest> upstream = CefRequest::Create();
    upstream->SetURL(request->GetURL());
    upstream->SetMethod(request->GetMethod());
    upstream->SetReferrer(request->GetReferrerURL(),
                          request->GetReferrerPolicy());
    upstream->SetFlags(request->GetFlags() | UR_FLAG_ALLOW_STORED_CREDENTIALS |
                       UR_FLAG_NO_RETRY_ON_5XX | UR_FLAG_STOP_ON_REDIRECT);
    upstream->SetFirstPartyForCookies(request->GetFirstPartyForCookies());
    CefRequest::HeaderMap headerMap;
    request->GetHeaderMap(headerMap);
    upstream->SetHeaderMap(headerMap);
    if (postData) {
      upstream->SetPostData(postData);
    }

    CefRefPtr<CefResponse> response_override = CefResponse::Create();
    bool replace_content = true;  // Default: enable body filtering.
    // In: cookie queries issued so far (DSL knows whether its cap is
    // reached). Out: n > (entry value) requests a cookie-jar snapshot for
    // this request; n <= 0 declines and resets the issued count. Preserve n
    // as the handler's cap and check it again at actual query issue: multiple
    // requests can be waiting for upstream headers concurrently.
    const int issued_at_decision = g_cookie_query_issued;
    int want_cookies = issued_at_decision;
    const bool intercept = on_get_resource_handler_filter_fptr(
        browser.get(), frame.get(), request.get(), upstream.get(),
        response_override.get(), &replace_content, want_cookies);
    if (want_cookies <= 0) {
      g_cookie_query_issued = 0;
      ++g_cookie_query_generation;
    }
    const int cookie_snapshot_limit =
        intercept && want_cookies > issued_at_decision ? want_cookies : 0;
    const int cookie_snapshot_generation = g_cookie_query_generation;
    if (intercept) {
      // Forward with the browser's request context so the upstream
      // CefURLRequest shares the browser's cookie store (SSO flows
      // converge upstream instead of looping in the browser).
      return new MyResourceHandler(upstream, response_override,
                                   browser->GetHost()->GetRequestContext(),
                                   frame, replace_content,
                                   cookie_snapshot_limit,
                                   cookie_snapshot_generation, browser);
    }
  }

  return resource_manager_->GetResourceHandler(browser, frame, request);
}

CefRefPtr<CefResponseFilter> BaseClientHandler::GetResourceResponseFilter(
    CefRefPtr<CefBrowser> browser,
    CefRefPtr<CefFrame> frame,
    CefRefPtr<CefRequest> request,
    CefRefPtr<CefResponse> response) {
  CEF_REQUIRE_IO_THREAD();

  // Inspection-mode hook: pass the actual (read-only) upstream response to C#
  // for read-only inspection. If C# returns true AND wants body filtering
  // (replace_content), register MyResponseFilter to stream the body through
  // on_response_content_filter. If C# returns true but replace_content=false,
  // skip body filter (C# was only interested in inspecting the response).
  if (on_resource_response_filter_fptr) {
    bool replace_content = true;  // Default: enable body filtering.
    if (on_resource_response_filter_fptr(browser.get(), frame.get(),
                                         request.get(), response.get(),
                                         &replace_content)) {
      if (replace_content) {
        return new MyResponseFilter();
      }
      else {
        return nullptr;
      }
    }
  }

  return test_runner::GetResourceResponseFilter(browser, frame, request,
                                                response);
}

void BaseClientHandler::OnResourceRedirect(
    CefRefPtr<CefBrowser> browser,
    CefRefPtr<CefFrame> frame,
    CefRefPtr<CefRequest> request,
    CefRefPtr<CefResponse> response,
    CefString& new_url) {
  CEF_REQUIRE_IO_THREAD();

  if (on_resource_redirect_fptr) {
    const int kMaxUrlSize = 4* 1024;
    char buf[kMaxUrlSize + 1];
    memset(buf, 0, sizeof(buf));
    int out_url_size = kMaxUrlSize;

    std::string current_url = new_url.ToString();
    if (on_resource_redirect_fptr(browser.get(), frame.get(), request.get(),
                                  response.get(), current_url.c_str(),
                                  buf, out_url_size)) {
      if (out_url_size > 0 && out_url_size <= kMaxUrlSize) {
        buf[out_url_size] = '\0';
        new_url = std::string(buf, out_url_size);
      }
    }
  }
}

int BaseClientHandler::GetBrowserCount() const {
  CEF_REQUIRE_UI_THREAD();
  return browser_count_;
}

void BaseClientHandler::SetStringResource(const std::string& page,
                                          const std::string& data) {
  if (!CefCurrentlyOn(TID_IO)) {
    CefPostTask(TID_IO, base::BindOnce(&BaseClientHandler::SetStringResource,
                                       this, page, data));
    return;
  }

  string_resource_map_[page] = data;
}

void BaseClientHandler::SetHangAction(HangAction action) {
  CEF_REQUIRE_UI_THREAD();
  hang_action_ = action;
}

BaseClientHandler::HangAction BaseClientHandler::GetHangAction() const {
  CEF_REQUIRE_UI_THREAD();
  return hang_action_;
}

bool BaseClientHandler::ShouldRequestFocus() {
  CEF_REQUIRE_UI_THREAD();

  if (initial_navigation_) {
    CefRefPtr<CefCommandLine> command_line =
        CefCommandLine::GetGlobalCommandLine();
    if (command_line->HasSwitch(switches::kNoActivate)) {
      // Don't give focus to the browser on creation.
      return false;
    }
  }

  return true;
}

}  // namespace client
