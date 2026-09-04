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
#include "tests/cefclient/hostclr/js_arg_v8.h"
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
        std::vector<JsArgNode> arg_nodes;

        for (size_t i = 0; i < size; i++) {
          // Preserve full JS typing (int64/bigint/binary/nested) via JsArg.
          V8ToNodes(context, arguments[i], arg_nodes);
        }
        std::vector<uint8_t> args_blob = SerializeNodes(arg_nodes);

        std::vector<uint8_t> result_buffer(c_result_buffer_size);
        int result_size = static_cast<int>(result_buffer.size());
        bool success = on_execute_metadsl_fptr(args_blob.data(), static_cast<int>(args_blob.size()), reinterpret_cast<char*>(result_buffer.data()), result_size, browser.get(), frame.get());

        if (success && result_size > 0 && result_size <= c_result_buffer_size) {
          std::vector<JsArgNode> result_nodes = DeserializeNodes(result_buffer.data(), static_cast<size_t>(result_size));
          CefV8ValueList result_vals;
          NodesToV8List(context, result_nodes, result_vals);
          retval = result_vals.empty() ? CefV8Value::CreateUndefined() : result_vals.front();
        } else {
          retval = CefV8Value::CreateUndefined();
          if (result_size > c_result_buffer_size) {
            printf_log(LOG_SEVERITY_ERROR, "executeMetaDSL failed: result_size: %d, buffer: %d", result_size, static_cast<int>(result_buffer.size()));
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
          std::vector<JsArgNode> arg_nodes;

          for (size_t i = 1; i < size; i++) {
            // Preserve full JS typing (int64/bigint/binary/nested) via JsArg.
            V8ToNodes(context, arguments[i], arg_nodes);
          }
          std::vector<uint8_t> args_blob = SerializeNodes(arg_nodes);

          std::vector<uint8_t> result_buffer(c_result_buffer_size);
          int result_size = static_cast<int>(result_buffer.size());
          bool success = on_call_metadsl_fptr(func_name.c_str(), args_blob.data(), static_cast<int>(args_blob.size()), reinterpret_cast<char*>(result_buffer.data()), result_size, browser.get(), frame.get());

          if (success && result_size > 0 && result_size <= c_result_buffer_size) {
            std::vector<JsArgNode> result_nodes = DeserializeNodes(result_buffer.data(), static_cast<size_t>(result_size));
            CefV8ValueList result_vals;
            NodesToV8List(context, result_nodes, result_vals);
            retval = result_vals.empty() ? CefV8Value::CreateUndefined() : result_vals.front();
          } else {
            retval = CefV8Value::CreateUndefined();
            if (result_size > c_result_buffer_size) {
              printf_log(LOG_SEVERITY_ERROR, "callMetaDSL failed: result_size: %d, buffer: %d", result_size, static_cast<int>(result_buffer.size()));
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

    // Inject the JsArg marshalling helpers used by the native V8<->JsArg
    // conversion (see js_arg_v8.h). Defined here so they are available before
    // any page script runs. __cefMarshal normalizes any JS value into a
    // [tagInt, payload] tree; __cefBuild reconstructs BigInt/Date/ArrayBuffer.
    static const char* kJsArgHelpers = R"JS(
(function(){
  if (window.__cefMarshal && window.__cefBuild) { return; }
  var T = {Null:0,Undefined:1,Bool:2,Int32:3,UInt32:4,Double:5,BigInt:6,String:7,Binary:8,Array:9,Object:10,DateTime:11};
  function b64(v){
    var bytes = (v instanceof ArrayBuffer) ? new Uint8Array(v) : new Uint8Array(v.buffer, v.byteOffset, v.byteLength);
    var s = '';
    for (var i = 0; i < bytes.length; i++) { s += String.fromCharCode(bytes[i]); }
    return btoa(s);
  }
  function m(v){
    if (v === null) { return [T.Null, null]; }
    var t = typeof v;
    if (t === 'undefined') { return [T.Undefined, null]; }
    if (t === 'boolean') { return [T.Bool, v]; }
    if (t === 'bigint') { return [T.BigInt, v.toString()]; }
    if (t === 'string') { return [T.String, v]; }
    if (t === 'number') {
      if (Number.isInteger(v)) {
        if (v >= -2147483648 && v <= 2147483647) { return [T.Int32, v]; }
        if (v >= 0 && v <= 4294967295) { return [T.UInt32, v]; }
      }
      return [T.Double, v];
    }
    if (t === 'object') {
      if (v instanceof Date) { return [T.DateTime, v.toISOString()]; }
      if (v instanceof ArrayBuffer || ArrayBuffer.isView(v)) { return [T.Binary, b64(v)]; }
      if (Array.isArray(v)) {
        var arr = [];
        for (var i = 0; i < v.length; i++) { arr.push(m(v[i])); }
        return [T.Array, arr];
      }
      var flat = [];
      for (var k in v) {
        if (Object.prototype.hasOwnProperty.call(v, k)) {
          flat.push(String(k));
          flat.push(m(v[k]));
        }
      }
      return [T.Object, flat];
    }
    return [T.String, String(v)];
  }
  window.__cefMarshal = function(x){ return m(x); };
  window.__cefBuild = function(tag, payload){
    switch (tag) {
      case 6: return BigInt(payload);
      case 11: return new Date(payload);
      case 8: {
        var bin = atob(payload);
        var bytes = new Uint8Array(bin.length);
        for (var i = 0; i < bin.length; i++) { bytes[i] = bin.charCodeAt(i); }
        return bytes.buffer;
      }
      default: return payload;
    }
  };
})();
)JS";
    frame->ExecuteJavaScript(kJsArgHelpers, frame->GetURL(), 0);

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

      on_receive_cef_message_fptr(message_name.c_str(), blob.empty() ? nullptr : blob.data(), static_cast<int>(blob.size()), browser.get(), frame.get(), static_cast<int>(source_process));
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
