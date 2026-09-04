// Copyright (c) 2024 The Chromium Embedded Framework Authors. All rights
// reserved. Use of this source code is governed by a BSD-style license that
// can be found in the LICENSE file.

#include "tests/cefclient/browser/base_client_handler.h"

#include <sstream>

#include "include/cef_command_line.h"
#include "include/cef_parser.h"
#include "tests/cefclient/browser/main_context.h"
#include "tests/cefclient/browser/my_response_filter.h"
#include "tests/cefclient/browser/root_window_manager.h"
#include "tests/cefclient/hostclr/HostCLR.h"
#include "tests/cefclient/hostclr/native_callbacks.h"
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
    CefRefPtr<CefListValue> arg_list = message->GetArgumentList();
    std::vector<uint8_t> blob;
    if (arg_list && arg_list->GetSize() > 0 &&
        arg_list->GetType(0) == VTYPE_BINARY) {
      CefRefPtr<CefBinaryValue> bin = arg_list->GetBinary(0);
      if (bin && bin->GetSize() > 0) {
        blob.resize(bin->GetSize());
        bin->GetData(blob.data(), bin->GetSize(), 0);
      }
    }
    on_receive_cef_message_fptr(message_name.c_str(),
        blob.empty() ? nullptr : blob.data(),
        static_cast<int>(blob.size()), browser.get(), frame.get(),
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

  // Cancel any async callback this browser still owns (JS dialogs, deferred
  // resource loads). Pending operations must never outlive their browser.
  CancelBrowserCallbacks(browser->GetIdentifier());

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
  if (!on_load_end_fptr && !inject_all_frame_ && !frame->IsMain()) {
    // No custom handler and sub-frame injection is disabled; nothing to do.
    return;
  }

  {
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

    // The default inject.js path is gated by inject_all_frame_/main frame;
    // the custom callback above decides on its own for every frame.
    if (!use_custom_code && (inject_all_frame_ || frame->IsMain())) {
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
    // Park the callback before calling managed code so RV_CONTINUE_ASYNC can be
    // honored: the handle is passed in as an input argument and managed code
    // resumes the request later through complete_native_callback. CefCallback
    // must be resumed on the IO thread, hence TID_IO.
    const int browser_id = browser ? browser->GetIdentifier() : 0;
    const int64_t handle = RegisterNativeCallback(
        browser_id, TID_IO,
        [callback](bool ok, const std::string& /*data*/, int /*code*/) {
          if (!callback) {
            return;
          }
          if (ok) {
            callback->Continue();
          } else {
            callback->Cancel();
          }
        },
        kResourceLoadTimeoutMs);

    if (on_before_resource_load_fptr(browser.get(), frame.get(), request.get(),
                                     handle, out_return_value)) {
      if (out_return_value == RV_CONTINUE_ASYNC) {
        // Managed code owns the request now and must complete |handle|,
        // otherwise the request stays pending until the browser closes.
        return RV_CONTINUE_ASYNC;
      }
      DiscardNativeCallback(handle);
      // Any other unexpected value is coerced to RV_CANCEL: safe-by-default,
      // cancels instead of hanging.
      if (out_return_value != RV_CONTINUE) {
        out_return_value = RV_CANCEL;
      }
      return static_cast<cef_return_value_t>(out_return_value);
    }
    DiscardNativeCallback(handle);
  }

  return resource_manager_->OnBeforeResourceLoad(browser, frame, request,
                                                 callback);
}

CefRefPtr<CefResourceHandler> BaseClientHandler::GetResourceHandler(
    CefRefPtr<CefBrowser> browser,
    CefRefPtr<CefFrame> frame,
    CefRefPtr<CefRequest> request) {
  CEF_REQUIRE_IO_THREAD();

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
    const int kMaxUrlSize = 4 * 1024;
    char buf[kMaxUrlSize + 1];
    memset(buf, 0, sizeof(buf));
    int out_url_size = kMaxUrlSize;

    std::string current_url = new_url.ToString();
    if (on_resource_redirect_fptr(browser.get(), frame.get(), request.get(),
                                  response.get(), current_url.c_str(), buf,
                                  out_url_size)) {
      if (out_url_size > 0 && out_url_size <= kMaxUrlSize) {
        buf[out_url_size] = '\0';
        new_url = std::string(buf, out_url_size);
      }
    }
  }
}

void BaseClientHandler::OnBeforeResourceResponse(
    CefRefPtr<CefBrowser> browser,
    CefRefPtr<CefFrame> frame,
    CefRefPtr<CefRequest> request,
    CefRefPtr<CefResponse> response) {
  CEF_REQUIRE_IO_THREAD();

  if (on_before_resource_response_fptr) {
    on_before_resource_response_fptr(browser.get(), frame.get(), request.get(),
                                     response.get());
  }
}

void BaseClientHandler::OnResourceLoadComplete(
    CefRefPtr<CefBrowser> browser,
    CefRefPtr<CefFrame> frame,
    CefRefPtr<CefRequest> request,
    CefRefPtr<CefResponse> response,
    URLRequestStatus status,
    int64_t received_content_length) {
  CEF_REQUIRE_IO_THREAD();

  if (on_resource_load_complete_fptr) {
    on_resource_load_complete_fptr(
        browser.get(), frame.get(), request.get(), response.get(),
        static_cast<int>(status), received_content_length);
  }
}

void BaseClientHandler::OnProtocolExecution(CefRefPtr<CefBrowser> browser,
                                            CefRefPtr<CefFrame> frame,
                                            CefRefPtr<CefRequest> request,
                                            bool& allow_os_execution) {
  CEF_REQUIRE_IO_THREAD();

  if (on_protocol_execution_fptr) {
    bool dsl_allow_os_execution = allow_os_execution;
    if (on_protocol_execution_fptr(browser.get(), frame.get(), request.get(),
                                   &dsl_allow_os_execution)) {
      allow_os_execution = dsl_allow_os_execution;
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

// static
bool BaseClientHandler::MaybeHandlePermissionPromptViaDSL(
    CefRefPtr<CefBrowser> browser,
    uint64_t prompt_id,
    const CefString& requesting_origin,
    uint32_t requested_permissions,
    CefRefPtr<CefPermissionPromptCallback> callback) {
  CEF_REQUIRE_UI_THREAD();

  // No DSL wired -> fall through to CEF default handling
  // (chrome-style: native bubble; alloy-style: IGNORE, Promise never resolves).
  if (!on_show_permission_prompt_fptr) {
    return false;
  }

  std::string originStr = requesting_origin.ToString();
  int action = 0;
  // CEF does not provide a frame here; pass browser->GetMainFrame() so C#
  // can set NativeApi context with the same (browser, frame) convention.
  CefRefPtr<CefFrame> main_frame = browser ? browser->GetMainFrame() : nullptr;
  const bool handled = on_show_permission_prompt_fptr(
      browser.get(), main_frame.get(), prompt_id, originStr.c_str(),
      requested_permissions, action);
  if (!handled) {
    return false;
  }

  switch (action) {
    case 1:
      printf_log(LOG_SEVERITY_INFO,
                 "OnShowPermissionPrompt: DSL accept prompt_id=%llu origin=%s "
                 "perms=0x%x",
                 static_cast<unsigned long long>(prompt_id),
                 originStr.c_str(), requested_permissions);
      callback->Continue(CEF_PERMISSION_RESULT_ACCEPT);
      return true;
    case 2:
      printf_log(LOG_SEVERITY_INFO,
                 "OnShowPermissionPrompt: DSL deny prompt_id=%llu origin=%s "
                 "perms=0x%x",
                 static_cast<unsigned long long>(prompt_id),
                 originStr.c_str(), requested_permissions);
      callback->Continue(CEF_PERMISSION_RESULT_DENY);
      return true;
    case 0:
    default:
      // DSL declined to decide -> fall through to CEF default handling.
      return false;
  }
}

}  // namespace client
