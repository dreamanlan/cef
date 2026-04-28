#pragma once
#include "coreclr/coreclr_delegates.h"
#include "path_utils.h"
#include "include/cef_browser.h"
#include "include/cef_frame.h"
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
typedef void (CORECLR_DELEGATE_CALLTYPE* on_render_process_terminated_fn)(void* browser, void* frame, const char* startup_url, const char* url, int status, int error_code, const char* error_string);
typedef void (CORECLR_DELEGATE_CALLTYPE* on_load_start_fn)(void* browser, void* frame, const char* url, int transition_type, bool is_main);
typedef bool (CORECLR_DELEGATE_CALLTYPE* on_load_end_fn)(void* browser, void* frame, const char* url, int http_status_code, bool inject_all_frame, bool is_main, char* js_code, int& code_size);
typedef void (CORECLR_DELEGATE_CALLTYPE* on_renderer_load_start_fn)(void* browser, void* frame, const char* url, int transition_type, bool is_main);
typedef bool (CORECLR_DELEGATE_CALLTYPE* on_renderer_load_end_fn)(void* browser, void* frame, const char* url, int http_status_code, bool is_main, char* js_code, int& code_size);
typedef void (CORECLR_DELEGATE_CALLTYPE* on_renderer_loading_state_change_fn)(void* browser, void* frame, const char* url, bool is_loading, bool can_go_back, bool can_go_forward);
typedef void (CORECLR_DELEGATE_CALLTYPE* on_renderer_load_error_fn)(void* browser, void* frame, int error_code, const char* error_text, const char* failed_url);
typedef void (CORECLR_DELEGATE_CALLTYPE* on_receive_cef_message_fn)(const char* message, const char** args, int arg_count, void* browser, void* frame, int source_process_id);
typedef bool (CORECLR_DELEGATE_CALLTYPE* on_receive_js_message_fn)(const char* message, const char** args, int arg_count, char* result_str, int& result_size, void* browser, void* frame);
typedef bool (CORECLR_DELEGATE_CALLTYPE* on_execute_metadsl_fn)(const char** args, int arg_count, char* result_str, int& result_size, void* browser, void* frame);
typedef void (CORECLR_DELEGATE_CALLTYPE* on_before_command_line_processing_fn)(int process_type, void* command_line);
typedef void (CORECLR_DELEGATE_CALLTYPE* on_before_child_process_launch_fn)(int process_type, void* command_line);
typedef bool (CORECLR_DELEGATE_CALLTYPE* on_already_running_app_relaunch_fn)(void* command_line, const char* current_directory);
typedef bool (CORECLR_DELEGATE_CALLTYPE* on_before_browse_fn)(void* browser, void* frame, void* request, bool user_gesture, bool is_redirect, bool* out_return_value);
typedef bool (CORECLR_DELEGATE_CALLTYPE* on_before_resource_load_fn)(void* browser, void* frame, void* request, int* out_return_value);
typedef void (CORECLR_DELEGATE_CALLTYPE* on_heart_beat_fn)(int process_type, float delta_time);
typedef bool (CORECLR_DELEGATE_CALLTYPE* on_call_metadsl_fn)(const char* func_name, const char** args, int arg_count, char* result_str, int& result_size, void* browser, void* frame);
typedef bool (CORECLR_DELEGATE_CALLTYPE* on_console_log_fn)(void* browser, int level, const char* message, const char* source, int line, int& max_log_size);

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
extern on_receive_js_message_fn on_receive_js_message_fptr;
extern on_execute_metadsl_fn on_execute_metadsl_fptr;
extern on_before_command_line_processing_fn on_before_command_line_processing_fptr;
extern on_before_child_process_launch_fn on_before_child_process_launch_fptr;
extern on_already_running_app_relaunch_fn on_already_running_app_relaunch_fptr;
extern on_before_browse_fn on_before_browse_fptr;
extern on_before_resource_load_fn on_before_resource_load_fptr;
extern on_heart_beat_fn on_heart_beat_fptr;
extern on_call_metadsl_fn on_call_metadsl_fptr;
extern on_console_log_fn on_console_log_fptr;

// Start/stop heartbeat timer
extern void StartHeartbeat(int process_type);
extern void StopHeartbeat();
extern void SetHeartbeatIntervalMs(int interval_ms);

// Renderer ref map: hold CefRefPtr to prevent premature release of browser/frame objects
extern void renderer_ref_add(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame);
extern void renderer_ref_remove(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame);
extern void renderer_ref_clear();

// Cross-platform function to terminate renderer processes
// Returns the number of renderer processes terminated, or -1 on error
extern int TerminateRenderProcess();

// Count renderer processes using platform-specific APIs
// Returns the number of renderer processes, or -1 on error
extern int CountRenderProcess();