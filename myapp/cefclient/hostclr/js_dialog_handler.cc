#include "myapp/cefclient/hostclr/js_dialog_handler.h"

#include "myapp/cefclient/hostclr/HostCLR.h"
#include "myapp/cefclient/hostclr/native_callbacks.h"

#include "include/wrapper/cef_helpers.h"

namespace client {

namespace {

// Resumes a pending JS dialog. Registered with TID_UI affinity because
// CefJSDialogCallback must be executed on the UI thread.
void ContinueJsDialog(CefRefPtr<CefJSDialogCallback> callback,
                      bool ok,
                      const std::string& user_input) {
    CEF_REQUIRE_UI_THREAD();
    if (callback) {
        callback->Continue(ok, CefString(user_input));
    }
}

}  // namespace

bool ClientJSDialogHandler::HandleDialog(
    CefRefPtr<CefBrowser> browser,
    int dialog_type,
    const CefString& origin_url,
    const CefString& message_text,
    const CefString& default_prompt_text,
    CefRefPtr<CefJSDialogCallback> callback,
    bool* suppress_message) {
  CEF_REQUIRE_UI_THREAD();

  // No managed hook: behave exactly as if no handler were registered.
  if (!on_js_dialog_fptr || !browser) {
    return false;
  }

  const int browser_id = browser->GetIdentifier();

  // Park the callback before asking managed code, so the handle can be passed
  // in as a plain input argument. The closure keeps a CefRefPtr alive until it
  // is completed, discarded or cancelled.
  const int64_t handle = RegisterNativeCallback(
      browser_id, TID_UI,
      [callback](bool ok, const std::string& user_input, int /*code*/) {
        ContinueJsDialog(callback, ok, user_input);
      },
      kJsDialogTimeoutMs);

  const std::string origin = origin_url.ToString();
  const std::string message = message_text.ToString();
  const std::string default_text = default_prompt_text.ToString();

  // Exceptions are disabled in this build, so the managed side is responsible
  // for catching its own failures (OnJsDialog wraps everything in try/catch and
  // returns 0 = not taken over on error).
  // CEF does not provide a frame on JS dialogs; pass browser->GetMainFrame()
  // so C# can set NativeApi context with the same (browser, frame) convention.
  CefRefPtr<CefFrame> main_frame = browser->GetMainFrame();
  const int decision =
      on_js_dialog_fptr(browser.get(), main_frame.get(), dialog_type,
                        origin.c_str(), message.c_str(), default_text.c_str(),
                        handle);

  if (decision == JS_DIALOG_DECISION_TAKEOVER ||
      decision == JS_DIALOG_DECISION_SCRIPT_OWNED) {
    // Managed code owns the dialog and will complete |handle| later.
    return true;
  }

  // Not taken over: drop the parked callback and let CEF handle the dialog.
  DiscardNativeCallback(handle);

  if (decision == JS_DIALOG_DECISION_SUPPRESS) {
    if (suppress_message) {
      *suppress_message = true;
    } else {
      // beforeunload has no suppress flag; degrade to the default dialog.
      printf_log(LOG_SEVERITY_WARNING,
                 "[native] suppress is not supported for beforeunload dialogs");
    }
  }
  return false;
}

bool ClientJSDialogHandler::OnJSDialog(CefRefPtr<CefBrowser> browser,
                                       const CefString& origin_url,
                                       JSDialogType dialog_type,
                                       const CefString& message_text,
                                       const CefString& default_prompt_text,
                                       CefRefPtr<CefJSDialogCallback> callback,
                                       bool& suppress_message) {
  return HandleDialog(browser, static_cast<int>(dialog_type), origin_url,
                      message_text, default_prompt_text, callback,
                      &suppress_message);
}

bool ClientJSDialogHandler::OnBeforeUnloadDialog(
    CefRefPtr<CefBrowser> browser,
    const CefString& message_text,
    bool is_reload,
    CefRefPtr<CefJSDialogCallback> callback) {
  // Note: callback semantics are inverted compared to a plain confirm:
  // Continue(true) leaves/reloads the page, Continue(false) stays.
  // CEF provides no suppress flag here, hence the null argument.
  return HandleDialog(browser, JS_DIALOG_BEFORE_UNLOAD, CefString(),
                      message_text, CefString(), callback, nullptr);
}

void ClientJSDialogHandler::OnResetDialogState(CefRefPtr<CefBrowser> browser) {
  CEF_REQUIRE_UI_THREAD();
  // Page navigation (or any other CEF-side reset) invalidates pending dialogs:
  // cancel everything this browser still owns so confirm() cannot hang.
  if (browser) {
    CancelBrowserCallbacks(browser->GetIdentifier());
  }
}

void ClientJSDialogHandler::OnDialogClosed(CefRefPtr<CefBrowser> browser) {
  CEF_REQUIRE_UI_THREAD();
}

}  // namespace client
