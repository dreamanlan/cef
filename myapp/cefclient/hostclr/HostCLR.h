#pragma once
#include "coreclr/coreclr_delegates.h"
#include "path_utils.h"
#include "include/cef_browser.h"
#include "include/cef_frame.h"
#include <cstdint>
#include <string>

// Cross-platform character set conversion functions
extern std::string WideStringToUtf8(const wchar_t* wstr);
extern std::wstring Utf8ToWstring(const char* str);
extern bool WideToUtf8ToBuffer(const wchar_t* wstr, char* buf, int bufSize);
extern bool MultiByteToWideBuffer(const char* src, wchar_t* buf, int bufSize, unsigned int codePage = 65001);  // CP_UTF8 = 65001

enum LogSeverity {
    LOG_SEVERITY_ERROR,
    LOG_SEVERITY_WARNING,
    LOG_SEVERITY_INFO
};

extern void printf_log(LogSeverity severity, const char* fmt, ...);
extern int load_hostfxr(bool is_debug, int& out_rc);
extern int load_dotnet_method(bool is_debug, int& out_rc);

typedef bool (CORECLR_DELEGATE_CALLTYPE* on_init_fn)(const char* cmd_line, const char* base_path, int process_type, const char* app_dir, bool is_mac);
typedef void (CORECLR_DELEGATE_CALLTYPE* on_finalize_fn)();
typedef void (CORECLR_DELEGATE_CALLTYPE* on_browser_init_fn)(void* browser);
typedef void (CORECLR_DELEGATE_CALLTYPE* on_browser_finalize_fn)(void* browser);
typedef bool (CORECLR_DELEGATE_CALLTYPE* on_browser_hot_reload_copyfiles_fn)(const char* url);
typedef void (CORECLR_DELEGATE_CALLTYPE* on_browser_hot_reload_completed_fn)(void* browser, void* frame, const char* url);
// on_browser_cef_query: called when the page calls window.cefQuery
// (browser process, UI thread). |handle| identifies the parked
// CefMessageRouterBrowserSide::Callback in the generic native callback registry.
// Returns false (the default/failure value, so a managed error degrades safely)
// when the query was handled synchronously: |out_result| then holds the result
// code (0 = Success, non-zero = Failure with that code) and the handle is
// discarded by the caller.
// Returns true to take the query over asynchronously: the managed side must
// later call complete_native_callback(handle, ok, response, error_code), where
// ok=true sends Success(response) and ok=false sends Failure(error_code, response).
typedef bool (CORECLR_DELEGATE_CALLTYPE* on_browser_cef_query_fn)(void* browser, void* frame, int64_t query_id, const char* request, bool persistent, int64_t handle, int& out_result);
typedef void (CORECLR_DELEGATE_CALLTYPE* on_renderer_init_fn)(void* browser, void* frame, const char* url);
typedef void (CORECLR_DELEGATE_CALLTYPE* on_renderer_finalize_fn)(void* browser, void* frame);
typedef void (CORECLR_DELEGATE_CALLTYPE* on_loading_state_change_fn)(void* browser, void* frame, const char* url, bool is_loading, bool can_go_back, bool can_go_forward);
typedef void (CORECLR_DELEGATE_CALLTYPE* on_load_error_fn)(void* browser, void* frame, int error_code, const char* error_text, const char* failed_url);
typedef bool (CORECLR_DELEGATE_CALLTYPE* on_render_process_terminated_fn)(void* browser, void* frame, const char* startup_url, const char* url, int status, int error_code, const char* error_string, char* reload_url, int& reload_url_size);
typedef void (CORECLR_DELEGATE_CALLTYPE* on_load_start_fn)(void* browser, void* frame, const char* url, int transition_type, bool is_main);
typedef bool (CORECLR_DELEGATE_CALLTYPE* on_load_end_fn)(void* browser, void* frame, const char* url, int http_status_code, bool inject_all_frame, bool is_main, char* js_code, int& code_size);
typedef void (CORECLR_DELEGATE_CALLTYPE* on_renderer_load_start_fn)(void* browser, void* frame, const char* url, int transition_type, bool is_main);
typedef bool (CORECLR_DELEGATE_CALLTYPE* on_renderer_load_end_fn)(void* browser, void* frame, const char* url, int http_status_code, bool is_main, char* js_code, int& code_size);
typedef void (CORECLR_DELEGATE_CALLTYPE* on_renderer_loading_state_change_fn)(void* browser, void* frame, const char* url, bool is_loading, bool can_go_back, bool can_go_forward);
typedef void (CORECLR_DELEGATE_CALLTYPE* on_renderer_load_error_fn)(void* browser, void* frame, int error_code, const char* error_text, const char* failed_url);
typedef void (CORECLR_DELEGATE_CALLTYPE* on_receive_cef_message_fn)(const char* message, const uint8_t* args_blob, int args_len, void* browser, void* frame, int source_process_id);
typedef bool (CORECLR_DELEGATE_CALLTYPE* on_execute_metadsl_fn)(const uint8_t* args_blob, int args_len, char* result_str, int& result_size, void* browser, void* frame);
typedef void (CORECLR_DELEGATE_CALLTYPE* on_before_command_line_processing_fn)(int process_type, void* command_line);
// Called on the CEF IO thread from GetAuthCredentials.
// |browser| is a raw CefBrowser* from the handler argument (valid for the
// synchronous duration of this call; do not retain past return). |frame| is
// ALWAYS nullptr here: CEF does not provide a frame on GetAuthCredentials,
// and the call runs on the IO thread where there is no meaningful page-frame
// context to fabricate. Managed code should tolerate frame==IntPtr.Zero and
// still set NativeApi context using |browser| alone.
// username_size/password_size carry buffer capacity in and byte length out.
// |handle| identifies the parked CefAuthCallback in the generic native
// callback registry (see native_callbacks.h). |attempt| is 0 for the first
// call on a given target within the current process, 1 for a retry after a
// previously supplied credential failed (used to skip stale saved
// credentials).
// Return value semantics (three-state):
//   return false                        -> DSL declined; C++ runs the native
//                                          credui fallback (Credential Manager
//                                          + Windows CredUI prompt).
//   return true,  username_size > 0     -> DSL supplied credentials
//                                          synchronously; C++ discards |handle|
//                                          and calls CefAuthCallback::Continue
//                                          with the buffers.
//   return true,  username_size == 0    -> DSL took ownership; C++ leaves
//                                          |handle| in the registry and the
//                                          managed side must eventually call
//                                          native_callback_complete(handle,
//                                          ok, "user\npass", 0). ok=false
//                                          triggers CefAuthCallback::Cancel.
typedef bool (CORECLR_DELEGATE_CALLTYPE* on_get_auth_credentials_fn)(void* browser, void* frame, bool is_proxy, const char* host, int port, const char* realm, const char* scheme, const char* origin_url, char* username, int& username_size, char* password, int& password_size, int64_t handle, int attempt);
// Synchronous: called on the CEF UI thread from OnRequestMediaAccessPermission.
// |browser| is a raw CefBrowser* and |frame| is the requesting CefFrame* as
// provided by CEF (both valid for the synchronous duration of this call on
// the CEF UI thread; do not retain past return).
// requested_permissions is a bitmask of CEF_MEDIA_PERMISSION_* values.
// menu_disabled reflects the current "media handling disabled" menu switch.
// On return, *allowed_permissions is a subset of requested_permissions to grant.
// Return: true = DSL handled (use *allowed_permissions); false = C++ falls back
// to the default logic (menu kill-switch, then native permission prompt).
typedef bool (CORECLR_DELEGATE_CALLTYPE* on_request_media_access_permission_fn)(void* browser, void* frame, const char* requesting_origin, uint32_t requested_permissions, bool menu_disabled, uint32_t* allowed_permissions);
// Synchronous: called on the CEF UI thread from OnShowPermissionPrompt for
// permission requests that surface as a Chromium permission bubble
// (notifications, geolocation, clipboard, storage-access, ...). See
// cef_types.h CEF_PERMISSION_TYPE_* for the bitmask meaning.
// |browser| is a raw CefBrowser* (valid for the synchronous duration of this
// call on the CEF UI thread; do not retain past return). CEF does not provide
// a frame here; C++ passes browser->GetMainFrame().get() as |frame| so the
// managed side can set NativeApi context uniformly.
// |prompt_id| is the unique id assigned by CEF for this prompt (matches the
// value passed to OnDismissPermissionPrompt when the prompt is dismissed).
// On return, |action| selects the behavior when the return value is true:
//   0 = default (fall through to CEF default handling; equivalent to false)
//   1 = accept  (CefPermissionPromptCallback::Continue(ACCEPT))
//   2 = deny    (CefPermissionPromptCallback::Continue(DENY))
// Return: true = DSL decided (use |action|); false = C++ default fallback
// (chrome-style shows the native bubble; alloy-style IGNOREs -- the JS
// Promise from Notification.requestPermission()/etc. will not resolve).
typedef bool (CORECLR_DELEGATE_CALLTYPE* on_show_permission_prompt_fn)(void* browser, void* frame, uint64_t prompt_id, const char* requesting_origin, uint32_t requested_permissions, int& action);
// Synchronous: called on the CEF UI thread from OnCertificateError.
// |browser| is a raw CefBrowser* (valid for the synchronous duration of this
// call on the CEF UI thread; do not retain past return). CEF does not provide
// a frame here; C++ passes browser->GetMainFrame().get() as |frame| so the
// managed side can set NativeApi context uniformly.
// cert_error is a Chromium net error code (e.g. -200 = ERR_CERT_COMMON_NAME_INVALID).
// *out_action selects the outcome:
//   0 = default (fall back to Chromium interstitial),
//   1 = Continue (silently proceed despite the error),
//   2 = Cancel   (silently cancel the request without an interstitial).
// Return: true = DSL handled (use *out_action); false = C++ falls back to the
// default certificate-error interstitial.
typedef bool (CORECLR_DELEGATE_CALLTYPE* on_certificate_error_fn)(void* browser, void* frame, int cert_error, const char* request_url, int* out_action);
typedef void (CORECLR_DELEGATE_CALLTYPE* on_before_child_process_launch_fn)(int process_type, void* command_line);
typedef bool (CORECLR_DELEGATE_CALLTYPE* on_already_running_app_relaunch_fn)(void* command_line, const char* current_directory);
typedef bool (CORECLR_DELEGATE_CALLTYPE* on_before_browse_fn)(void* browser, void* frame, void* request, bool user_gesture, bool is_redirect, bool* out_return_value);
typedef void (CORECLR_DELEGATE_CALLTYPE* on_heart_beat_fn)(int process_type, float delta_time);
typedef bool (CORECLR_DELEGATE_CALLTYPE* on_call_metadsl_fn)(const char* func_name, const uint8_t* args_blob, int args_len, char* result_str, int& result_size, void* browser, void* frame);
typedef bool (CORECLR_DELEGATE_CALLTYPE* on_console_log_fn)(void* browser, void* frame, int level, const char* message, const char* source, int line, int& max_log_size);

// DevTools observer callbacks (browser process, UI thread).
// The observer is registered per-browser in OnAfterCreated and released in
// OnBeforeClose, so |browser| is always the browser that owns the DevTools
// agent for this event. CEF does not provide a frame on these callbacks;
// C++ passes browser->GetMainFrame().get() as |frame| so managed code can
// set NativeApi context with the same (browser, frame) convention used by
// every other UI-thread callback.
typedef int  (CORECLR_DELEGATE_CALLTYPE* on_devtools_message_fn)(void* browser, void* frame, const void* msg, int size);
typedef void (CORECLR_DELEGATE_CALLTYPE* on_devtools_method_result_fn)(void* browser, void* frame, int message_id, int success, const void* result, int size);
typedef void (CORECLR_DELEGATE_CALLTYPE* on_devtools_event_fn)(void* browser, void* frame, const char* method, const void* params, int size);
typedef void (CORECLR_DELEGATE_CALLTYPE* on_devtools_agent_attached_fn)(void* browser, void* frame);
typedef void (CORECLR_DELEGATE_CALLTYPE* on_devtools_agent_detached_fn)(void* browser, void* frame);

// Resource interception hooks (browser process, IO thread).
// |handle| identifies the parked CefCallback in the generic native callback
// registry (see native_callbacks.h). It is only meaningful when C# sets
// out_return_value to RV_CONTINUE_ASYNC(2): the managed side must then call
// complete_native_callback(handle, ok) later or the request stays pending.
// For RV_CONTINUE / RV_CANCEL the handle is discarded by the caller.
typedef bool (CORECLR_DELEGATE_CALLTYPE* on_before_resource_load_fn)(void* browser, void* frame, void* request, int64_t handle, int& out_return_value);
// on_resource_response_filter: called from
// BaseClientHandler::GetResourceResponseFilter (inspection mode) with the
// actual upstream response for read-only inspection. |request| is the
// actual CEF request. Returns true to register MyResponseFilter for body
// filtering; |out_replace_content| false skips the body filter (C# only
// inspected the response).
typedef bool (CORECLR_DELEGATE_CALLTYPE* on_resource_response_filter_fn)(void* browser, void* frame, void* request, void* response, bool* out_replace_content);
// on_response_content_filter: streams body chunks through C# for transformation.
// Returns true if DSL handled the chunk (use DSL's outputs), false to pass
// through unchanged. out_status receives the filter status (0=DONE,
// 1=NEED_MORE_DATA, 2=ERROR, matches cef_response_filter_status_t).
// out_data_in_read / out_data_out_written receive consumed/written byte counts.
// C# side uses `ref int` for the three output params (matches
// on_before_resource_load_fn's int& out_return_value pattern).
// No browser/frame params: CefResourceHandler::Read / CefResponseFilter::Filter
// signatures do not carry them, and the body filter is a pure data transform.
typedef bool (CORECLR_DELEGATE_CALLTYPE* on_response_content_filter_fn)(const void* data_in, int data_in_size, void* data_out, int data_out_size, int& out_data_in_read, int& out_data_out_written, int& out_status);
// on_resource_redirect: called from BaseClientHandler::OnResourceRedirect.
// Returns true if DSL provides a replacement URL. The new URL is written to
// out_url (UTF-8) and out_url_size is set to the number of bytes written.
typedef bool (CORECLR_DELEGATE_CALLTYPE* on_resource_redirect_fn)(void* browser, void* frame, void* request, void* response, const char* new_url, char* out_url, int& out_url_size);
// on_before_resource_response: called before CEF processes response headers.
// The response is writable for status, status text, MIME type, charset and
// response-header changes during this callback.
typedef void (CORECLR_DELEGATE_CALLTYPE* on_before_resource_response_fn)(void* browser, void* frame, void* request, void* response);
// on_resource_load_complete: called after every resource load completes.
// Request and response are read-only. |status| is cef_urlrequest_status_t.
typedef void (CORECLR_DELEGATE_CALLTYPE* on_resource_load_complete_fn)(void* browser, void* frame, void* request, void* response, int status, int64_t received_content_length);
// on_protocol_execution: called for an unknown URL scheme. Return true only
// when C# supplies |out_allow_os_execution|; false preserves CEF's default.
typedef bool (CORECLR_DELEGATE_CALLTYPE* on_protocol_execution_fn)(void* browser, void* frame, void* request, bool* out_allow_os_execution);

// on_js_dialog: called for alert / confirm / prompt / beforeunload dialogs
// (browser process, UI thread). |dialog_type| is 0=alert, 1=confirm, 2=prompt,
// 3=beforeunload. |handle| identifies the parked CefJSDialogCallback in the
// generic native callback registry.
// Return value (see client::JsDialogDecision):
//   0 = not taken over, use the CEF default dialog
//   1 = taken over, managed side shows a custom dialog
//   2 = suppress the message (ignored for beforeunload)
//   3 = taken over, the script handles display and completion itself
// When taking over (1 or 3) the managed side must eventually call
// complete_native_callback(handle, ok, data) or the page hangs.
// CEF does not provide a frame on JS dialogs; C++ passes
// browser->GetMainFrame().get() as |frame| so the managed side can set
// NativeApi context uniformly. Executing JavaScript in the takeover path
// falls back to the browser's main frame anyway.
typedef int (CORECLR_DELEGATE_CALLTYPE* on_js_dialog_fn)(void* browser, void* frame, int dialog_type, const char* origin_url, const char* message_text, const char* default_prompt_text, int64_t handle);

extern on_init_fn on_init_fptr;
extern on_finalize_fn on_finalize_fptr;
extern on_browser_init_fn on_browser_init_fptr;
extern on_browser_finalize_fn on_browser_finalize_fptr;
extern on_browser_hot_reload_copyfiles_fn on_browser_hot_reload_copyfiles_fptr;
extern on_browser_hot_reload_completed_fn on_browser_hot_reload_completed_fptr;
extern on_browser_cef_query_fn on_browser_cef_query_fptr;
extern on_renderer_init_fn on_renderer_init_fptr;
extern on_renderer_finalize_fn on_renderer_finalize_fptr;
extern on_loading_state_change_fn on_loading_state_change_fptr;
extern on_load_error_fn on_load_error_fptr;
extern on_render_process_terminated_fn on_render_process_terminated_fptr;
extern on_load_start_fn on_load_start_fptr;
extern on_load_end_fn on_load_end_fptr;
extern on_renderer_load_start_fn on_renderer_load_start_fptr;
extern on_renderer_load_end_fn on_renderer_load_end_fptr;
extern on_renderer_loading_state_change_fn on_renderer_loading_state_change_fptr;
extern on_renderer_load_error_fn on_renderer_load_error_fptr;

extern on_receive_cef_message_fn on_receive_cef_message_fptr;
extern on_execute_metadsl_fn on_execute_metadsl_fptr;
extern on_before_command_line_processing_fn on_before_command_line_processing_fptr;
extern on_get_auth_credentials_fn on_get_auth_credentials_fptr;
extern on_request_media_access_permission_fn on_request_media_access_permission_fptr;
extern on_show_permission_prompt_fn on_show_permission_prompt_fptr;
extern on_certificate_error_fn on_certificate_error_fptr;
extern on_before_child_process_launch_fn on_before_child_process_launch_fptr;
extern on_already_running_app_relaunch_fn on_already_running_app_relaunch_fptr;
extern on_before_browse_fn on_before_browse_fptr;
extern on_before_resource_load_fn on_before_resource_load_fptr;
extern on_heart_beat_fn on_heart_beat_fptr;
extern on_call_metadsl_fn on_call_metadsl_fptr;
extern on_console_log_fn on_console_log_fptr;

extern on_devtools_message_fn on_devtools_message_fptr;
extern on_devtools_method_result_fn on_devtools_method_result_fptr;
extern on_devtools_event_fn on_devtools_event_fptr;
extern on_devtools_agent_attached_fn on_devtools_agent_attached_fptr;
extern on_devtools_agent_detached_fn on_devtools_agent_detached_fptr;

extern on_resource_response_filter_fn on_resource_response_filter_fptr;
extern on_response_content_filter_fn on_response_content_filter_fptr;
extern on_resource_redirect_fn on_resource_redirect_fptr;
extern on_before_resource_response_fn on_before_resource_response_fptr;
extern on_resource_load_complete_fn on_resource_load_complete_fptr;
extern on_protocol_execution_fn on_protocol_execution_fptr;

extern on_js_dialog_fn on_js_dialog_fptr;

// Start/stop heartbeat timer
extern void StartHeartbeat(int process_type);
extern void StopHeartbeat();
extern void SetHeartbeatIntervalMs(int interval_ms);

// Renderer ref map: hold CefRefPtr on both browser and its main frame to
// prevent premature release while C# may still hold raw pointers. Refs are
// captured at main-frame OnContextCreated and released at OnContextReleased.
extern void renderer_ref_add(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame);
extern void renderer_ref_remove(CefRefPtr<CefBrowser> browser);

// Browser ref map (browser process): hold CefRefPtr to keep browser alive
// between OnAfterCreated and OnBeforeClose so C# raw pointers stay valid.
extern void browser_ref_add(CefRefPtr<CefBrowser> browser);
extern void browser_ref_remove(CefRefPtr<CefBrowser> browser);
// Called from CefFrameHandler::OnMainFrameChanged to keep the stored main
// frame ref in sync with cross-origin navigations and renderer crash recovery.
extern void browser_ref_update_frame(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame);

// Cross-platform function to terminate renderer processes
// Returns the number of renderer processes terminated, or -1 on error
extern int TerminateRenderProcess();

// Count renderer processes using platform-specific APIs
// Returns the number of renderer processes, or -1 on error
extern int CountRenderProcess();

// DevTools observer registration (browser process, UI thread).
// Called from BaseClientHandler::OnAfterCreated / OnBeforeClose.
extern void RegisterDevToolsObserver(CefBrowser* browser);
extern void UnregisterDevToolsObserver(CefBrowser* browser);