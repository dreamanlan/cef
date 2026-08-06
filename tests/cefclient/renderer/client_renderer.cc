// Copyright (c) 2012 The Chromium Embedded Framework Authors. All rights
// reserved. Use of this source code is governed by a BSD-style license that
// can be found in the LICENSE file.

#include "tests/cefclient/renderer/client_renderer.h"

#include <sstream>
#include <string>

#include "include/cef_crash_util.h"
#include "include/cef_dom.h"
#include "include/wrapper/cef_helpers.h"
#include "include/wrapper/cef_message_router.h"
#include "tests/cefclient/hostclr/HostCLR.h"
#include "tests/shared/common/client_app.h"

namespace client::renderer {

namespace {

// Must match the value in client_handler.cc.
const char kFocusedNodeChangedMessage[] = "ClientRenderer.FocusedNodeChanged";

// Shared result buffer size for sync calls from JS to C# (callMetaDSL / executeMetaDSL).
const int c_result_buffer_size = 4 * 1024 * 1024 + 1;

class JsBridgeV8Handler : public CefV8Handler {
public:
  bool Execute(const CefString& name,
                CefRefPtr<CefV8Value> object,
                const CefV8ValueList& arguments,
                CefRefPtr<CefV8Value>& retval,
                CefString& exception) override
  {
    if (name == "executeMetaDSL") {
      CefRefPtr<CefV8Context> context = CefV8Context::GetCurrentContext();
      CefRefPtr<CefBrowser> browser = context->GetBrowser();
      CefRefPtr<CefFrame> frame = context->GetFrame();

      if (on_execute_metadsl_fptr) {
        size_t size = arguments.size();
        std::vector<std::string> args_vec;
        std::vector<const char*> args_ptrs;

        for (size_t i = 0; i < size; i++) {
          if (arguments[i]->IsString()) {
            args_vec.push_back(arguments[i]->GetStringValue());
          } else {
            args_vec.push_back("");
          }
        }

        for (const auto& arg : args_vec) {
          args_ptrs.push_back(arg.c_str());
        }

        std::vector<uint8_t> result_buffer(c_result_buffer_size);
        int result_size = static_cast<int>(result_buffer.size());
        bool success = on_execute_metadsl_fptr(args_ptrs.empty() ? nullptr : args_ptrs.data(), static_cast<int>(args_vec.size()), reinterpret_cast<char*>(result_buffer.data()), result_size, browser.get(), frame.get());

        if (success && result_size > 0 && result_size < c_result_buffer_size) {
          result_buffer[result_size] = '\0';
          retval = CefV8Value::CreateString(std::string(reinterpret_cast<char*>(result_buffer.data()), result_size));
        } else {
          retval = CefV8Value::CreateString("");
          if (result_size >= c_result_buffer_size) {
            printf_log(LOG_SEVERITY_ERROR, "executeMetaDSL failed: result_size: %d, result_buffer.size(): %d", result_size, result_buffer.size());
          }
        }
        return true;
      }
    }
    else if (name == "callMetaDSL") {
      if (arguments.size() > 0 && arguments[0]->IsString()) {
        std::string func_name = arguments[0]->GetStringValue();

        CefRefPtr<CefV8Context> context = CefV8Context::GetCurrentContext();
        CefRefPtr<CefBrowser> browser = context->GetBrowser();
        CefRefPtr<CefFrame> frame = context->GetFrame();

        if (on_call_metadsl_fptr) {
          size_t size = arguments.size();
          std::vector<std::string> args_vec;
          std::vector<const char*> args_ptrs;

          for (size_t i = 1; i < size; i++) {
            if (arguments[i]->IsString()) {
              args_vec.push_back(arguments[i]->GetStringValue());
            } else {
              args_vec.push_back("");
            }
          }

          for (const auto& arg : args_vec) {
            args_ptrs.push_back(arg.c_str());
          }

          std::vector<uint8_t> result_buffer(c_result_buffer_size);
          int result_size = static_cast<int>(result_buffer.size());
          bool success = on_call_metadsl_fptr(func_name.c_str(), args_ptrs.empty() ? nullptr : args_ptrs.data(), static_cast<int>(args_vec.size()), reinterpret_cast<char*>(result_buffer.data()), result_size, browser.get(), frame.get());

          if (success && result_size > 0 && result_size < c_result_buffer_size) {
            result_buffer[result_size] = '\0';
            retval = CefV8Value::CreateString(std::string(reinterpret_cast<char*>(result_buffer.data()), result_size));
          } else {
            retval = CefV8Value::CreateString("");
            if (result_size >= c_result_buffer_size) {
              printf_log(LOG_SEVERITY_ERROR, "callMetaDSL failed: result_size: %d, result_buffer.size(): %d", result_size, result_buffer.size());
            }
          }
          return true;
        }
      }
    }

    return false;
  }

  IMPLEMENT_REFCOUNTING(JsBridgeV8Handler);
};

// CefLoadHandler implementation for renderer process
class RendererLoadHandler : public CefLoadHandler {
 public:
  void OnLoadingStateChange(CefRefPtr<CefBrowser> browser,
                            bool isLoading,
                            bool canGoBack,
                            bool canGoForward) override {
    if (on_renderer_loading_state_change_fptr) {
      CefRefPtr<CefFrame> frame = browser->GetMainFrame();
      std::string url_str;
      if (frame) {
        url_str = frame->GetURL();
      }
      on_renderer_loading_state_change_fptr(browser.get(), frame.get(), url_str.empty() ? "" : url_str.c_str(), isLoading, canGoBack, canGoForward);
    }
  }

  void OnLoadStart(CefRefPtr<CefBrowser> browser,
                   CefRefPtr<CefFrame> frame,
                   TransitionType transition_type) override {
    if (on_renderer_load_start_fptr) {
      std::string url = frame->GetURL();
      on_renderer_load_start_fptr(browser.get(), frame.get(), url.c_str(), static_cast<int>(transition_type), frame->IsMain());
    }
  }

  void OnLoadEnd(CefRefPtr<CefBrowser> browser,
                 CefRefPtr<CefFrame> frame,
                 int httpStatusCode) override {
    printf_log(LOG_SEVERITY_INFO, "RendererLoadHandler::OnLoadEnd: Browser %d frame=%s httpStatusCode=%d isMain=%d", browser->GetIdentifier(), frame->GetURL().ToString().c_str(), httpStatusCode, frame->IsMain());

    if (on_renderer_load_end_fptr) {
      const int max_size = 4 * 1024 * 1024;
      char* buf = new char[max_size + 1];
      memset(buf, 0, max_size + 1);

      std::string url = frame->GetURL();
      int code_size = max_size;
      bool use_custom_code = on_renderer_load_end_fptr(browser.get(), frame.get(), url.c_str(), httpStatusCode, frame->IsMain(), buf, code_size);

      if (use_custom_code && code_size > 0) {
        buf[code_size] = '\0';
        frame->ExecuteJavaScript(buf, frame->GetURL(), 0);
      }
      delete[] buf;
    }
  }

  void OnLoadError(CefRefPtr<CefBrowser> browser,
                   CefRefPtr<CefFrame> frame,
                   ErrorCode errorCode,
                   const CefString& errorText,
                   const CefString& failedUrl) override {
    if (on_renderer_load_error_fptr) {
      on_renderer_load_error_fptr(browser.get(), frame.get(), errorCode, errorText.ToString().c_str(), failedUrl.ToString().c_str());
    }
  }

 private:
  IMPLEMENT_REFCOUNTING(RendererLoadHandler);
};

class ClientRenderDelegate : public ClientAppRenderer::Delegate {
 public:
  ClientRenderDelegate() : renderer_load_handler_(new RendererLoadHandler()) {}

  void OnBeforeCommandLineProcessing(
      CefRefPtr<ClientAppRenderer> app,
      CefRefPtr<CefCommandLine> command_line) override {
    if (on_before_command_line_processing_fptr) {
      int process_type = static_cast<int>(ClientApp::GetProcessType(command_line));
      on_before_command_line_processing_fptr(process_type, command_line.get());
    }
  }

  CefRefPtr<CefLoadHandler> GetLoadHandler(
      CefRefPtr<ClientAppRenderer> app) override {
    return renderer_load_handler_;
  }

  ClientRenderDelegate(const ClientRenderDelegate&) = delete;
  ClientRenderDelegate& operator=(const ClientRenderDelegate&) = delete;

  void OnWebKitInitialized(CefRefPtr<ClientAppRenderer> app) override {
    if (CefCrashReportingEnabled()) {
      // Set some crash keys for testing purposes. Keys must be defined in the
      // "crash_reporter.cfg" file. See cef_crash_util.h for details.
      CefSetCrashKeyValue("testkey_small1", "value1_small_renderer");
      CefSetCrashKeyValue("testkey_small2", "value2_small_renderer");
      CefSetCrashKeyValue("testkey_medium1", "value1_medium_renderer");
      CefSetCrashKeyValue("testkey_medium2", "value2_medium_renderer");
      CefSetCrashKeyValue("testkey_large1", "value1_large_renderer");
      CefSetCrashKeyValue("testkey_large2", "value2_large_renderer");
    }

    // Create the renderer-side router for query handling.
    CefMessageRouterConfig config;
    message_router_ = CefMessageRouterRendererSide::Create(config);
  }

  void OnContextCreated(CefRefPtr<ClientAppRenderer> app,
                        CefRefPtr<CefBrowser> browser,
                        CefRefPtr<CefFrame> frame,
                        CefRefPtr<CefV8Context> context) override {
    message_router_->OnContextCreated(browser, frame, context);

    CefRefPtr<CefV8Value> global = context->GetGlobal();
    CefRefPtr<JsBridgeV8Handler> handler = new JsBridgeV8Handler();

    CefRefPtr<CefV8Value> execFunc = CefV8Value::CreateFunction("executeMetaDSL", handler);
    global->SetValue("executeMetaDSL", execFunc, V8_PROPERTY_ATTRIBUTE_NONE);

    CefRefPtr<CefV8Value> callFunc = CefV8Value::CreateFunction("callMetaDSL", handler);
    global->SetValue("callMetaDSL", callFunc, V8_PROPERTY_ATTRIBUTE_NONE);

    // Register the browser in the renderer-process ref map BEFORE firing any
    // C# callback, and only on the main frame (main frame lifetime == browser
    // lifetime). Sub-frame contexts are tracked implicitly via the main
    // frame's registration.
    if (frame->IsMain()) {
      renderer_ref_add(browser, frame);
    }

    if (on_renderer_init_fptr) {
      std::string url = frame->GetURL();
      on_renderer_init_fptr(browser.get(), frame.get(), url.c_str());
    }

    // Start heartbeat timer for renderer process (process_type=1), only once
    if (!heartbeat_started_) {
      heartbeat_started_ = true;
      StartHeartbeat(1);
    }
  }

  void OnContextReleased(CefRefPtr<ClientAppRenderer> app,
                         CefRefPtr<CefBrowser> browser,
                         CefRefPtr<CefFrame> frame,
                         CefRefPtr<CefV8Context> context) override {
    // Fire C# callback FIRST while the browser is still marked valid.
    if (on_renderer_finalize_fptr) {
      on_renderer_finalize_fptr(browser.get(), frame.get());
    }

    // Unregister the browser AFTER all C# callbacks have fired. Only on the
    // main frame so sub-frame teardown does not prematurely invalidate the
    // browser pointer.
    if (frame->IsMain()) {
      renderer_ref_remove(browser);
    }

    message_router_->OnContextReleased(browser, frame, context);
  }

  void OnFocusedNodeChanged(CefRefPtr<ClientAppRenderer> app,
                            CefRefPtr<CefBrowser> browser,
                            CefRefPtr<CefFrame> frame,
                            CefRefPtr<CefDOMNode> node) override {
    bool is_editable = (node.get() && node->IsEditable());
    if (is_editable != last_node_is_editable_) {
      // Notify the browser of the change in focused element type.
      last_node_is_editable_ = is_editable;
      CefRefPtr<CefProcessMessage> message =
          CefProcessMessage::Create(kFocusedNodeChangedMessage);
      message->GetArgumentList()->SetBool(0, is_editable);
      frame->SendProcessMessage(PID_BROWSER, message);
    }
  }

  bool OnProcessMessageReceived(CefRefPtr<ClientAppRenderer> app,
                                CefRefPtr<CefBrowser> browser,
                                CefRefPtr<CefFrame> frame,
                                CefProcessId source_process,
                                CefRefPtr<CefProcessMessage> message) override {
    if(message_router_->OnProcessMessageReceived(browser, frame, source_process, message)){
      return true;
    }


    if (on_receive_cef_message_fptr) {
      std::string message_name = message->GetName();
      size_t size = message->GetArgumentList()->GetSize();

      std::vector<std::string> args_vec;
      std::vector<const char*> args_ptrs;

      for (size_t i = 0; i < size; i++) {
        args_vec.push_back(message->GetArgumentList()->GetString(i).ToString());
      }

      for (const auto& arg : args_vec) {
        args_ptrs.push_back(arg.c_str());
      }

      on_receive_cef_message_fptr(message_name.c_str(), args_ptrs.empty() ? nullptr : args_ptrs.data(), static_cast<int>(args_vec.size()), browser.get(), frame.get(), static_cast<int>(source_process));
      return true;
    }
    return false;
  }

 private:
  bool last_node_is_editable_ = false;
  bool heartbeat_started_ = false;

  // Renderer load handler for load callbacks
  CefRefPtr<RendererLoadHandler> renderer_load_handler_;

  // Handles the renderer side of query routing.
  CefRefPtr<CefMessageRouterRendererSide> message_router_;
  IMPLEMENT_REFCOUNTING(ClientRenderDelegate);
};

}  // namespace

void CreateDelegates(ClientAppRenderer::DelegateSet& delegates) {
  delegates.insert(new ClientRenderDelegate);
}

}  // namespace client::renderer
