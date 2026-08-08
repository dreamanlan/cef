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
typedef int (CORECLR_DELEGATE_CALLTYPE* on_browser_cef_query_fn)(void* browser, void* frame, int64_t query_id, const char* request, bool persistent);
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
typedef void (CORECLR_DELEGATE_CALLTYPE* on_receive_cef_message_fn)(const char* message, const char** args, int arg_count, void* browser, void* frame, int source_process_id);
typedef bool (CORECLR_DELEGATE_CALLTYPE* on_execute_metadsl_fn)(const char** args, int arg_count, char* result_str, int& result_size, void* browser, void* frame);
typedef void (CORECLR_DELEGATE_CALLTYPE* on_before_command_line_processing_fn)(int process_type, void* command_line);
typedef void (CORECLR_DELEGATE_CALLTYPE* on_before_child_process_launch_fn)(int process_type, void* command_line);
typedef bool (CORECLR_DELEGATE_CALLTYPE* on_already_running_app_relaunch_fn)(void* command_line, const char* current_directory);
typedef bool (CORECLR_DELEGATE_CALLTYPE* on_before_browse_fn)(void* browser, void* frame, void* request, bool user_gesture, bool is_redirect, bool* out_return_value);
typedef void (CORECLR_DELEGATE_CALLTYPE* on_heart_beat_fn)(int process_type, float delta_time);
typedef bool (CORECLR_DELEGATE_CALLTYPE* on_call_metadsl_fn)(const char* func_name, const char** args, int arg_count, char* result_str, int& result_size, void* browser, void* frame);
typedef bool (CORECLR_DELEGATE_CALLTYPE* on_console_log_fn)(void* browser, int level, const char* message, const char* source, int line, int& max_log_size);

// DevTools observer callbacks (browser process, UI thread)
typedef int  (CORECLR_DELEGATE_CALLTYPE* on_devtools_message_fn)(void* browser, const void* msg, int size);
typedef void (CORECLR_DELEGATE_CALLTYPE* on_devtools_method_result_fn)(void* browser, int message_id, int success, const void* result, int size);
typedef void (CORECLR_DELEGATE_CALLTYPE* on_devtools_event_fn)(void* browser, const char* method, const void* params, int size);
typedef void (CORECLR_DELEGATE_CALLTYPE* on_devtools_agent_attached_fn)(void* browser);
typedef void (CORECLR_DELEGATE_CALLTYPE* on_devtools_agent_detached_fn)(void* browser);

// Resource interception hooks (browser process, IO thread).
typedef bool (CORECLR_DELEGATE_CALLTYPE* on_before_resource_load_fn)(void* browser, void* frame, void* request, int& out_return_value);
// on_get_resource_handler_filter: called from
// BaseClientHandler::GetResourceHandler (decision mode). Returns true to
// intercept the resource with MyResourceHandler.
// |request| is the original CEF request (read-only here). Its ResourceType /
// TransitionType / Identifier are always authoritative.
// |request_override| is a mutable copy of the upstream request: DSL may edit
// its URL / headers / referrer / flags via CefRequest setters, and the same
// object is reused by MyResourceHandler when creating the forwarded
// CefURLRequest (no second copy). Note: ResourceType / TransitionType /
// Identifier are NOT copied (CEF has no public setters); read them from
// |request| if needed.
// |response_override| is an empty writable CefResponse for C# to fill header
// overrides into.
// |out_replace_content| receives whether to enable body filtering:
//   false = MyResourceHandler only applies header overrides, passes the body
//           through unchanged.
//   true  = enable body filter (default).
// |io_want_cookies| carries the cookie-snapshot budget. On entry it holds
// the number of cookie queries issued so far (so DSL knows whether its cap
// is reached). On return:
//   n > (entry value) = request a cookie-jar snapshot for this request,
//     delivered via on_resource_cookie_list once the upstream response
//     headers arrive (jar has ingested the response's Set-Cookie by then).
//     The returned n acts as the DSL-specified global cap.
//   n <= 0 = decline and reset the issued count to 0. Pending snapshots
//     armed before the reset are invalidated.
typedef bool (CORECLR_DELEGATE_CALLTYPE* on_get_resource_handler_filter_fn)(void* browser, void* frame, void* request, void* request_override, void* response_override, bool* out_replace_content, int& io_want_cookies);
// on_resource_response_filter: called from
// BaseClientHandler::GetResourceResponseFilter (inspection mode) with the
// actual upstream response for read-only inspection. |request| is the
// actual CEF request. Returns true to register MyResponseFilter for body
// filtering; |out_replace_content| false skips the body filter (C# only
// inspected the response).
typedef bool (CORECLR_DELEGATE_CALLTYPE* on_resource_response_filter_fn)(void* browser, void* frame, void* request, void* response, bool* out_replace_content);
// on_resource_cookie_list: delivers a completed cookie-jar snapshot
// (CookieListBridge, see browser/cookie_list_bridge.h) requested via
// io_want_cookies. |cookie_list| is valid only for the duration of this
// call. Field accessors are the cookie_list_* host APIs.
typedef void (CORECLR_DELEGATE_CALLTYPE* on_resource_cookie_list_fn)(void* cookie_list);
// on_resource_auth_challenge: called on the CEF IO thread when a forwarded
// CefURLRequest receives a real HTTP/proxy authentication challenge. Returns
// true only when C# accepted ownership of the challenge and will resolve it
// through HostApi::ReplyResourceAuthCredentials; false cancels it immediately.
// |browser| is the originating CefBrowser and is valid only for this callback
// (as with other IO-thread callbacks). C# clears its temporary native context
// before returning; pending challenge state retains no raw browser pointer.
// No password is passed in this direction. |challenge_id| is one-shot and
// expires after the native timeout.
typedef bool (CORECLR_DELEGATE_CALLTYPE* on_resource_auth_challenge_fn)(void* browser, uint64_t challenge_id, const char* url, int is_proxy, const char* host, int port, const char* realm, const char* scheme);
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

extern on_get_resource_handler_filter_fn on_get_resource_handler_filter_fptr;
extern on_resource_response_filter_fn on_resource_response_filter_fptr;
extern on_resource_cookie_list_fn on_resource_cookie_list_fptr;
extern on_resource_auth_challenge_fn on_resource_auth_challenge_fptr;
extern on_response_content_filter_fn on_response_content_filter_fptr;
extern on_resource_redirect_fn on_resource_redirect_fptr;

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