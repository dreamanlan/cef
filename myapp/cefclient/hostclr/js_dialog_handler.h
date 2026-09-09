#pragma once
#include "include/cef_jsdialog_handler.h"

namespace client {

// JS dialog types passed to managed code. Values mirror
// cef_jsdialog_type_t for 0..2; beforeunload is appended because CEF reports it
// through a separate handler method.
enum JsDialogTypeForManaged {
    JS_DIALOG_ALERT = 0,
    JS_DIALOG_CONFIRM = 1,
    JS_DIALOG_PROMPT = 2,
    JS_DIALOG_BEFORE_UNLOAD = 3
};

// Decision returned by managed code (C# -> DSL).
enum JsDialogDecision {
    // Not taken over: fall through to the CEF default dialog. Identical to not
    // registering a handler at all.
    JS_DIALOG_DECISION_DEFAULT = 0,
    // Taken over: managed code shows its own dialog (typically by executing
    // window.AgentDialog.show in the page) and completes the handle later.
    JS_DIALOG_DECISION_TAKEOVER = 1,
    // Suppress the message without showing anything. confirm() returns false.
    // Ignored for beforeunload (that CEF method has no suppress flag).
    JS_DIALOG_DECISION_SUPPRESS = 2,
    // Taken over by the script itself; behaves like TAKEOVER for CEF. Kept
    // separate so the script can tell the two flows apart.
    JS_DIALOG_DECISION_SCRIPT_OWNED = 3
};

// Bridges CEF JS dialogs (alert / confirm / prompt / beforeunload) to managed
// code. The pending CefJSDialogCallback is parked in the generic native
// callback registry (see hostclr/native_callbacks.h) and resumed later through
// complete_native_callback.
//
// All methods run on the browser process UI thread.
class ClientJSDialogHandler : public CefJSDialogHandler {
 public:
  ClientJSDialogHandler() = default;

  ClientJSDialogHandler(const ClientJSDialogHandler&) = delete;
  ClientJSDialogHandler& operator=(const ClientJSDialogHandler&) = delete;

  // CefJSDialogHandler methods.
  bool OnJSDialog(CefRefPtr<CefBrowser> browser,
                  const CefString& origin_url,
                  JSDialogType dialog_type,
                  const CefString& message_text,
                  const CefString& default_prompt_text,
                  CefRefPtr<CefJSDialogCallback> callback,
                  bool& suppress_message) override;
  bool OnBeforeUnloadDialog(CefRefPtr<CefBrowser> browser,
                            const CefString& message_text,
                            bool is_reload,
                            CefRefPtr<CefJSDialogCallback> callback) override;
  void OnResetDialogState(CefRefPtr<CefBrowser> browser) override;
  void OnDialogClosed(CefRefPtr<CefBrowser> browser) override;

 private:
  // Shared implementation for both dialog entry points. |suppress_message| is
  // null for beforeunload, where CEF provides no suppress flag.
  bool HandleDialog(CefRefPtr<CefBrowser> browser,
                    int dialog_type,
                    const CefString& origin_url,
                    const CefString& message_text,
                    const CefString& default_prompt_text,
                    CefRefPtr<CefJSDialogCallback> callback,
                    bool* suppress_message);

  IMPLEMENT_REFCOUNTING(ClientJSDialogHandler);
};

}  // namespace client
