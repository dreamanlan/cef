#include "HostCLR.h"
#include "include/base/cef_logging.h"
#include "include/cef_command_line.h"
#include "include/cef_browser.h"
#include "include/cef_frame.h"
#include "include/cef_request.h"
#include "include/cef_task.h"
#include "include/cef_parser.h"
#include "include/cef_devtools_message_observer.h"
#include "JavaScriptCaller.h"
#include "path_utils.h"

#include <chrono>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

// Cross-platform string literal macro for char_t
#if defined(_MSC_VER)
    #define CHAR_T_LITERAL(str) L##str
#else
    #define CHAR_T_LITERAL(str) str
#endif

#if defined(_MSC_VER)
#include "windows.h"
// Ensure NTSTATUS and other NT types are available
#ifndef NTSTATUS
typedef LONG NTSTATUS;
#endif
typedef struct _UNICODE_STRING {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR  Buffer;
} UNICODE_STRING, *PUNICODE_STRING;
typedef struct _MY_PROCESS_BASIC_INFORMATION {
    PVOID Reserved1;
    PVOID PebBaseAddress;
    PVOID Reserved2[2];
    ULONG_PTR UniqueProcessId;
    PVOID Reserved3;
} MY_PROCESS_BASIC_INFORMATION, *PMY_PROCESS_BASIC_INFORMATION;

// Helper function to check if memory is readable before ReadProcessMemory
bool IsMemoryReadable(HANDLE process, LPCVOID address) {
    MEMORY_BASIC_INFORMATION mbi;
    SIZE_T result = VirtualQueryEx(process, address, &mbi, sizeof(mbi));
    if (result == 0) {
        return false;
    }
    // Check if memory is committed and has read access
    return (mbi.State == MEM_COMMIT) &&
           (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE));
}

#include "coreclr/nethost.h"
#include "coreclr/coreclr_delegates.h"
#include "coreclr/hostfxr.h"

// Related to the path "out"
#pragma comment(lib, "../../cef/tests/cefclient/hostclr/coreclr/nethost.lib")

#elif defined(__APPLE__)
#include <TargetConditionals.h>
#if TARGET_OS_OSX
#include <iostream>
#include <dlfcn.h>
#include <signal.h>
#include <sys/sysctl.h>
#include <mach-o/dyld.h>
#include "coreclr/nethost.h"
#include "coreclr/coreclr_delegates.h"
#include "coreclr/hostfxr.h"
#endif
#elif defined(__linux__)
#include <syslog.h>
#endif

// Cross-platform character set conversion functions
std::string WideStringToUtf8(const wchar_t* wstr)
{
    if (!wstr) return std::string();

#if defined(_MSC_VER)
    int size_needed = ::WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
    if (size_needed == 0) {
        return std::string();
    }

    std::string result;
    result.resize(size_needed - 1, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wstr, -1, &result[0], size_needed, nullptr, nullptr);
    return result;
#else
    // On non-Windows platforms, use standard library conversion
    std::mbstate_t state = std::mbstate_t();
    std::size_t len = std::wcsrtombs(nullptr, &wstr, 0, &state);
    if (len == static_cast<std::size_t>(-1)) {
        return std::string();
    }
    std::string result(len, '\0');
    std::wcsrtombs(&result[0], &wstr, len, &state);
    return result;
#endif
}

std::wstring Utf8ToWstring(const char* str)
{
    if (!str) return std::wstring();

#if defined(_MSC_VER)
    int size_needed = ::MultiByteToWideChar(CP_UTF8, 0, str, -1, nullptr, 0);
    if (size_needed == 0) {
        return std::wstring();
    }

    std::wstring wstr;
    wstr.resize(size_needed, '\0');
    ::MultiByteToWideChar(CP_UTF8, 0, str, -1, &wstr[0], size_needed);

    if (!wstr.empty()) wstr.pop_back();
    return wstr;
#else
    // On non-Windows platforms, use standard library conversion
    std::mbstate_t state = std::mbstate_t();
    const char* ptr = str;
    std::size_t len = std::mbsrtowcs(nullptr, &ptr, 0, &state);
    if (len == static_cast<std::size_t>(-1)) {
        return std::wstring();
    }
    std::wstring result(len, L'\0');
    ptr = str;
    std::mbsrtowcs(&result[0], &ptr, len, &state);
    return result;
#endif
}

bool WideToUtf8ToBuffer(const wchar_t* wstr, char* buf, int bufSize)
{
    if (!wstr || !buf || bufSize <= 0) return false;

#if defined(_MSC_VER)
    int needed = ::WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
    if (needed == 0 || needed > bufSize) return false;
    ::WideCharToMultiByte(CP_UTF8, 0, wstr, -1, buf, needed, nullptr, nullptr);
    return true;
#else
    std::mbstate_t state = std::mbstate_t();
    std::size_t len = std::wcsrtombs(nullptr, &wstr, 0, &state);
    if (len == static_cast<std::size_t>(-1) || static_cast<int>(len) >= bufSize) {
        return false;
    }
    std::wcsrtombs(buf, &wstr, bufSize, &state);
    return true;
#endif
}

bool MultiByteToWideBuffer(const char* src, wchar_t* buf, int bufSize, unsigned int codePage)
{
    if (!src || !buf || bufSize <= 0) return false;

#if defined(_MSC_VER)
    int needed = ::MultiByteToWideChar(codePage, 0, src, -1, nullptr, 0);
    if (needed == 0 || needed > bufSize) return false;
    int ret = ::MultiByteToWideChar(codePage, 0, src, -1, buf, bufSize);
    return ret != 0;
#else
    // On non-Windows platforms, ignore codePage and use standard conversion
    std::mbstate_t state = std::mbstate_t();
    const char* ptr = src;
    std::size_t len = std::mbsrtowcs(nullptr, &ptr, 0, &state);
    if (len == static_cast<std::size_t>(-1) || static_cast<int>(len) >= bufSize) {
        return false;
    }
    ptr = src;
    std::mbsrtowcs(buf, &ptr, bufSize, &state);
    return true;
#endif
}

void* load_library(const char* path)
{
#if defined(_MSC_VER)
    //HMODULE h = ::LoadLibraryA(path);
    //return reinterpret_cast<void*>(h);
    wchar_t wpath[1025];
    MultiByteToWideBuffer(path, wpath, 1024);
    HMODULE h = LoadLibraryW(wpath);
    return reinterpret_cast<void*>(h);
#else
    return dlopen(path, RTLD_LAZY | RTLD_LOCAL);
#endif
}

void* get_export(void* h, const char* name)
{
#if defined(_MSC_VER)
    return reinterpret_cast<void*>(::GetProcAddress(reinterpret_cast<HMODULE>(h), name));
#else
    return dlsym(h, name);
#endif
}

void free_library(void* h)
{
#if defined(_MSC_VER)
    ::FreeLibrary(reinterpret_cast<HMODULE>(h));
#else
    dlclose(h);
#endif
}

void printf_log(LogSeverity severity, const char* fmt, ...)
{
    va_list vl;
    va_start(vl, fmt);
    char buffer[4097];
    int len = vsnprintf(buffer, sizeof(buffer) - 1, fmt, vl);
    va_end(vl);
    // Guard against vsnprintf returning negative (error) or exceeding buffer size
    if (len < 0) {
        len = 0;
    } else if (len >= static_cast<int>(sizeof(buffer) - 1)) {
        len = static_cast<int>(sizeof(buffer) - 1);
    }
    buffer[len] = '\0';

    if (severity == LOG_SEVERITY_ERROR) {
        LOG(ERROR) << buffer;
    } else {
        // Both WARNING and INFO use LOG(WARNING)
        LOG(WARNING) << buffer;
    }

#if defined(_MSC_VER)
    // Convert UTF-8 to wide char for OutputDebugString
    std::wstring wbuffer = Utf8ToWstring(buffer);
    ::OutputDebugStringW(wbuffer.c_str());
#elif defined(__APPLE__)
    // Avoid os_log_with_type which can conflict with .NET CLR signal handlers,
    // causing the main thread to hang during PAL_DispatchException.
    fprintf(stderr, "%s\n", buffer);
#elif defined(__linux__)
    syslog(severity == LOG_SEVERITY_ERROR ? LOG_ERR : LOG_WARNING,
        "%s", buffer);
#endif
}

[[maybe_unused]]static void convert_separators_to_platform(std::string& pathName)
{
#if _MSC_VER
    std::string::iterator it = pathName.begin(), itEnd = pathName.end();
    while (it != itEnd)
    {
        if (*it == '/')
            *it = '\\';
        ++it;
    }
#endif
}

// Initialize absolute paths based on executable location
// Use GetExeDir() from path_utils.h, which returns directory with trailing separator
static std::string GetExeDirWithSeparator() {
    std::string dir = GetExeDir();
    if (dir.empty()) {
        return "./";
    }
    // Ensure trailing separator
    if (dir.back() != '/' && dir.back() != '\\') {
#if defined(_MSC_VER)
        dir += '\\';
#else
        dir += '/';
#endif
    }
    return dir;
}

// Helper functions to build paths dynamically (no static storage, no memory leak)
#if defined(_MSC_VER)
// Windows: use wide strings
typedef std::wstring string_t;
#define STR_LITERAL(s) L##s

static string_t GetAppBaseDirString() {
    return Utf8ToWstring(GetExeDirWithSeparator().c_str());
}
#else
// Unix: use narrow strings
typedef std::string string_t;
#define STR_LITERAL(s) s

static string_t GetAppBaseDirString() {
#if defined(__APPLE__)
    // On macOS, return the outermost (main) .app directory path with trailing separator.
    // This ensures Helper processes (inside Frameworks/) also find the main app's path.
    std::string appPath = GetMacMainAppDirPath();
    if (appPath.empty()) {
        return GetExeDirWithSeparator();
    }
    // Ensure trailing separator
    if (appPath.back() != '/') {
        appPath += '/';
    }
    return appPath;
#else
    return GetExeDirWithSeparator();
#endif
}
#endif

// Build paths dynamically based on debug/release mode
static string_t BuildManagedDllDir(bool is_debug) {
    string_t base = GetAppBaseDirString();
#if defined(__APPLE__)
    return is_debug ? (base + STR_LITERAL("../cefclient.app/Contents/managed/")) : base + STR_LITERAL("Contents/managed/");
#else
    return is_debug ? (base + STR_LITERAL("../cefclient/managed/")) : base + STR_LITERAL("managed/");
#endif
}

static string_t BuildDotnetRuntimeDir(bool is_debug) {
    string_t base = GetAppBaseDirString();
#if defined(__APPLE__)
    return is_debug ? (base + STR_LITERAL("../cefclient.app/Contents/dotnet/Microsoft.NETCore.App/9.0.2")) : base + STR_LITERAL("Contents/dotnet/Microsoft.NETCore.App/9.0.2");
#else
    if (is_debug) {
        return base + STR_LITERAL("../cefclient/dotnet/Microsoft.NETCore.App/9.0.2");
    }
    return base + STR_LITERAL("dotnet/Microsoft.NETCore.App/9.0.2");
#endif
}

static string_t BuildRuntimeConfigPath(bool is_debug) {
    string_t base = GetAppBaseDirString();
#if defined(__APPLE__)
    return is_debug ? (base + STR_LITERAL("../cefclient.app/Contents/managed/CefDotnetApp.runtimeconfig.json")) : base + STR_LITERAL("Contents/managed/CefDotnetApp.runtimeconfig.json");
#else
    if (is_debug) {
        return base + STR_LITERAL("../cefclient/managed/CefDotnetApp.runtimeconfig.json");
    }
    return base + STR_LITERAL("managed/CefDotnetApp.runtimeconfig.json");
#endif
}

static string_t BuildAssemblyPath(bool is_debug) {
    string_t base = GetAppBaseDirString();
#if defined(__APPLE__)
    return is_debug ? (base + STR_LITERAL("../cefclient.app/Contents/managed/CefDotnetApp.dll")) : base + STR_LITERAL("Contents/managed/CefDotnetApp.dll");
#else
    if (is_debug) {
        return base + STR_LITERAL("../cefclient/managed/CefDotnetApp.dll");
    }
    return base + STR_LITERAL("managed/CefDotnetApp.dll");
#endif
}

#if defined(_MSC_VER)
static const wchar_t* c_dotnet_class_name = L"DotNetLib.Lib, CefDotnetApp";
#else
static const char* c_dotnet_class_name = "DotNetLib.Lib, CefDotnetApp";
#endif
static load_assembly_and_get_function_pointer_fn load_assembly_and_get_function_pointer = nullptr;
// Function to initialize .NET Core runtime
int load_hostfxr(bool is_debug, int& out_rc)
{
    // Build paths dynamically based on debug mode
    string_t dotnet_runtime_config_path = BuildRuntimeConfigPath(is_debug);
    string_t local_managed_dll_dir = BuildManagedDllDir(is_debug);
    string_t local_dotnet_runtime_dir = BuildDotnetRuntimeDir(is_debug);
    string_t dotnet_assembly_path = BuildAssemblyPath(is_debug);

    out_rc = 0;
#ifdef USE_SPEC_DOTNET
    // Load hostfxr.dll and use dotnet framework in specific directory
    const char* hostfxr_path = "hostfxr.dll";
    void* hostfxr_lib = load_library(hostfxr_path);
    if (!hostfxr_lib)
    {
        printf_log(LOG_SEVERITY_ERROR, "Failed to load hostfxr.dll");
        return -2;
    }
#else
#ifdef _WIN32
    wchar_t hostfxr_path_w[1024];
    size_t sz = sizeof(hostfxr_path_w) / sizeof(wchar_t);
    int rc0 = get_hostfxr_path(hostfxr_path_w, &sz, nullptr);
    if (rc0 != 0) {
        printf_log(LOG_SEVERITY_ERROR, "get_hostfxr_path failed: %d (0x%x)", rc0, rc0);
        out_rc = rc0;
        return -1;
    }
    char path[1025];
    WideToUtf8ToBuffer(hostfxr_path_w, path, 1024);
    printf_log(LOG_SEVERITY_INFO, "[native] hostfxr path: %s\n", path);
#else
    char hostfxr_path[PATH_MAX];
    size_t sz = sizeof(hostfxr_path);
    int rc0 = get_hostfxr_path(hostfxr_path, &sz, nullptr);
    if (rc0 != 0) {
        printf_log(LOG_SEVERITY_ERROR, "get_hostfxr_path failed: %d (0x%x)", rc0, rc0);
        out_rc = rc0;
        return -1;
    }
    printf_log(LOG_SEVERITY_INFO, "[native] hostfxr path: %s\n", hostfxr_path);
#endif

#ifdef _WIN32
    HMODULE hostfxr_lib = LoadLibraryW(hostfxr_path_w);
    if (!hostfxr_lib) {
        out_rc = static_cast<int>(::GetLastError());
        printf_log(LOG_SEVERITY_ERROR, "LoadLibraryW failed\n");
        return -2;
    }
#else
    void* hostfxr_lib = load_library(hostfxr_path);
    if (!hostfxr_lib) {
        const char* dl_err = dlerror();
        printf_log(LOG_SEVERITY_ERROR, "dlopen failed: %s\n", dl_err ? dl_err : "unknown error");
        return -2;
    }
#endif

#endif

    auto init_cmdline_fptr = reinterpret_cast<hostfxr_initialize_for_dotnet_command_line_fn>(get_export(hostfxr_lib, "hostfxr_initialize_for_dotnet_command_line"));
    auto run_app_fptr = reinterpret_cast<hostfxr_run_app_fn>(get_export(hostfxr_lib, "hostfxr_run_app"));
    auto init_config_fptr = reinterpret_cast<hostfxr_initialize_for_runtime_config_fn>(get_export(hostfxr_lib, "hostfxr_initialize_for_runtime_config"));
    auto get_delegate_fptr = reinterpret_cast<hostfxr_get_runtime_delegate_fn>(get_export(hostfxr_lib, "hostfxr_get_runtime_delegate"));
    auto close_fptr = reinterpret_cast<hostfxr_close_fn>(get_export(hostfxr_lib, "hostfxr_close"));

    if (!init_cmdline_fptr || !run_app_fptr || !init_config_fptr || !get_delegate_fptr || !close_fptr)
    {
        printf_log(LOG_SEVERITY_ERROR, "Failed to get hostfxr functions");
        return -3;
    }

#ifdef USE_SPEC_DOTNET
    // Initialize the .NET Core runtime
    hostfxr_initialize_parameters parameters{
        sizeof(hostfxr_initialize_parameters),
        local_managed_dll_dir.c_str(),
        local_dotnet_runtime_dir.c_str()
    };

    hostfxr_handle cxt = nullptr;
    int rc = init_config_fptr(dotnet_runtime_config_path.c_str(), &parameters, &cxt);
#else
    hostfxr_handle cxt = nullptr;
    int rc = init_config_fptr(dotnet_runtime_config_path.c_str(), nullptr, &cxt);
#endif
    //int argc = 1;
    //const char_t* argv[] = { dotnet_assembly_path };
    //int rc = init_cmdline_fptr(argc, argv, &parameters, &cxt);
    if (rc != 0 || cxt == nullptr)
    {
        printf_log(LOG_SEVERITY_ERROR, "Failed to initialize .NET Core runtime: %d (0x%x)", rc, rc);
        out_rc = rc;
        return -4;
    }

    // Get the delegate for the runtime
    rc = get_delegate_fptr(cxt, hdt_load_assembly_and_get_function_pointer, reinterpret_cast<void**>(&load_assembly_and_get_function_pointer));
    if (rc != 0 || load_assembly_and_get_function_pointer == nullptr)
    {
        printf_log(LOG_SEVERITY_ERROR, "Failed to get load_assembly_and_get_function_pointer: %d (0x%x)", rc, rc);
        out_rc = rc;
        return -5;
    }

    //run_app_fptr(cxt);

    // Close the host context
    close_fptr(cxt);

    return 0;
}

// Function pointers to call dotnet methods
on_init_fn on_init_fptr = nullptr;
on_finalize_fn on_finalize_fptr = nullptr;
on_browser_init_fn on_browser_init_fptr = nullptr;
on_browser_finalize_fn on_browser_finalize_fptr = nullptr;
on_browser_hot_reload_copyfiles_fn on_browser_hot_reload_copyfiles_fptr = nullptr;
on_browser_hot_reload_completed_fn on_browser_hot_reload_completed_fptr = nullptr;
on_browser_cef_query_fn on_browser_cef_query_fptr = nullptr;
on_renderer_init_fn on_renderer_init_fptr = nullptr;
on_renderer_finalize_fn on_renderer_finalize_fptr = nullptr;
on_loading_state_change_fn on_loading_state_change_fptr = nullptr;
on_load_error_fn on_load_error_fptr = nullptr;
on_render_process_terminated_fn on_render_process_terminated_fptr = nullptr;
on_load_start_fn on_load_start_fptr = nullptr;
on_load_end_fn on_load_end_fptr = nullptr;
on_renderer_load_start_fn on_renderer_load_start_fptr = nullptr;
on_renderer_load_end_fn on_renderer_load_end_fptr = nullptr;
on_renderer_loading_state_change_fn on_renderer_loading_state_change_fptr = nullptr;
on_renderer_load_error_fn on_renderer_load_error_fptr = nullptr;

on_receive_cef_message_fn on_receive_cef_message_fptr = nullptr;
on_execute_metadsl_fn on_execute_metadsl_fptr = nullptr;
on_before_command_line_processing_fn on_before_command_line_processing_fptr = nullptr;
on_before_child_process_launch_fn on_before_child_process_launch_fptr = nullptr;
on_already_running_app_relaunch_fn on_already_running_app_relaunch_fptr = nullptr;
on_before_browse_fn on_before_browse_fptr = nullptr;
on_heart_beat_fn on_heart_beat_fptr = nullptr;
on_call_metadsl_fn on_call_metadsl_fptr = nullptr;
on_console_log_fn on_console_log_fptr = nullptr;

// DevTools observer callbacks
on_devtools_message_fn on_devtools_message_fptr = nullptr;
on_devtools_method_result_fn on_devtools_method_result_fptr = nullptr;
on_devtools_event_fn on_devtools_event_fptr = nullptr;
on_devtools_agent_attached_fn on_devtools_agent_attached_fptr = nullptr;
on_devtools_agent_detached_fn on_devtools_agent_detached_fptr = nullptr;

// Resource interception callbacks
on_before_resource_load_fn on_before_resource_load_fptr = nullptr;
on_resource_response_filter_fn on_resource_response_filter_fptr = nullptr;
on_response_content_filter_fn on_response_content_filter_fptr = nullptr;



// Ref containers (browser+renderer) are protected by a single mutex.
// Use Meyers local-static + heap allocation so containers are never destroyed
// during static teardown (avoids releasing CefRefPtrs after CEF has shut down).
static std::mutex& GetRefContainersMutex() {
    static auto* s_m = new std::mutex();
    return *s_m;
}

// Renderer process: hold CefRefPtr to keep both the browser AND its main frame
// alive while C# may still touch either. Both refs are captured from the main
// frame's OnContextCreated (which provides a live CefFrame param) and released
// on OnContextReleased. Storing the frame ref prevents use-after-free when
// C# reads a raw frame pointer whose lifetime is otherwise not guaranteed.
static std::map<int, std::pair<CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>>>&
GetRendererRefMap() {
    static auto* s_map = new std::map<int, std::pair<CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>>>();
    return *s_map;
}
// Valid-set of raw browser pointers currently registered in renderer process.
// Used by browser_is_valid without dereferencing the input pointer.
static std::unordered_set<CefBrowser*>&
GetRendererValidSet() {
    static auto* s_set = new std::unordered_set<CefBrowser*>();
    return *s_set;
}

// Browser process: hold CefRefPtr for both the browser AND its main frame
// between OnAfterCreated and OnBeforeClose. The main frame is grabbed once at
// OnAfterCreated via browser->GetMainFrame(); if it later swaps (cross-site
// navigation), our stored ref may go stale, but the CToCpp wrapper stays alive
// and safe to inspect (methods degrade gracefully instead of UAF). Holding it
// gives future accessors a stable raw pointer without requiring extra hooks.
static std::map<int, std::pair<CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>>>&
GetBrowserRefMap() {
    static auto* s_map = new std::map<int, std::pair<CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>>>();
    return *s_map;
}
static std::unordered_set<CefBrowser*>&
GetBrowserValidSet() {
    static auto* s_set = new std::unordered_set<CefBrowser*>();
    return *s_set;
}

// Heartbeat implementation
static bool g_heartbeat_running = false;
static int g_heartbeat_process_type = 0;
static int g_heartbeat_interval_ms = 100;
static std::chrono::steady_clock::time_point g_heartbeat_last_time;
class HeartbeatTask : public CefTask {
 public:
  HeartbeatTask() = default;
  void Execute() override {
    if (!g_heartbeat_running || !on_heart_beat_fptr) {
      return;
    }
    auto now = std::chrono::steady_clock::now();
    float delta_ms = std::chrono::duration<float, std::milli>(now - g_heartbeat_last_time).count();
    g_heartbeat_last_time = now;
    on_heart_beat_fptr(g_heartbeat_process_type, delta_ms);
    // Schedule next heartbeat
    if (g_heartbeat_running) {
      cef_thread_id_t tid = (g_heartbeat_process_type == 0) ? TID_UI : TID_RENDERER;
      CefPostDelayedTask(tid, new HeartbeatTask(), g_heartbeat_interval_ms);
    }
  }
 private:
  IMPLEMENT_REFCOUNTING(HeartbeatTask);
  DISALLOW_COPY_AND_ASSIGN(HeartbeatTask);
};

void StartHeartbeat(int process_type) {
  if (g_heartbeat_running) {
    return;
  }
  g_heartbeat_running = true;
  g_heartbeat_process_type = process_type;
  g_heartbeat_last_time = std::chrono::steady_clock::now();
  cef_thread_id_t tid = (process_type == 0) ? TID_UI : TID_RENDERER;
  CefPostDelayedTask(tid, new HeartbeatTask(), g_heartbeat_interval_ms);
  printf_log(LOG_SEVERITY_INFO, "[native] Heartbeat started for process_type=%d interval=%dms", process_type, g_heartbeat_interval_ms);
}

void StopHeartbeat() {
  g_heartbeat_running = false;
  printf_log(LOG_SEVERITY_INFO, "[native] Heartbeat stopped");
}

void SetHeartbeatIntervalMs(int interval_ms) {
  if (interval_ms < 10) interval_ms = 10;
  if (interval_ms > 60000) interval_ms = 60000;
  g_heartbeat_interval_ms = interval_ms;
  printf_log(LOG_SEVERITY_INFO, "[native] Heartbeat interval set to %dms", interval_ms);
}

// Native api
typedef void (*host_native_log_fn)(const char* msg, void* browser, void* frame);
typedef void (*send_javascript_code_fn)(const char* code, void* browser, void* frame);

typedef void (*send_cef_message_fn)(const char* msg, const char** args, int argCount, void* browser, void* frame, int source_process_id);
typedef void (*send_javascript_call_fn)(const char* func, const char** args, int argCount, void* browser, void* frame);
typedef const char* (*call_javascript_func_in_renderer_fn)(const char* func, const char** args, int argCount, void* browser, void* frame);
typedef const char* (*execute_javascript_in_renderer_fn)(const char* code, void* browser, void* frame);
typedef void (*free_native_string_fn)(const char* str);

typedef bool (*command_line_has_switch_fn)(void* command_line, const char* name);
typedef const char* (*command_line_get_switch_value_fn)(void* command_line, const char* name);
typedef void (*command_line_append_switch_fn)(void* command_line, const char* name);
typedef void (*command_line_append_switch_with_value_fn)(void* command_line, const char* name, const char* value);
typedef void (*command_line_remove_switch_fn)(void* command_line, const char* name);
typedef bool (*command_line_is_valid_fn)(void* command_line);
typedef bool (*command_line_is_read_only_fn)(void* command_line);
typedef bool (*command_line_has_switches_fn)(void* command_line);
typedef bool (*command_line_has_arguments_fn)(void* command_line);
typedef const char* (*command_line_get_program_fn)(void* command_line);
typedef void (*command_line_set_program_fn)(void* command_line, const char* program);
typedef const char* (*command_line_get_command_line_string_fn)(void* command_line);
typedef const char* (*command_line_get_argv_fn)(void* command_line);
typedef const char* (*command_line_get_switches_fn)(void* command_line);
typedef const char* (*command_line_get_arguments_fn)(void* command_line);
typedef void (*command_line_append_argument_fn)(void* command_line, const char* argument);
typedef void (*command_line_prepend_wrapper_fn)(void* command_line, const char* wrapper);
typedef void* (*command_line_get_global_fn)();

// Browser traversal
typedef void* (*get_browser_by_id_fn)(int browser_id);
typedef bool (*browser_is_valid_fn)(void* browser);
typedef bool (*get_renderer_browser_frame_by_id_fn)(int browser_id, void** out_browser, void** out_frame);

// Browser properties (both processes)
typedef int (*browser_get_id_fn)(void* browser);
typedef const char* (*browser_get_url_fn)(void* browser);
typedef bool (*browser_is_loading_fn)(void* browser);
typedef bool (*browser_is_popup_fn)(void* browser);
typedef bool (*browser_has_document_fn)(void* browser);

// Browser frame access (both processes)
typedef int (*browser_get_frame_count_fn)(void* browser);
typedef const char* (*browser_get_frame_identifiers_fn)(void* browser);
typedef const char* (*browser_get_frame_names_fn)(void* browser);
typedef void* (*browser_get_main_frame_fn)(void* browser);
typedef void* (*browser_get_focused_frame_fn)(void* browser);
typedef void* (*browser_get_frame_by_identifier_fn)(void* browser, const char* identifier);
typedef void* (*browser_get_frame_by_name_fn)(void* browser, const char* name);

// Browser actions (both processes)
typedef void (*browser_reload_fn)(void* browser);
typedef void (*browser_reload_ignore_cache_fn)(void* browser);
typedef void (*browser_stop_load_fn)(void* browser);

// Browser host actions (browser process only, no-op in renderer)
typedef void (*browser_close_fn)(void* browser, int force_close);
typedef void (*browser_set_focus_fn)(void* browser, int focus);
typedef int (*browser_get_opener_id_fn)(void* browser);

// DevTools host actions (browser process only, no-op in renderer)
typedef int (*browser_show_devtools_fn)(void* browser, int inspect_x, int inspect_y, int has_inspect_point);
typedef int (*browser_close_devtools_fn)(void* browser);
typedef int (*browser_has_devtools_fn)(void* browser);
typedef int (*browser_send_devtools_message_fn)(void* browser, const void* message, int size);
typedef int (*browser_execute_devtools_method_fn)(void* browser, int message_id, const char* method, const char* params_json);

// Frame properties
typedef const char* (*frame_get_url_fn)(void* frame);
typedef const char* (*frame_get_name_fn)(void* frame);
typedef const char* (*frame_get_identifier_fn)(void* frame);
typedef bool (*frame_is_main_fn)(void* frame);
typedef bool (*frame_is_valid_fn)(void* frame);
typedef bool (*frame_is_focused_fn)(void* frame);
typedef void* (*frame_get_parent_fn)(void* frame);
typedef void* (*frame_get_browser_fn)(void* frame);

// Frame actions
typedef void (*frame_load_url_fn)(void* frame, const char* url);

// CefRequest properties
typedef bool (*request_is_read_only_fn)(void* request);
typedef const char* (*request_get_url_fn)(void* request);
typedef const char* (*request_get_method_fn)(void* request);
typedef const char* (*request_get_referrer_url_fn)(void* request);
typedef int (*request_get_referrer_policy_fn)(void* request);
typedef const char* (*request_get_header_map_fn)(void* request);
typedef const char* (*request_get_header_by_name_fn)(void* request, const char* name);
typedef int (*request_get_flags_fn)(void* request);
typedef const char* (*request_get_first_party_for_cookies_fn)(void* request);
typedef int (*request_get_resource_type_fn)(void* request);
typedef int (*request_get_transition_type_fn)(void* request);
typedef uint64_t (*request_get_identifier_fn)(void* request);

// CefResponse properties (writable; native-created via CefResponse::Create()).
typedef bool (*response_is_read_only_fn)(void* response);
typedef int (*response_get_status_fn)(void* response);
typedef void (*response_set_status_fn)(void* response, int status);
typedef const char* (*response_get_status_text_fn)(void* response);
typedef void (*response_set_status_text_fn)(void* response, const char* status_text);
typedef const char* (*response_get_mime_type_fn)(void* response);
typedef void (*response_set_mime_type_fn)(void* response, const char* mime_type);
typedef const char* (*response_get_charset_fn)(void* response);
typedef void (*response_set_charset_fn)(void* response, const char* charset);
typedef const char* (*response_get_url_fn)(void* response);
typedef const char* (*response_get_header_map_fn)(void* response);
typedef const char* (*response_get_header_by_name_fn)(void* response, const char* name);
typedef void (*response_set_header_by_name_fn)(void* response, const char* name, const char* value, int overwrite);
typedef void (*response_remove_header_by_name_fn)(void* response, const char* name);
typedef void (*response_set_header_map_fn)(void* response, const char* header_map_str);

// Heartbeat control
typedef void (*set_heartbeat_interval_fn)(int interval_ms);

typedef struct {
    host_native_log_fn NativeLog;
    send_cef_message_fn SendCefMessage;
    send_javascript_code_fn SendJavascriptCode;
    send_javascript_call_fn SendJavascriptCall;
    call_javascript_func_in_renderer_fn CallJavascriptFuncInRenderer;
    execute_javascript_in_renderer_fn ExecuteJavascriptInRenderer;
    free_native_string_fn FreeNativeString;
    command_line_has_switch_fn CommandLineHasSwitch;
    command_line_get_switch_value_fn CommandLineGetSwitchValue;
    command_line_append_switch_fn CommandLineAppendSwitch;
    command_line_append_switch_with_value_fn CommandLineAppendSwitchWithValue;
    command_line_remove_switch_fn CommandLineRemoveSwitch;
    command_line_is_valid_fn CommandLineIsValid;
    command_line_is_read_only_fn CommandLineIsReadOnly;
    command_line_has_switches_fn CommandLineHasSwitches;
    command_line_has_arguments_fn CommandLineHasArguments;
    command_line_get_program_fn CommandLineGetProgram;
    command_line_set_program_fn CommandLineSetProgram;
    command_line_get_command_line_string_fn CommandLineGetCommandLineString;
    command_line_get_argv_fn CommandLineGetArgv;
    command_line_get_switches_fn CommandLineGetSwitches;
    command_line_get_arguments_fn CommandLineGetArguments;
    command_line_append_argument_fn CommandLineAppendArgument;
    command_line_prepend_wrapper_fn CommandLinePrependWrapper;
    command_line_get_global_fn CommandLineGetGlobal;
    // Browser traversal
    get_browser_by_id_fn GetBrowserById;
    browser_is_valid_fn BrowserIsValid;
    get_renderer_browser_frame_by_id_fn GetRendererBrowserFrameById;
    // Browser properties
    browser_get_id_fn BrowserGetId;
    browser_get_url_fn BrowserGetUrl;
    browser_is_loading_fn BrowserIsLoading;
    browser_is_popup_fn BrowserIsPopup;
    browser_has_document_fn BrowserHasDocument;
    // Browser frame access
    browser_get_frame_count_fn BrowserGetFrameCount;
    browser_get_frame_identifiers_fn BrowserGetFrameIdentifiers;
    browser_get_frame_names_fn BrowserGetFrameNames;
    browser_get_main_frame_fn BrowserGetMainFrame;
    browser_get_focused_frame_fn BrowserGetFocusedFrame;
    browser_get_frame_by_identifier_fn BrowserGetFrameByIdentifier;
    browser_get_frame_by_name_fn BrowserGetFrameByName;
    // Browser actions
    browser_reload_fn BrowserReload;
    browser_reload_ignore_cache_fn BrowserReloadIgnoreCache;
    browser_stop_load_fn BrowserStopLoad;
    // Browser host actions
    browser_close_fn BrowserClose;
    browser_set_focus_fn BrowserSetFocus;
    browser_get_opener_id_fn BrowserGetOpenerId;
    // DevTools host actions
    browser_show_devtools_fn BrowserShowDevTools;
    browser_close_devtools_fn BrowserCloseDevTools;
    browser_has_devtools_fn BrowserHasDevTools;
    browser_send_devtools_message_fn BrowserSendDevToolsMessage;
    browser_execute_devtools_method_fn BrowserExecuteDevToolsMethod;
    // Frame properties
    frame_get_url_fn FrameGetUrl;
    frame_get_name_fn FrameGetName;
    frame_get_identifier_fn FrameGetIdentifier;
    frame_is_main_fn FrameIsMain;
    frame_is_valid_fn FrameIsValid;
    frame_is_focused_fn FrameIsFocused;
    frame_get_parent_fn FrameGetParent;
    frame_get_browser_fn FrameGetBrowser;
    // Frame actions
    frame_load_url_fn FrameLoadUrl;
    // CefRequest properties
    request_is_read_only_fn RequestIsReadOnly;
    request_get_url_fn RequestGetUrl;
    request_get_method_fn RequestGetMethod;
    request_get_referrer_url_fn RequestGetReferrerUrl;
    request_get_referrer_policy_fn RequestGetReferrerPolicy;
    request_get_header_map_fn RequestGetHeaderMap;
    request_get_header_by_name_fn RequestGetHeaderByName;
    request_get_flags_fn RequestGetFlags;
    request_get_first_party_for_cookies_fn RequestGetFirstPartyForCookies;
    request_get_resource_type_fn RequestGetResourceType;
    request_get_transition_type_fn RequestGetTransitionType;
    request_get_identifier_fn RequestGetIdentifier;
    // CefResponse properties
    response_is_read_only_fn ResponseIsReadOnly;
    response_get_status_fn ResponseGetStatus;
    response_set_status_fn ResponseSetStatus;
    response_get_status_text_fn ResponseGetStatusText;
    response_set_status_text_fn ResponseSetStatusText;
    response_get_mime_type_fn ResponseGetMimeType;
    response_set_mime_type_fn ResponseSetMimeType;
    response_get_charset_fn ResponseGetCharset;
    response_set_charset_fn ResponseSetCharset;
    response_get_url_fn ResponseGetUrl;
    response_get_header_map_fn ResponseGetHeaderMap;
    response_get_header_by_name_fn ResponseGetHeaderByName;
    response_set_header_by_name_fn ResponseSetHeaderByName;
    response_remove_header_by_name_fn ResponseRemoveHeaderByName;
    response_set_header_map_fn ResponseSetHeaderMap;
    // Heartbeat control
    set_heartbeat_interval_fn SetHeartbeatInterval;
} HostApi;

void host_native_log(const char* msg, void* browser, void* frame)
{
    if (!msg) {
        return;
    }

    LOG(WARNING) << msg;

#if defined(_MSC_VER)
    // Convert UTF-8 to wide char for OutputDebugString
    std::wstring wmsg = Utf8ToWstring(msg);
    ::OutputDebugStringW(wmsg.c_str());
#elif defined(__APPLE__)
    // Avoid os_log which can conflict with .NET CLR signal handlers.
    fprintf(stderr, "%s\n", msg);
#elif defined(__linux__)
    syslog(LOG_WARNING, "%s", msg);
#endif
}
void send_cef_message(const char* msg_str, const char** args, int argCount, void* browser, void* frame, int source_process_id)
{
    if (!msg_str) {
        return;
    }
    auto* pBrowser = reinterpret_cast<CefBrowser*>(browser);
    auto* pFrame = reinterpret_cast<CefFrame*>(frame);
    auto msg = CefProcessMessage::Create(msg_str);

    for (int i = 0; i < argCount; i++) {
        if (args[i]) {
            msg->GetArgumentList()->SetString(i, args[i]);
        }
    }

    if (pFrame) {
        pFrame->SendProcessMessage((CefProcessId)source_process_id, msg);
    }
    else if (pBrowser) {
        pBrowser->GetMainFrame()->SendProcessMessage((CefProcessId)source_process_id, msg);
    }
}
void send_javascript_code(const char* code, void* browser, void* frame)
{
    if (!code) {
        return;
    }
    auto* pBrowser = reinterpret_cast<CefBrowser*>(browser);
    auto* pFrame = reinterpret_cast<CefFrame*>(frame);
    if (pFrame) {
        pFrame->ExecuteJavaScript(code, pFrame->GetURL(), 0);
    }
    else if (pBrowser) {
        pBrowser->GetMainFrame()->ExecuteJavaScript(code, pBrowser->GetMainFrame()->GetURL(), 0);
    }
}
void send_javascript_call(const char* func, const char** args, int argCount, void* browser, void* frame)
{
    if (!func) {
        return;
    }
    auto* pBrowser = reinterpret_cast<CefBrowser*>(browser);
    auto* pFrame = reinterpret_cast<CefFrame*>(frame);
    if (pBrowser || pFrame) {
        if (argCount == 0) {
            JavaScriptCaller::SendCall(pBrowser, pFrame, func);
        }
        else if (argCount == 1) {
            JavaScriptCaller::SendCall(pBrowser, pFrame, func, args[0]);
        }
        else {
            std::vector<std::string> argVec;
            for (int i = 0; i < argCount; i++) {
                if (args[i]) {
                    argVec.push_back(args[i]);
                }
            }
            JavaScriptCaller::SendCall(pBrowser, pFrame, func, argVec);
        }
    }
}

const char* call_javascript_func_in_renderer(const char* func, const char** args, int argCount, void* browser, void* frame)
{
    if (!func) {
        return nullptr;
    }
    auto* pBrowser = reinterpret_cast<CefBrowser*>(browser);
    auto* pFrame = reinterpret_cast<CefFrame*>(frame);
    if (!pBrowser && !pFrame) {
        return nullptr;
    }

    std::string result;
    if (argCount == 0) {
        result = JavaScriptCaller::CallInRenderer(pBrowser, pFrame, func);
    }
    else if (argCount == 1) {
        result = JavaScriptCaller::CallInRenderer(pBrowser, pFrame, func, args[0]);
    }
    else {
        std::vector<std::string> argVec;
        for (int i = 0; i < argCount; i++) {
            if (args[i]) {
                argVec.push_back(args[i]);
            }
        }
        result = JavaScriptCaller::CallInRenderer(pBrowser, pFrame, func, argVec);
    }

    // Allocate memory for result string (caller must free it)
    if (result.empty()) {
        return nullptr;
    }
    char* result_str = new char[result.length() + 1];
    strcpy(result_str, result.c_str());
    return result_str;
}

const char* execute_javascript_in_renderer(const char* code, void* browser, void* frame)
{
    if (!code) {
        return nullptr;
    }
    auto* pBrowser = reinterpret_cast<CefBrowser*>(browser);
    auto* pFrame = reinterpret_cast<CefFrame*>(frame);
    if (!pBrowser && !pFrame) {
        return nullptr;
    }

    std::string result = JavaScriptCaller::ExecuteInRenderer(pBrowser, pFrame, code);

    // Allocate memory for result string (caller must free it)
    if (result.empty()) {
        return nullptr;
    }
    char* result_str = new char[result.length() + 1];
    strcpy(result_str, result.c_str());
    return result_str;
}

void free_native_string(const char* str)
{
    if (str) {
        delete[] str;
    }
}

bool command_line_has_switch(void* command_line, const char* name)
{
    if (!command_line || !name) {
        return false;
    }
    auto* pCommandLine = reinterpret_cast<CefCommandLine*>(command_line);
    return pCommandLine->HasSwitch(name);
}

const char* command_line_get_switch_value(void* command_line, const char* name)
{
    if (!command_line || !name) {
        return nullptr;
    }
    auto* pCommandLine = reinterpret_cast<CefCommandLine*>(command_line);
    CefString value = pCommandLine->GetSwitchValue(name);
    if (value.empty()) {
        return nullptr;
    }
    // Allocate memory for result string (caller must free it using FreeNativeString)
    std::string utf8_value = value.ToString();
    char* result_str = new char[utf8_value.length() + 1];
    strcpy(result_str, utf8_value.c_str());
    return result_str;
}

void command_line_append_switch(void* command_line, const char* name)
{
    if (!command_line || !name) {
        return;
    }
    auto* pCommandLine = reinterpret_cast<CefCommandLine*>(command_line);
    pCommandLine->AppendSwitch(name);
}

void command_line_append_switch_with_value(void* command_line, const char* name, const char* value)
{
    if (!command_line || !name) {
        return;
    }
    auto* pCommandLine = reinterpret_cast<CefCommandLine*>(command_line);
    pCommandLine->AppendSwitchWithValue(name, value ? value : "");
}

void command_line_remove_switch(void* command_line, const char* name)
{
    if (!command_line || !name) {
        return;
    }
    auto* pCommandLine = reinterpret_cast<CefCommandLine*>(command_line);
    pCommandLine->RemoveSwitch(name);
}

bool command_line_is_valid(void* command_line)
{
    if (!command_line) return false;
    return reinterpret_cast<CefCommandLine*>(command_line)->IsValid();
}

bool command_line_is_read_only(void* command_line)
{
    if (!command_line) return false;
    return reinterpret_cast<CefCommandLine*>(command_line)->IsReadOnly();
}

bool command_line_has_switches(void* command_line)
{
    if (!command_line) return false;
    return reinterpret_cast<CefCommandLine*>(command_line)->HasSwitches();
}

bool command_line_has_arguments(void* command_line)
{
    if (!command_line) return false;
    return reinterpret_cast<CefCommandLine*>(command_line)->HasArguments();
}

// Forward declaration
static char* alloc_string(const std::string& s);

const char* command_line_get_program(void* command_line)
{
    if (!command_line) return nullptr;
    auto* pCommandLine = reinterpret_cast<CefCommandLine*>(command_line);
    CefString program = pCommandLine->GetProgram();
    if (program.empty()) return nullptr;
    return alloc_string(program.ToString());
}

void command_line_set_program(void* command_line, const char* program)
{
    if (!command_line || !program) return;
    reinterpret_cast<CefCommandLine*>(command_line)->SetProgram(program);
}

const char* command_line_get_command_line_string(void* command_line)
{
    if (!command_line) return nullptr;
    auto* pCommandLine = reinterpret_cast<CefCommandLine*>(command_line);
    CefString str = pCommandLine->GetCommandLineString();
    if (str.empty()) return nullptr;
    return alloc_string(str.ToString());
}

const char* command_line_get_argv(void* command_line)
{
    if (!command_line) return nullptr;
    auto* pCommandLine = reinterpret_cast<CefCommandLine*>(command_line);
    std::vector<CefString> argv;
    pCommandLine->GetArgv(argv);
    if (argv.empty()) return nullptr;
    std::string result;
    for (const auto& arg : argv) {
        if (!result.empty()) result += "\n";
        result += arg.ToString();
    }
    return alloc_string(result);
}

const char* command_line_get_switches(void* command_line)
{
    if (!command_line) return nullptr;
    auto* pCommandLine = reinterpret_cast<CefCommandLine*>(command_line);
    CefCommandLine::SwitchMap switches;
    pCommandLine->GetSwitches(switches);
    if (switches.empty()) return nullptr;
    std::string result;
    for (const auto& pair : switches) {
        if (!result.empty()) result += "\n";
        result += pair.first.ToString() + "=" + pair.second.ToString();
    }
    return alloc_string(result);
}

const char* command_line_get_arguments(void* command_line)
{
    if (!command_line) return nullptr;
    auto* pCommandLine = reinterpret_cast<CefCommandLine*>(command_line);
    CefCommandLine::ArgumentList arguments;
    pCommandLine->GetArguments(arguments);
    if (arguments.empty()) return nullptr;
    std::string result;
    for (const auto& arg : arguments) {
        if (!result.empty()) result += "\n";
        result += arg.ToString();
    }
    return alloc_string(result);
}

void command_line_append_argument(void* command_line, const char* argument)
{
    if (!command_line || !argument) return;
    reinterpret_cast<CefCommandLine*>(command_line)->AppendArgument(argument);
}

void command_line_prepend_wrapper(void* command_line, const char* wrapper)
{
    if (!command_line || !wrapper) return;
    reinterpret_cast<CefCommandLine*>(command_line)->PrependWrapper(wrapper);
}

void* command_line_get_global()
{
    return CefCommandLine::GetGlobalCommandLine().get();
}

// Returns true if running in browser process (lazily initialized on first call)
static bool is_browser_process()
{
    static std::once_flag s_flag;
    static bool s_result = false;
    std::call_once(s_flag, []() {
        auto cmd_line = CefCommandLine::GetGlobalCommandLine();
        if (cmd_line) {
            // Browser process has no --type switch; all sub-processes have one
            s_result = cmd_line->GetSwitchValue("type").ToString().empty();
        }
    });
    return s_result;
}

// Helper: allocate a copy of std::string as char* (caller must free with delete[])
static char* alloc_string(const std::string& s)
{
    char* p = new char[s.length() + 1];
    strcpy(p, s.c_str());
    return p;
}

// --- Browser traversal ---

// Check whether a browser raw pointer is currently registered as alive in
// the current process's valid-set. Never dereferences the pointer, so it is
// safe to call with a possibly-stale pointer from C#.
bool browser_is_valid(void* browser)
{
    if (!browser) return false;
    auto* raw = reinterpret_cast<CefBrowser*>(browser);
    std::lock_guard<std::mutex> lock(GetRefContainersMutex());
    if (is_browser_process()) {
        return GetBrowserValidSet().count(raw) > 0;
    } else {
        return GetRendererValidSet().count(raw) > 0;
    }
}

bool get_renderer_browser_frame_by_id(int browser_id, void** out_browser, void** out_frame)
{
    if (!out_browser || !out_frame) return false;
    *out_browser = nullptr;
    *out_frame = nullptr;
    CefRefPtr<CefBrowser> b;
    CefRefPtr<CefFrame> f;
    {
        std::lock_guard<std::mutex> lock(GetRefContainersMutex());
        auto& m = GetRendererRefMap();
        auto it = m.find(browser_id);
        if (it == m.end()) return false;
        b = it->second.first;
        f = it->second.second;
    }
    if (!b) return false;
    *out_browser = b.get();
    *out_frame = f ? f.get() : nullptr;
    return true;
}

// --- Renderer ref map: hold CefRefPtr to prevent premature release ---
// Called from renderer process only, driven by main-frame OnContextCreated/
// OnContextReleased in client_renderer.cc.

void renderer_ref_add(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame)
{
    if (!browser) return;
    int id = browser->GetIdentifier();
    CefBrowser* raw = browser.get();
    std::lock_guard<std::mutex> lock(GetRefContainersMutex());
    GetRendererRefMap()[id] = std::make_pair(browser, frame);
    GetRendererValidSet().insert(raw);
}

void renderer_ref_remove(CefRefPtr<CefBrowser> browser)
{
    if (!browser) return;
    int id = browser->GetIdentifier();
    CefBrowser* raw = browser.get();
    // Release outside the lock: destroying CefRefPtr may trigger CEF internals
    // that could otherwise deadlock while we hold the map mutex.
    std::pair<CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>> to_release;
    {
        std::lock_guard<std::mutex> lock(GetRefContainersMutex());
        GetRendererValidSet().erase(raw);
        auto& m = GetRendererRefMap();
        auto it = m.find(id);
        if (it != m.end()) {
            to_release = std::move(it->second);
            m.erase(it);
        }
    }
    // to_release destructor runs here, outside the lock (browser + frame refs)
    (void)to_release;
}

// --- Browser ref map (browser process): hold CefRefPtr for the browser's ---
// entire lifetime so C# raw pointers remain valid between OnAfterCreated and
// OnBeforeClose.

void browser_ref_add(CefRefPtr<CefBrowser> browser)
{
    if (!browser) return;
    int id = browser->GetIdentifier();
    CefBrowser* raw = browser.get();
    // Grab main frame once at OnAfterCreated. May be null in edge cases; that
    // is tolerated (only browser ref is required for correctness, frame ref is
    // a safety net for future accessors).
    CefRefPtr<CefFrame> mf = browser->GetMainFrame();
    std::lock_guard<std::mutex> lock(GetRefContainersMutex());
    GetBrowserRefMap()[id] = std::make_pair(browser, mf);
    GetBrowserValidSet().insert(raw);
}

void browser_ref_remove(CefRefPtr<CefBrowser> browser)
{
    if (!browser) return;
    int id = browser->GetIdentifier();
    CefBrowser* raw = browser.get();
    // Release outside the lock (browser + frame refs together).
    std::pair<CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>> to_release;
    {
        std::lock_guard<std::mutex> lock(GetRefContainersMutex());
        GetBrowserValidSet().erase(raw);
        auto& m = GetBrowserRefMap();
        auto it = m.find(id);
        if (it != m.end()) {
            to_release = std::move(it->second);
            m.erase(it);
        }
    }
    (void)to_release;
}

// Called from CefFrameHandler::OnMainFrameChanged on the UI thread to keep the
// stored main-frame ref in sync with cross-origin navigations, renderer crash
// recovery, and initial/final main-frame lifecycle events. If the map entry
// does not exist yet (OnMainFrameChanged can fire before OnAfterCreated on
// initial creation), we silently skip; browser_ref_add will capture the
// current main frame when the entry is created. |frame| may be null (final
// main-frame destruction just before OnBeforeClose) -- that's fine, the entry
// will be erased shortly by browser_ref_remove.
void browser_ref_update_frame(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame)
{
    if (!browser) return;
    int id = browser->GetIdentifier();
    // Release the previous frame ref outside the lock.
    CefRefPtr<CefFrame> to_release;
    {
        std::lock_guard<std::mutex> lock(GetRefContainersMutex());
        auto& m = GetBrowserRefMap();
        auto it = m.find(id);
        if (it == m.end()) return;
        to_release = std::move(it->second.second);
        it->second.second = frame;
    }
    (void)to_release;
}

void* get_browser_by_id(int browser_id)
{
    if (!is_browser_process()) return nullptr;
    std::lock_guard<std::mutex> lock(GetRefContainersMutex());
    auto& m = GetBrowserRefMap();
    auto it = m.find(browser_id);
    if (it == m.end()) return nullptr;
    return it->second.first.get();
}

// --- Browser properties ---

int browser_get_id(void* browser)
{
    if (!browser_is_valid(browser)) return 0;
    return reinterpret_cast<CefBrowser*>(browser)->GetIdentifier();
}

const char* browser_get_url(void* browser)
{
    if (!browser_is_valid(browser)) return nullptr;
    auto* pBrowser = reinterpret_cast<CefBrowser*>(browser);
    auto frame = pBrowser->GetMainFrame();
    if (!frame) return nullptr;
    std::string url = frame->GetURL().ToString();
    if (url.empty()) return nullptr;
    return alloc_string(url);
}

bool browser_is_loading(void* browser)
{
    if (!browser_is_valid(browser)) return false;
    return reinterpret_cast<CefBrowser*>(browser)->IsLoading();
}

bool browser_is_popup(void* browser)
{
    if (!browser_is_valid(browser)) return false;
    return reinterpret_cast<CefBrowser*>(browser)->IsPopup();
}

bool browser_has_document(void* browser)
{
    if (!browser_is_valid(browser)) return false;
    return reinterpret_cast<CefBrowser*>(browser)->HasDocument();
}

// --- Browser frame access ---

int browser_get_frame_count(void* browser)
{
    if (!browser_is_valid(browser)) return 0;
    return static_cast<int>(reinterpret_cast<CefBrowser*>(browser)->GetFrameCount());
}

const char* browser_get_frame_identifiers(void* browser)
{
    if (!browser_is_valid(browser)) return nullptr;
    auto* pBrowser = reinterpret_cast<CefBrowser*>(browser);
    std::vector<CefString> identifiers;
    pBrowser->GetFrameIdentifiers(identifiers);
    std::string result;
    for (auto& id : identifiers) {
        if (!result.empty()) result += "\n";
        result += id.ToString();
    }
    if (result.empty()) return nullptr;
    return alloc_string(result);
}

const char* browser_get_frame_names(void* browser)
{
    if (!browser_is_valid(browser)) return nullptr;
    auto* pBrowser = reinterpret_cast<CefBrowser*>(browser);
    std::vector<CefString> names;
    pBrowser->GetFrameNames(names);
    std::string result;
    for (auto& name : names) {
        if (!result.empty()) result += "\n";
        result += name.ToString();
    }
    if (result.empty()) return nullptr;
    return alloc_string(result);
}

void* browser_get_main_frame(void* browser)
{
    if (!browser_is_valid(browser)) return nullptr;
    return reinterpret_cast<CefBrowser*>(browser)->GetMainFrame().get();
}

void* browser_get_focused_frame(void* browser)
{
    if (!browser_is_valid(browser)) return nullptr;
    return reinterpret_cast<CefBrowser*>(browser)->GetFocusedFrame().get();
}

void* browser_get_frame_by_identifier(void* browser, const char* identifier)
{
    if (!browser_is_valid(browser) || !identifier) return nullptr;
    return reinterpret_cast<CefBrowser*>(browser)->GetFrameByIdentifier(identifier).get();
}

void* browser_get_frame_by_name(void* browser, const char* name)
{
    if (!browser_is_valid(browser) || !name) return nullptr;
    return reinterpret_cast<CefBrowser*>(browser)->GetFrameByName(name).get();
}

// --- Browser actions ---

void browser_reload(void* browser)
{
    if (!browser_is_valid(browser)) return;
    reinterpret_cast<CefBrowser*>(browser)->Reload();
}

void browser_reload_ignore_cache(void* browser)
{
    if (!browser_is_valid(browser)) return;
    reinterpret_cast<CefBrowser*>(browser)->ReloadIgnoreCache();
}

void browser_stop_load(void* browser)
{
    if (!browser_is_valid(browser)) return;
    reinterpret_cast<CefBrowser*>(browser)->StopLoad();
}

// --- Browser host actions (browser process only) ---

void browser_close(void* browser, int force_close)
{
    if (!browser_is_valid(browser) || !is_browser_process()) return;
    auto host = reinterpret_cast<CefBrowser*>(browser)->GetHost();
    if (host) {
        host->CloseBrowser(force_close != 0);
    }
}

void browser_set_focus(void* browser, int focus)
{
    if (!browser_is_valid(browser) || !is_browser_process()) return;
    auto host = reinterpret_cast<CefBrowser*>(browser)->GetHost();
    if (host) {
        host->SetFocus(focus != 0);
    }
}

int browser_get_opener_id(void* browser)
{
    if (!browser_is_valid(browser) || !is_browser_process()) return 0;
    auto host = reinterpret_cast<CefBrowser*>(browser)->GetHost();
    if (host) {
        return host->GetOpenerIdentifier();
    }
    return 0;
}

// --- DevTools host actions (browser process only) ---

int browser_show_devtools(void* browser, int inspect_x, int inspect_y, int has_inspect_point)
{
    if (!browser_is_valid(browser) || !is_browser_process()) return 0;
    auto host = reinterpret_cast<CefBrowser*>(browser)->GetHost();
    if (!host) return 0;
    CefWindowInfo window_info;
    CefBrowserSettings settings;
    CefPoint inspect_at = has_inspect_point ? CefPoint(inspect_x, inspect_y) : CefPoint();
    host->ShowDevTools(window_info, nullptr, settings, inspect_at);
    return 1;
}

int browser_close_devtools(void* browser)
{
    if (!browser_is_valid(browser) || !is_browser_process()) return 0;
    auto host = reinterpret_cast<CefBrowser*>(browser)->GetHost();
    if (!host) return 0;
    host->CloseDevTools();
    return 1;
}

int browser_has_devtools(void* browser)
{
    if (!browser_is_valid(browser) || !is_browser_process()) return 0;
    auto host = reinterpret_cast<CefBrowser*>(browser)->GetHost();
    if (!host) return 0;
    return host->HasDevTools() ? 1 : 0;
}

int browser_send_devtools_message(void* browser, const void* message, int size)
{
    if (!browser_is_valid(browser) || !is_browser_process() || !message || size <= 0) return 0;
    auto host = reinterpret_cast<CefBrowser*>(browser)->GetHost();
    if (!host) return 0;
    return host->SendDevToolsMessage(message, static_cast<size_t>(size)) ? 1 : 0;
}

int browser_execute_devtools_method(void* browser, int message_id, const char* method, const char* params_json)
{
    if (!browser_is_valid(browser) || !is_browser_process() || !method) return 0;
    auto host = reinterpret_cast<CefBrowser*>(browser)->GetHost();
    if (!host) return 0;
    CefRefPtr<CefDictionaryValue> params;
    if (params_json && params_json[0] != '\0') {
        CefRefPtr<CefValue> value = CefParseJSON(CefString(params_json), JSON_PARSER_ALLOW_TRAILING_COMMAS);
        if (value && value->GetType() == VTYPE_DICTIONARY) {
            params = value->GetDictionary();
        }
    }
    return host->ExecuteDevToolsMethod(message_id, CefString(method), params);
}

// --- DevTools observer bridge ---

class HostDevToolsObserver : public CefDevToolsMessageObserver {
 public:
  HostDevToolsObserver() = default;

  bool OnDevToolsMessage(CefRefPtr<CefBrowser> browser,
                         const void* message,
                         size_t message_size) override {
    if (on_devtools_message_fptr) {
      return on_devtools_message_fptr(browser.get(), message,
                                      static_cast<int>(message_size)) != 0;
    }
    return false;
  }

  void OnDevToolsMethodResult(CefRefPtr<CefBrowser> browser,
                              int message_id,
                              bool success,
                              const void* result,
                              size_t result_size) override {
    if (on_devtools_method_result_fptr) {
      on_devtools_method_result_fptr(browser.get(), message_id,
                                     success ? 1 : 0, result,
                                     static_cast<int>(result_size));
    }
  }

  void OnDevToolsEvent(CefRefPtr<CefBrowser> browser,
                       const CefString& method,
                       const void* params,
                       size_t params_size) override {
    if (on_devtools_event_fptr) {
      std::string method_str = method.ToString();
      on_devtools_event_fptr(browser.get(), method_str.c_str(),
                             params, static_cast<int>(params_size));
    }
  }

  void OnDevToolsAgentAttached(CefRefPtr<CefBrowser> browser) override {
    if (on_devtools_agent_attached_fptr) {
      on_devtools_agent_attached_fptr(browser.get());
    }
  }

  void OnDevToolsAgentDetached(CefRefPtr<CefBrowser> browser) override {
    if (on_devtools_agent_detached_fptr) {
      on_devtools_agent_detached_fptr(browser.get());
    }
  }

 private:
  IMPLEMENT_REFCOUNTING(HostDevToolsObserver);
};

// Map browser_id -> registration handle. All access on UI thread; mutex is
// defensive against unexpected callers.
// Use Meyers local-static + heap allocation so these are never destroyed
// during static teardown (avoids releasing CefRegistration / locking a
// destroyed mutex after CEF has shut down).
static std::map<int, CefRefPtr<CefRegistration>>& GetDevToolsRegistrations() {
    static auto* s_map = new std::map<int, CefRefPtr<CefRegistration>>();
    return *s_map;
}

static std::mutex& GetDevToolsRegMutex() {
    static auto* s_mutex = new std::mutex();
    return *s_mutex;
}

void RegisterDevToolsObserver(CefBrowser* browser)
{
    if (!browser) return;
    auto host = browser->GetHost();
    if (!host) return;
    CefRefPtr<HostDevToolsObserver> observer = new HostDevToolsObserver();
    CefRefPtr<CefRegistration> registration =
        host->AddDevToolsMessageObserver(observer);
    if (!registration) return;
    std::lock_guard<std::mutex> lock(GetDevToolsRegMutex());
    GetDevToolsRegistrations()[browser->GetIdentifier()] = registration;
}

void UnregisterDevToolsObserver(CefBrowser* browser)
{
    if (!browser) return;
    std::lock_guard<std::mutex> lock(GetDevToolsRegMutex());
    GetDevToolsRegistrations().erase(browser->GetIdentifier());
}

// --- Frame properties ---

const char* frame_get_url(void* frame)
{
    if (!frame) return nullptr;
    std::string url = reinterpret_cast<CefFrame*>(frame)->GetURL().ToString();
    if (url.empty()) return nullptr;
    return alloc_string(url);
}

const char* frame_get_name(void* frame)
{
    if (!frame) return nullptr;
    std::string name = reinterpret_cast<CefFrame*>(frame)->GetName().ToString();
    if (name.empty()) return nullptr;
    return alloc_string(name);
}

const char* frame_get_identifier(void* frame)
{
    if (!frame) return nullptr;
    std::string id = reinterpret_cast<CefFrame*>(frame)->GetIdentifier().ToString();
    if (id.empty()) return nullptr;
    return alloc_string(id);
}

bool frame_is_main(void* frame)
{
    if (!frame) return false;
    return reinterpret_cast<CefFrame*>(frame)->IsMain();
}

bool frame_is_valid(void* frame)
{
    if (!frame) return false;
    return reinterpret_cast<CefFrame*>(frame)->IsValid();
}

bool frame_is_focused(void* frame)
{
    if (!frame) return false;
    return reinterpret_cast<CefFrame*>(frame)->IsFocused();
}

void* frame_get_parent(void* frame)
{
    if (!frame) return nullptr;
    return reinterpret_cast<CefFrame*>(frame)->GetParent().get();
}

void* frame_get_browser(void* frame)
{
    if (!frame) return nullptr;
    return reinterpret_cast<CefFrame*>(frame)->GetBrowser().get();
}

// --- Frame actions ---

void frame_load_url(void* frame, const char* url)
{
    if (!frame || !url) return;
    reinterpret_cast<CefFrame*>(frame)->LoadURL(url);
}

// --- CefRequest properties ---

bool request_is_read_only(void* request)
{
    if (!request) return true;
    return reinterpret_cast<CefRequest*>(request)->IsReadOnly();
}

const char* request_get_url(void* request)
{
    if (!request) return nullptr;
    std::string url = reinterpret_cast<CefRequest*>(request)->GetURL().ToString();
    if (url.empty()) return nullptr;
    return alloc_string(url);
}

const char* request_get_method(void* request)
{
    if (!request) return nullptr;
    std::string method = reinterpret_cast<CefRequest*>(request)->GetMethod().ToString();
    if (method.empty()) return nullptr;
    return alloc_string(method);
}

const char* request_get_referrer_url(void* request)
{
    if (!request) return nullptr;
    std::string url = reinterpret_cast<CefRequest*>(request)->GetReferrerURL().ToString();
    if (url.empty()) return nullptr;
    return alloc_string(url);
}

int request_get_referrer_policy(void* request)
{
    if (!request) return 0;
    return static_cast<int>(reinterpret_cast<CefRequest*>(request)->GetReferrerPolicy());
}

const char* request_get_header_map(void* request)
{
    if (!request) return nullptr;
    CefRequest::HeaderMap headerMap;
    reinterpret_cast<CefRequest*>(request)->GetHeaderMap(headerMap);
    if (headerMap.empty()) return nullptr;
    std::string result;
    for (const auto& pair : headerMap) {
        if (!result.empty()) result += "\n";
        result += pair.first.ToString() + ":" + pair.second.ToString();
    }
    return alloc_string(result);
}

const char* request_get_header_by_name(void* request, const char* name)
{
    if (!request || !name) return nullptr;
    std::string value = reinterpret_cast<CefRequest*>(request)->GetHeaderByName(name).ToString();
    if (value.empty()) return nullptr;
    return alloc_string(value);
}

int request_get_flags(void* request)
{
    if (!request) return 0;
    return reinterpret_cast<CefRequest*>(request)->GetFlags();
}

const char* request_get_first_party_for_cookies(void* request)
{
    if (!request) return nullptr;
    std::string url = reinterpret_cast<CefRequest*>(request)->GetFirstPartyForCookies().ToString();
    if (url.empty()) return nullptr;
    return alloc_string(url);
}

int request_get_resource_type(void* request)
{
    if (!request) return 0;
    return static_cast<int>(reinterpret_cast<CefRequest*>(request)->GetResourceType());
}

int request_get_transition_type(void* request)
{
    if (!request) return 0;
    return static_cast<int>(reinterpret_cast<CefRequest*>(request)->GetTransitionType());
}

uint64_t request_get_identifier(void* request)
{
    if (!request) return 0;
    return reinterpret_cast<CefRequest*>(request)->GetIdentifier();
}

// --- CefResponse properties ---

bool response_is_read_only(void* response)
{
    if (!response) return true;
    return reinterpret_cast<CefResponse*>(response)->IsReadOnly();
}

int response_get_status(void* response)
{
    if (!response) return 0;
    return reinterpret_cast<CefResponse*>(response)->GetStatus();
}

void response_set_status(void* response, int status)
{
    if (!response) return;
    reinterpret_cast<CefResponse*>(response)->SetStatus(status);
}

const char* response_get_status_text(void* response)
{
    if (!response) return nullptr;
    std::string text = reinterpret_cast<CefResponse*>(response)->GetStatusText().ToString();
    if (text.empty()) return nullptr;
    return alloc_string(text);
}

void response_set_status_text(void* response, const char* status_text)
{
    if (!response) return;
    reinterpret_cast<CefResponse*>(response)->SetStatusText(status_text ? CefString(status_text) : CefString());
}

const char* response_get_mime_type(void* response)
{
    if (!response) return nullptr;
    std::string mime = reinterpret_cast<CefResponse*>(response)->GetMimeType().ToString();
    if (mime.empty()) return nullptr;
    return alloc_string(mime);
}

void response_set_mime_type(void* response, const char* mime_type)
{
    if (!response) return;
    reinterpret_cast<CefResponse*>(response)->SetMimeType(mime_type ? CefString(mime_type) : CefString());
}

const char* response_get_charset(void* response)
{
    if (!response) return nullptr;
    std::string charset = reinterpret_cast<CefResponse*>(response)->GetCharset().ToString();
    if (charset.empty()) return nullptr;
    return alloc_string(charset);
}

void response_set_charset(void* response, const char* charset)
{
    if (!response) return;
    reinterpret_cast<CefResponse*>(response)->SetCharset(charset ? CefString(charset) : CefString());
}

const char* response_get_url(void* response)
{
    if (!response) return nullptr;
    std::string url = reinterpret_cast<CefResponse*>(response)->GetURL().ToString();
    if (url.empty()) return nullptr;
    return alloc_string(url);
}

const char* response_get_header_map(void* response)
{
    if (!response) return nullptr;
    CefResponse::HeaderMap headerMap;
    reinterpret_cast<CefResponse*>(response)->GetHeaderMap(headerMap);
    if (headerMap.empty()) return nullptr;
    std::string result;
    for (const auto& pair : headerMap) {
        if (!result.empty()) result += "\n";
        result += pair.first.ToString() + ":" + pair.second.ToString();
    }
    return alloc_string(result);
}

const char* response_get_header_by_name(void* response, const char* name)
{
    if (!response || !name) return nullptr;
    std::string value = reinterpret_cast<CefResponse*>(response)->GetHeaderByName(name).ToString();
    if (value.empty()) return nullptr;
    return alloc_string(value);
}

void response_set_header_by_name(void* response, const char* name, const char* value, int overwrite)
{
    if (!response || !name) return;
    reinterpret_cast<CefResponse*>(response)->SetHeaderByName(
        name, value ? CefString(value) : CefString(), overwrite != 0);
}

void response_remove_header_by_name(void* response, const char* name)
{
    if (!response || !name) return;
    // CefResponse has no RemoveHeaderByName; emulate via full-map replace.
    CefResponse::HeaderMap headerMap;
    auto* resp = reinterpret_cast<CefResponse*>(response);
    resp->GetHeaderMap(headerMap);
    std::string nameLower = name;
    for (auto& c : nameLower) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    for (auto it = headerMap.begin(); it != headerMap.end();) {
        std::string keyLower = it->first.ToString();
        for (auto& c : keyLower) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        if (keyLower == nameLower) {
            it = headerMap.erase(it);
        } else {
            ++it;
        }
    }
    resp->SetHeaderMap(headerMap);
}

void response_set_header_map(void* response, const char* header_map_str)
{
    if (!response) return;
    CefResponse::HeaderMap headerMap;
    if (header_map_str && header_map_str[0] != '\0') {
        // Parse "name:value\nname:value\n..." (same format as response_get_header_map).
        std::string input = header_map_str;
        size_t pos = 0;
        while (pos < input.size()) {
            size_t nl = input.find('\n', pos);
            std::string line = (nl == std::string::npos) ? input.substr(pos) : input.substr(pos, nl - pos);
            pos = (nl == std::string::npos) ? input.size() : nl + 1;
            size_t colon = line.find(':');
            if (colon == std::string::npos) continue;
            headerMap.insert(std::make_pair(
                CefString(line.substr(0, colon)),
                CefString(line.substr(colon + 1))));
        }
    }
    reinterpret_cast<CefResponse*>(response)->SetHeaderMap(headerMap);
}

// Function to call .NET Core method
int load_dotnet_method(bool is_debug, int& rc)
{
    string_t dotnet_assembly_path = BuildAssemblyPath(is_debug);
    const char_t* dotnet_class_name = c_dotnet_class_name;
    // native api
    HostApi api = {};
    api.NativeLog = &host_native_log;
    api.SendCefMessage = &send_cef_message;
    api.SendJavascriptCode = &send_javascript_code;
    api.SendJavascriptCall = &send_javascript_call;
    api.CallJavascriptFuncInRenderer = &call_javascript_func_in_renderer;
    api.ExecuteJavascriptInRenderer = &execute_javascript_in_renderer;
    api.FreeNativeString = &free_native_string;
    api.CommandLineHasSwitch = &command_line_has_switch;
    api.CommandLineGetSwitchValue = &command_line_get_switch_value;
    api.CommandLineAppendSwitch = &command_line_append_switch;
    api.CommandLineAppendSwitchWithValue = &command_line_append_switch_with_value;
    api.CommandLineRemoveSwitch = &command_line_remove_switch;
    api.CommandLineIsValid = &command_line_is_valid;
    api.CommandLineIsReadOnly = &command_line_is_read_only;
    api.CommandLineHasSwitches = &command_line_has_switches;
    api.CommandLineHasArguments = &command_line_has_arguments;
    api.CommandLineGetProgram = &command_line_get_program;
    api.CommandLineSetProgram = &command_line_set_program;
    api.CommandLineGetCommandLineString = &command_line_get_command_line_string;
    api.CommandLineGetArgv = &command_line_get_argv;
    api.CommandLineGetSwitches = &command_line_get_switches;
    api.CommandLineGetArguments = &command_line_get_arguments;
    api.CommandLineAppendArgument = &command_line_append_argument;
    api.CommandLinePrependWrapper = &command_line_prepend_wrapper;
    api.CommandLineGetGlobal = &command_line_get_global;
    api.GetBrowserById = &get_browser_by_id;
    api.BrowserIsValid = &browser_is_valid;
    api.GetRendererBrowserFrameById = &get_renderer_browser_frame_by_id;
    api.BrowserGetId = &browser_get_id;
    api.BrowserGetUrl = &browser_get_url;
    api.BrowserIsLoading = &browser_is_loading;
    api.BrowserIsPopup = &browser_is_popup;
    api.BrowserHasDocument = &browser_has_document;
    api.BrowserGetFrameCount = &browser_get_frame_count;
    api.BrowserGetFrameIdentifiers = &browser_get_frame_identifiers;
    api.BrowserGetFrameNames = &browser_get_frame_names;
    api.BrowserGetMainFrame = &browser_get_main_frame;
    api.BrowserGetFocusedFrame = &browser_get_focused_frame;
    api.BrowserGetFrameByIdentifier = &browser_get_frame_by_identifier;
    api.BrowserGetFrameByName = &browser_get_frame_by_name;
    api.BrowserReload = &browser_reload;
    api.BrowserReloadIgnoreCache = &browser_reload_ignore_cache;
    api.BrowserStopLoad = &browser_stop_load;
    api.BrowserClose = &browser_close;
    api.BrowserSetFocus = &browser_set_focus;
    api.BrowserGetOpenerId = &browser_get_opener_id;
    api.BrowserShowDevTools = &browser_show_devtools;
    api.BrowserCloseDevTools = &browser_close_devtools;
    api.BrowserHasDevTools = &browser_has_devtools;
    api.BrowserSendDevToolsMessage = &browser_send_devtools_message;
    api.BrowserExecuteDevToolsMethod = &browser_execute_devtools_method;
    api.FrameGetUrl = &frame_get_url;
    api.FrameGetName = &frame_get_name;
    api.FrameGetIdentifier = &frame_get_identifier;
    api.FrameIsMain = &frame_is_main;
    api.FrameIsValid = &frame_is_valid;
    api.FrameIsFocused = &frame_is_focused;
    api.FrameGetParent = &frame_get_parent;
    api.FrameGetBrowser = &frame_get_browser;
    api.FrameLoadUrl = &frame_load_url;
    api.RequestIsReadOnly = &request_is_read_only;
    api.RequestGetUrl = &request_get_url;
    api.RequestGetMethod = &request_get_method;
    api.RequestGetReferrerUrl = &request_get_referrer_url;
    api.RequestGetReferrerPolicy = &request_get_referrer_policy;
    api.RequestGetHeaderMap = &request_get_header_map;
    api.RequestGetHeaderByName = &request_get_header_by_name;
    api.RequestGetFlags = &request_get_flags;
    api.RequestGetFirstPartyForCookies = &request_get_first_party_for_cookies;
    api.RequestGetResourceType = &request_get_resource_type;
    api.RequestGetTransitionType = &request_get_transition_type;
    api.RequestGetIdentifier = &request_get_identifier;
    // CefResponse properties
    api.ResponseIsReadOnly = &response_is_read_only;
    api.ResponseGetStatus = &response_get_status;
    api.ResponseSetStatus = &response_set_status;
    api.ResponseGetStatusText = &response_get_status_text;
    api.ResponseSetStatusText = &response_set_status_text;
    api.ResponseGetMimeType = &response_get_mime_type;
    api.ResponseSetMimeType = &response_set_mime_type;
    api.ResponseGetCharset = &response_get_charset;
    api.ResponseSetCharset = &response_set_charset;
    api.ResponseGetUrl = &response_get_url;
    api.ResponseGetHeaderMap = &response_get_header_map;
    api.ResponseGetHeaderByName = &response_get_header_by_name;
    api.ResponseSetHeaderByName = &response_set_header_by_name;
    api.ResponseRemoveHeaderByName = &response_remove_header_by_name;
    api.ResponseSetHeaderMap = &response_set_header_map;
    // Heartbeat control
    api.SetHeartbeatInterval = &SetHeartbeatIntervalMs;

    // For UNMANAGEDCALLERSONLY_METHOD, this must be int (or other directly copyable type), not bool.
    typedef int (CORECLR_DELEGATE_CALLTYPE* register_api_fn)(void* arg);
    register_api_fn register_api = nullptr;
    rc = load_assembly_and_get_function_pointer(
        dotnet_assembly_path.c_str(),
        dotnet_class_name,
        CHAR_T_LITERAL("RegisterApi"),
        UNMANAGEDCALLERSONLY_METHOD,
        nullptr,
        (void**)&register_api);
    if (rc || !register_api) {
        printf_log(LOG_SEVERITY_ERROR, "Failure: load register_api");
    }

    if (register_api) {
        int result = register_api(&api);
        printf_log(LOG_SEVERITY_INFO, "register_api returned: %d", result);
    }

    // dotnet methods
    rc = load_assembly_and_get_function_pointer(
    dotnet_assembly_path.c_str(),
    dotnet_class_name,
    CHAR_T_LITERAL("OnInit"),
    CHAR_T_LITERAL("DotNetLib.Lib+OnInitDelegation, CefDotnetApp"), // Delegate type
    nullptr,
    (void**)&on_init_fptr);
    if (rc || !on_init_fptr) {
        printf_log(LOG_SEVERITY_ERROR, "Failure: load on_init");
    }

    rc = load_assembly_and_get_function_pointer(
    dotnet_assembly_path.c_str(),
    dotnet_class_name,
    CHAR_T_LITERAL("OnFinalize"),
    CHAR_T_LITERAL("DotNetLib.Lib+OnFinalizeDelegation, CefDotnetApp"), // Delegate type
    nullptr,
    (void**)&on_finalize_fptr);
    if (rc || !on_finalize_fptr) {
        printf_log(LOG_SEVERITY_ERROR, "Failure: load on_finalize");
    }

    rc = load_assembly_and_get_function_pointer(
    dotnet_assembly_path.c_str(),
    dotnet_class_name,
    CHAR_T_LITERAL("OnBrowserInit"),
    CHAR_T_LITERAL("DotNetLib.Lib+OnBrowserInitDelegation, CefDotnetApp"), // Delegate type
    nullptr,
    (void**)&on_browser_init_fptr);
    if (rc || !on_browser_init_fptr) {
        printf_log(LOG_SEVERITY_ERROR, "Failure: load on_browser_init");
    }

    rc = load_assembly_and_get_function_pointer(
    dotnet_assembly_path.c_str(),
    dotnet_class_name,
    CHAR_T_LITERAL("OnBrowserFinalize"),
    CHAR_T_LITERAL("DotNetLib.Lib+OnBrowserFinalizeDelegation, CefDotnetApp"), // Delegate type
    nullptr,
    (void**)&on_browser_finalize_fptr);
    if (rc || !on_browser_finalize_fptr) {
        printf_log(LOG_SEVERITY_ERROR, "Failure: load on_browser_finalize");
    }

    rc = load_assembly_and_get_function_pointer(
    dotnet_assembly_path.c_str(),
    dotnet_class_name,
    CHAR_T_LITERAL("OnDevToolsMessage"),
    CHAR_T_LITERAL("DotNetLib.Lib+OnDevToolsMessageDelegation, CefDotnetApp"),
    nullptr,
    (void**)&on_devtools_message_fptr);
    if (rc || !on_devtools_message_fptr) {
        printf_log(LOG_SEVERITY_ERROR, "Failure: load on_devtools_message");
    }

    rc = load_assembly_and_get_function_pointer(
    dotnet_assembly_path.c_str(),
    dotnet_class_name,
    CHAR_T_LITERAL("OnDevToolsMethodResult"),
    CHAR_T_LITERAL("DotNetLib.Lib+OnDevToolsMethodResultDelegation, CefDotnetApp"),
    nullptr,
    (void**)&on_devtools_method_result_fptr);
    if (rc || !on_devtools_method_result_fptr) {
        printf_log(LOG_SEVERITY_ERROR, "Failure: load on_devtools_method_result");
    }

    rc = load_assembly_and_get_function_pointer(
    dotnet_assembly_path.c_str(),
    dotnet_class_name,
    CHAR_T_LITERAL("OnDevToolsEvent"),
    CHAR_T_LITERAL("DotNetLib.Lib+OnDevToolsEventDelegation, CefDotnetApp"),
    nullptr,
    (void**)&on_devtools_event_fptr);
    if (rc || !on_devtools_event_fptr) {
        printf_log(LOG_SEVERITY_ERROR, "Failure: load on_devtools_event");
    }

    rc = load_assembly_and_get_function_pointer(
    dotnet_assembly_path.c_str(),
    dotnet_class_name,
    CHAR_T_LITERAL("OnDevToolsAgentAttached"),
    CHAR_T_LITERAL("DotNetLib.Lib+OnDevToolsAgentAttachedDelegation, CefDotnetApp"),
    nullptr,
    (void**)&on_devtools_agent_attached_fptr);
    if (rc || !on_devtools_agent_attached_fptr) {
        printf_log(LOG_SEVERITY_ERROR, "Failure: load on_devtools_agent_attached");
    }

    rc = load_assembly_and_get_function_pointer(
    dotnet_assembly_path.c_str(),
    dotnet_class_name,
    CHAR_T_LITERAL("OnDevToolsAgentDetached"),
    CHAR_T_LITERAL("DotNetLib.Lib+OnDevToolsAgentDetachedDelegation, CefDotnetApp"),
    nullptr,
    (void**)&on_devtools_agent_detached_fptr);
    if (rc || !on_devtools_agent_detached_fptr) {
        printf_log(LOG_SEVERITY_ERROR, "Failure: load on_devtools_agent_detached");
    }

    rc = load_assembly_and_get_function_pointer(
    dotnet_assembly_path.c_str(),
    dotnet_class_name,
    CHAR_T_LITERAL("OnResourceResponseFilter"),
    CHAR_T_LITERAL("DotNetLib.Lib+OnResourceResponseFilterDelegation, CefDotnetApp"),
    nullptr,
    (void**)&on_resource_response_filter_fptr);
    if (rc || !on_resource_response_filter_fptr) {
        printf_log(LOG_SEVERITY_ERROR, "Failure: load on_resource_response_filter");
    }

    rc = load_assembly_and_get_function_pointer(
    dotnet_assembly_path.c_str(),
    dotnet_class_name,
    CHAR_T_LITERAL("OnResponseContentFilter"),
    CHAR_T_LITERAL("DotNetLib.Lib+OnResponseContentFilterDelegation, CefDotnetApp"),
    nullptr,
    (void**)&on_response_content_filter_fptr);
    if (rc || !on_response_content_filter_fptr) {
        printf_log(LOG_SEVERITY_ERROR, "Failure: load on_response_content_filter");
    }

    rc = load_assembly_and_get_function_pointer(
    dotnet_assembly_path.c_str(),
    dotnet_class_name,
    CHAR_T_LITERAL("OnBrowserHotReloadCopyFiles"),
    CHAR_T_LITERAL("DotNetLib.Lib+OnBrowserHotReloadCopyFilesDelegation, CefDotnetApp"), // Delegate type
    nullptr,
    (void**)&on_browser_hot_reload_copyfiles_fptr);
    if (rc || !on_browser_hot_reload_copyfiles_fptr) {
        printf_log(LOG_SEVERITY_ERROR, "Failure: load on_browser_hot_reload_copyfiles");
    }

    rc = load_assembly_and_get_function_pointer(
    dotnet_assembly_path.c_str(),
    dotnet_class_name,
    CHAR_T_LITERAL("OnBrowserHotReloadCompleted"),
    CHAR_T_LITERAL("DotNetLib.Lib+OnBrowserHotReloadCompletedDelegation, CefDotnetApp"), // Delegate type
    nullptr,
    (void**)&on_browser_hot_reload_completed_fptr);
    if (rc || !on_browser_hot_reload_completed_fptr) {
        printf_log(LOG_SEVERITY_ERROR, "Failure: load on_browser_hot_reload_completed");
    }

    rc = load_assembly_and_get_function_pointer(
    dotnet_assembly_path.c_str(),
    dotnet_class_name,
    CHAR_T_LITERAL("OnBrowserCefQuery"),
    CHAR_T_LITERAL("DotNetLib.Lib+OnBrowserCefQueryDelegation, CefDotnetApp"), // Delegate type
    nullptr,
    (void**)&on_browser_cef_query_fptr);
    if (rc || !on_browser_cef_query_fptr) {
        printf_log(LOG_SEVERITY_ERROR, "Failure: load on_browser_cef_query");
    }

    rc = load_assembly_and_get_function_pointer(
    dotnet_assembly_path.c_str(),
    dotnet_class_name,
    CHAR_T_LITERAL("OnRendererInit"),
    CHAR_T_LITERAL("DotNetLib.Lib+OnRendererInitDelegation, CefDotnetApp"), // Delegate type
    nullptr,
    (void**)&on_renderer_init_fptr);
    if (rc || !on_renderer_init_fptr) {
        printf_log(LOG_SEVERITY_ERROR, "Failure: load on_renderer_init");
    }

    rc = load_assembly_and_get_function_pointer(
    dotnet_assembly_path.c_str(),
    dotnet_class_name,
    CHAR_T_LITERAL("OnRendererFinalize"),
    CHAR_T_LITERAL("DotNetLib.Lib+OnRendererFinalizeDelegation, CefDotnetApp"), // Delegate type
    nullptr,
    (void**)&on_renderer_finalize_fptr);
    if (rc || !on_renderer_finalize_fptr) {
        printf_log(LOG_SEVERITY_ERROR, "Failure: load on_renderer_finalize");
    }

    rc = load_assembly_and_get_function_pointer(
    dotnet_assembly_path.c_str(),
    dotnet_class_name,
    CHAR_T_LITERAL("OnLoadingStateChange"),
    CHAR_T_LITERAL("DotNetLib.Lib+OnLoadingStateChangeDelegation, CefDotnetApp"), // Delegate type
    nullptr,
    (void**)&on_loading_state_change_fptr);
    if (rc || !on_loading_state_change_fptr) {
        printf_log(LOG_SEVERITY_ERROR, "Failure: load on_loading_state_change");
    }

    rc = load_assembly_and_get_function_pointer(
    dotnet_assembly_path.c_str(),
    dotnet_class_name,
    CHAR_T_LITERAL("OnLoadError"),
    CHAR_T_LITERAL("DotNetLib.Lib+OnLoadErrorDelegation, CefDotnetApp"), // Delegate type
    nullptr,
    (void**)&on_load_error_fptr);
    if (rc || !on_load_error_fptr) {
        printf_log(LOG_SEVERITY_ERROR, "Failure: load on_load_error");
    }

    rc = load_assembly_and_get_function_pointer(
    dotnet_assembly_path.c_str(),
    dotnet_class_name,
    CHAR_T_LITERAL("OnRenderProcessTerminated"),
    CHAR_T_LITERAL("DotNetLib.Lib+OnRenderProcessTerminatedDelegation, CefDotnetApp"), // Delegate type
    nullptr,
    (void**)&on_render_process_terminated_fptr);
    if (rc || !on_render_process_terminated_fptr) {
        printf_log(LOG_SEVERITY_ERROR, "Failure: load on_render_process_terminated");
    }

    rc = load_assembly_and_get_function_pointer(
    dotnet_assembly_path.c_str(),
    dotnet_class_name,
    CHAR_T_LITERAL("OnLoadStart"),
    CHAR_T_LITERAL("DotNetLib.Lib+OnLoadStartDelegation, CefDotnetApp"), // Delegate type
    nullptr,
    (void**)&on_load_start_fptr);
    if (rc || !on_load_start_fptr) {
        printf_log(LOG_SEVERITY_ERROR, "Failure: load on_load_start");
    }

    rc = load_assembly_and_get_function_pointer(
    dotnet_assembly_path.c_str(),
    dotnet_class_name,
    CHAR_T_LITERAL("OnLoadEnd"),
    CHAR_T_LITERAL("DotNetLib.Lib+OnLoadEndDelegation, CefDotnetApp"), // Delegate type
    nullptr,
    (void**)&on_load_end_fptr);
    if (rc || !on_load_end_fptr) {
        printf_log(LOG_SEVERITY_ERROR, "Failure: load on_load_end");
    }

    rc = load_assembly_and_get_function_pointer(
    dotnet_assembly_path.c_str(),
    dotnet_class_name,
    CHAR_T_LITERAL("OnRendererLoadStart"),
    CHAR_T_LITERAL("DotNetLib.Lib+OnRendererLoadStartDelegation, CefDotnetApp"), // Delegate type
    nullptr,
    (void**)&on_renderer_load_start_fptr);
    if (rc || !on_renderer_load_start_fptr) {
        printf_log(LOG_SEVERITY_ERROR, "Failure: load on_renderer_load_start");
    }

    rc = load_assembly_and_get_function_pointer(
    dotnet_assembly_path.c_str(),
    dotnet_class_name,
    CHAR_T_LITERAL("OnRendererLoadEnd"),
    CHAR_T_LITERAL("DotNetLib.Lib+OnRendererLoadEndDelegation, CefDotnetApp"), // Delegate type
    nullptr,
    (void**)&on_renderer_load_end_fptr);
    if (rc || !on_renderer_load_end_fptr) {
        printf_log(LOG_SEVERITY_ERROR, "Failure: load on_renderer_load_end");
    }

    rc = load_assembly_and_get_function_pointer(
    dotnet_assembly_path.c_str(),
    dotnet_class_name,
    CHAR_T_LITERAL("OnRendererLoadingStateChange"),
    CHAR_T_LITERAL("DotNetLib.Lib+OnRendererLoadingStateChangeDelegation, CefDotnetApp"), // Delegate type
    nullptr,
    (void**)&on_renderer_loading_state_change_fptr);
    if (rc || !on_renderer_loading_state_change_fptr) {
        printf_log(LOG_SEVERITY_ERROR, "Failure: load on_renderer_loading_state_change");
    }

    rc = load_assembly_and_get_function_pointer(
    dotnet_assembly_path.c_str(),
    dotnet_class_name,
    CHAR_T_LITERAL("OnRendererLoadError"),
    CHAR_T_LITERAL("DotNetLib.Lib+OnRendererLoadErrorDelegation, CefDotnetApp"), // Delegate type
    nullptr,
    (void**)&on_renderer_load_error_fptr);
    if (rc || !on_renderer_load_error_fptr) {
        printf_log(LOG_SEVERITY_ERROR, "Failure: load on_renderer_load_error");
    }

    rc = load_assembly_and_get_function_pointer(
    dotnet_assembly_path.c_str(),
    dotnet_class_name,
    CHAR_T_LITERAL("OnReceiveCefMessage"),
    CHAR_T_LITERAL("DotNetLib.Lib+OnReceiveCefMessageDelegation, CefDotnetApp"),
    nullptr,
    (void**)&on_receive_cef_message_fptr);
    if (rc || !on_receive_cef_message_fptr) {
        printf_log(LOG_SEVERITY_ERROR, "Failure: load on_receive_cef_message");
    }

    rc = load_assembly_and_get_function_pointer(
    dotnet_assembly_path.c_str(),
    dotnet_class_name,
    CHAR_T_LITERAL("OnExecuteMetaDSL"),
    CHAR_T_LITERAL("DotNetLib.Lib+OnExecuteMetaDSLDelegation, CefDotnetApp"),
    nullptr,
    (void**)&on_execute_metadsl_fptr);
    if (rc || !on_execute_metadsl_fptr) {
        printf_log(LOG_SEVERITY_ERROR, "Failure: load on_execute_metadsl");
    }

    rc = load_assembly_and_get_function_pointer(
    dotnet_assembly_path.c_str(),
    dotnet_class_name,
    CHAR_T_LITERAL("OnBeforeCommandLineProcessing"),
    CHAR_T_LITERAL("DotNetLib.Lib+OnBeforeCommandLineProcessingDelegation, CefDotnetApp"),
    nullptr,
    (void**)&on_before_command_line_processing_fptr);
    if (rc || !on_before_command_line_processing_fptr) {
        printf_log(LOG_SEVERITY_ERROR, "Failure: load on_before_command_line_processing");
    }

    rc = load_assembly_and_get_function_pointer(
    dotnet_assembly_path.c_str(),
    dotnet_class_name,
    CHAR_T_LITERAL("OnBeforeChildProcessLaunch"),
    CHAR_T_LITERAL("DotNetLib.Lib+OnBeforeChildProcessLaunchDelegation, CefDotnetApp"),
    nullptr,
    (void**)&on_before_child_process_launch_fptr);
    if (rc || !on_before_child_process_launch_fptr) {
        printf_log(LOG_SEVERITY_ERROR, "Failure: load on_before_child_process_launch");
    }

    rc = load_assembly_and_get_function_pointer(
    dotnet_assembly_path.c_str(),
    dotnet_class_name,
    CHAR_T_LITERAL("OnAlreadyRunningAppRelaunch"),
    CHAR_T_LITERAL("DotNetLib.Lib+OnAlreadyRunningAppRelaunchDelegation, CefDotnetApp"),
    nullptr,
    (void**)&on_already_running_app_relaunch_fptr);
    if (rc || !on_already_running_app_relaunch_fptr) {
        printf_log(LOG_SEVERITY_ERROR, "Failure: load on_already_running_app_relaunch");
    }

    rc = load_assembly_and_get_function_pointer(
    dotnet_assembly_path.c_str(),
    dotnet_class_name,
    CHAR_T_LITERAL("OnBeforeBrowse"),
    CHAR_T_LITERAL("DotNetLib.Lib+OnBeforeBrowseDelegation, CefDotnetApp"),
    nullptr,
    (void**)&on_before_browse_fptr);
    if (rc || !on_before_browse_fptr) {
        printf_log(LOG_SEVERITY_ERROR, "Failure: load on_before_browse");
    }

    rc = load_assembly_and_get_function_pointer(
    dotnet_assembly_path.c_str(),
    dotnet_class_name,
    CHAR_T_LITERAL("OnBeforeResourceLoad"),
    CHAR_T_LITERAL("DotNetLib.Lib+OnBeforeResourceLoadDelegation, CefDotnetApp"),
    nullptr,
    (void**)&on_before_resource_load_fptr);
    if (rc || !on_before_resource_load_fptr) {
        printf_log(LOG_SEVERITY_ERROR, "Failure: load on_before_resource_load");
    }

    rc = load_assembly_and_get_function_pointer(
    dotnet_assembly_path.c_str(),
    dotnet_class_name,
    CHAR_T_LITERAL("OnHeartBeat"),
    CHAR_T_LITERAL("DotNetLib.Lib+OnHeartBeatDelegation, CefDotnetApp"),
    nullptr,
    (void**)&on_heart_beat_fptr);
    if (rc || !on_heart_beat_fptr) {
        printf_log(LOG_SEVERITY_ERROR, "Failure: load on_heart_beat");
    }

    rc = load_assembly_and_get_function_pointer(
    dotnet_assembly_path.c_str(),
    dotnet_class_name,
    CHAR_T_LITERAL("OnCallMetaDSL"),
    CHAR_T_LITERAL("DotNetLib.Lib+OnCallMetaDSLDelegation, CefDotnetApp"),
    nullptr,
    (void**)&on_call_metadsl_fptr);
    if (rc || !on_call_metadsl_fptr) {
        printf_log(LOG_SEVERITY_ERROR, "Failure: load on_call_metadsl");
    }

    rc = load_assembly_and_get_function_pointer(
    dotnet_assembly_path.c_str(),
    dotnet_class_name,
    CHAR_T_LITERAL("OnConsoleLog"),
    CHAR_T_LITERAL("DotNetLib.Lib+OnConsoleLogDelegation, CefDotnetApp"),
    nullptr,
    (void**)&on_console_log_fptr);
    if (rc || !on_console_log_fptr) {
        printf_log(LOG_SEVERITY_ERROR, "Failure: load on_console_log");
    }

    return 0;
}

#if defined(_MSC_VER)
#include <tlhelp32.h>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")

#pragma pack(push, 8)
// Windows structures for reading command line
typedef NTSTATUS (NTAPI *PFN_NtQueryInformationProcess)(
    HANDLE ProcessHandle,
    DWORD ProcessInformationClass,
    PVOID ProcessInformation,
    DWORD ProcessInformationLength,
    PDWORD ReturnLength);

typedef struct _MY_RTL_USER_PROCESS_PARAMETERS {
    BYTE Reserved1[16];
    PVOID Reserved2[10];
    UNICODE_STRING ImagePathName;
    UNICODE_STRING CommandLine;
} MY_RTL_USER_PROCESS_PARAMETERS, *PMY_RTL_USER_PROCESS_PARAMETERS;

typedef struct _MY_PEB {
    BYTE Reserved1[2];
    BYTE BeingDebugged;
    BYTE Reserved2[1];
    PVOID Reserved3[2];
    PVOID Ldr;
    PMY_RTL_USER_PROCESS_PARAMETERS ProcessParameters;
    BYTE Reserved4[104];
    PVOID Reserved5[52];
    PVOID PostProcessInitRoutine;
    BYTE Reserved6[128];
    PVOID Reserved7[1];
    ULONG SessionId;
} MY_PEB, *PMY_PEB;
#pragma pack(pop)

#define ProcessBasicInformation 0

#elif defined(__linux__)
#include <dirent.h>
#include <unistd.h>
#include <sys/types.h>
#elif defined(__APPLE__)
#include <libproc.h>
#include <sys/sysctl.h>
#endif

// Cross-platform function to terminate renderer processes
// Returns the number of renderer processes terminated, or -1 on error
int TerminateRenderProcess() {
    int terminated_count = 0;
#if defined(_MSC_VER)
    // Windows implementation
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        printf_log(LOG_SEVERITY_ERROR, "TerminateRenderProcess: Failed to create process snapshot");
        return -1;
    }

    PROCESSENTRY32 pe32;
    pe32.dwSize = sizeof(PROCESSENTRY32);

    if (!Process32First(snapshot, &pe32)) {
        printf_log(LOG_SEVERITY_ERROR, "TerminateRenderProcess: Failed to get first process");
        CloseHandle(snapshot);
        return -1;
    }

    DWORD current_pid = GetCurrentProcessId();

    // Dynamically load NtQueryInformationProcess for reading command line
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    PFN_NtQueryInformationProcess pNtQueryInformationProcess = nullptr;
    if (ntdll) {
        pNtQueryInformationProcess = reinterpret_cast<PFN_NtQueryInformationProcess>(
            GetProcAddress(ntdll, "NtQueryInformationProcess"));
    }

    // Loop through processes
    do {
        // Skip current process
        if (pe32.th32ProcessID == current_pid) {
            continue;
        }

        // Only process child processes of current process
        if (pe32.th32ParentProcessID != current_pid) {
            continue;
        }

        // Open process with required access rights
        HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_TERMINATE,
                                    FALSE, pe32.th32ProcessID);
        if (!process) {
            continue;
        }

        // Try to read command line from PEB
        bool is_renderer = false;

        if (pNtQueryInformationProcess) {
            MY_PROCESS_BASIC_INFORMATION pbi;
            DWORD return_length = 0;

            // Query process basic information to get PEB address
            NTSTATUS status = pNtQueryInformationProcess(process, ProcessBasicInformation,
                                                         &pbi, sizeof(pbi), &return_length);

            if (status >= 0 && pbi.PebBaseAddress) {
                // Check if PEB memory is readable before reading
                if (!IsMemoryReadable(process, pbi.PebBaseAddress)) {
                    printf_log(LOG_SEVERITY_WARNING,
                              "TerminateRenderProcess: PEB memory not readable for PID=%d",
                              pe32.th32ProcessID);
                    CloseHandle(process);
                    continue;
                }

                // Read PEB from target process
                MY_PEB peb;
                SIZE_T bytes_read = 0;

                if (ReadProcessMemory(process, pbi.PebBaseAddress, &peb, sizeof(peb), &bytes_read)) {
                    if (peb.ProcessParameters) {
                        // Check if ProcessParameters memory is readable before reading
                        if (!IsMemoryReadable(process, peb.ProcessParameters)) {
                            printf_log(LOG_SEVERITY_WARNING,
                                      "TerminateRenderProcess: ProcessParameters memory not readable for PID=%d",
                                      pe32.th32ProcessID);
                            CloseHandle(process);
                            continue;
                        }

                        // Read process parameters
                        MY_RTL_USER_PROCESS_PARAMETERS params;
                        if (ReadProcessMemory(process, peb.ProcessParameters, &params,
                                              sizeof(params), &bytes_read)) {
                            // Check if CommandLine.Buffer memory is readable before reading
                            if (!IsMemoryReadable(process, params.CommandLine.Buffer)) {
                                printf_log(LOG_SEVERITY_WARNING,
                                          "TerminateRenderProcess: CommandLine.Buffer memory not readable for PID=%d",
                                          pe32.th32ProcessID);
                                CloseHandle(process);
                                continue;
                            }

                            // Read command line string
                            WCHAR* cmd_line = (WCHAR*)malloc(params.CommandLine.MaximumLength + sizeof(WCHAR));
                            if (cmd_line) {
                                if (ReadProcessMemory(process, params.CommandLine.Buffer,
                                                     cmd_line, params.CommandLine.Length, &bytes_read)) {
                                    // Null-terminate the string
                                    cmd_line[params.CommandLine.Length / sizeof(WCHAR)] = L'\0';

                                    // Convert to narrow string for easier comparison
                                    int cmd_line_len = WideCharToMultiByte(CP_UTF8, 0, cmd_line, -1,
                                                                          NULL, 0, NULL, NULL);
                                    if (cmd_line_len > 0) {
                                        char* cmd_line_utf8 = (char*)malloc(cmd_line_len);
                                        if (cmd_line_utf8) {
                                            WideCharToMultiByte(CP_UTF8, 0, cmd_line, -1,
                                                              cmd_line_utf8, cmd_line_len, NULL, NULL);

                                            // Check if command line contains --type=renderer
                                            if (strstr(cmd_line_utf8, "--type=renderer")) {
                                                is_renderer = true;
                                                printf_log(LOG_SEVERITY_INFO,
                                                          "TerminateRenderProcess: Found renderer process PID=%d",
                                                          pe32.th32ProcessID);
                                            }

                                            free(cmd_line_utf8);
                                        }
                                    }
                                }
                                free(cmd_line);
                            }
                        }
                    }
                }
            }
        }

        if (!is_renderer && !pNtQueryInformationProcess) {
            // Fallback: check if it's cefclient.exe (less precise)
            char exe_name[MAX_PATH];
            if (GetModuleBaseNameA(process, NULL, exe_name, MAX_PATH)) {
                if (strstr(exe_name, "cefclient.exe")) {
                    printf_log(LOG_SEVERITY_WARNING,
                              "TerminateRenderProcess: Using fallback for cefclient.exe PID=%d",
                              pe32.th32ProcessID);
                    is_renderer = true;
                }
            }
        }

        // Terminate renderer process
        if (is_renderer) {
            if (TerminateProcess(process, 0)) {
                terminated_count++;
                printf_log(LOG_SEVERITY_INFO,
                          "TerminateRenderProcess: Terminated renderer process PID=%d",
                          pe32.th32ProcessID);
            } else {
                printf_log(LOG_SEVERITY_WARNING,
                          "TerminateRenderProcess: Failed to terminate PID=%d, error=%lu",
                          pe32.th32ProcessID, GetLastError());
            }
        }

        CloseHandle(process);

    } while (Process32Next(snapshot, &pe32));

    CloseHandle(snapshot);

#elif defined(__linux__)
    // Linux implementation
    DIR* proc_dir = opendir("/proc");
    if (!proc_dir) {
        printf_log(LOG_SEVERITY_ERROR, "TerminateRenderProcess: Failed to open /proc");
        return -1;
    }

    pid_t current_pid = getpid();

    struct dirent* entry;
    while ((entry = readdir(proc_dir)) != NULL) {
        // Skip non-numeric directories
        if (!isdigit(entry->d_name[0])) {
            continue;
        }

        pid_t pid = atoi(entry->d_name);

        // Skip current process
        if (pid == current_pid) {
            continue;
        }

        // Check if it's a child process of current process
        char status_path[256];
        snprintf(status_path, sizeof(status_path), "/proc/%d/status", pid);

        FILE* status_file = fopen(status_path, "r");
        if (!status_file) {
            continue;
        }

        // Read status to find PPid
        char status_line[256];
        pid_t parent_pid = 0;
        while (fgets(status_line, sizeof(status_line), status_file)) {
            if (strncmp(status_line, "PPid:", 5) == 0) {
                sscanf(status_line, "PPid:\t%d", &parent_pid);
                break;
            }
        }
        fclose(status_file);

        // Only process child processes of current process
        if (parent_pid != current_pid) {
            continue;
        }

        // Read cmdline
        char cmdline_path[256];
        snprintf(cmdline_path, sizeof(cmdline_path), "/proc/%d/cmdline", pid);

        FILE* cmdline_file = fopen(cmdline_path, "r");
        if (!cmdline_file) {
            continue;
        }

        char cmdline[8192];
        size_t bytes_read = fread(cmdline, 1, sizeof(cmdline) - 1, cmdline_file);
        cmdline[bytes_read] = '\0';
        fclose(cmdline_file);

        // Check if it's a renderer process
        if (strstr(cmdline, "--type=renderer")) {
            printf_log(LOG_SEVERITY_INFO, "TerminateRenderProcess: Found renderer process PID=%d", pid);

            // Terminate the process
            if (kill(pid, SIGTERM) == 0) {
                terminated_count++;
                printf_log(LOG_SEVERITY_INFO, "TerminateRenderProcess: Terminated renderer process PID=%d", pid);
            } else {
                printf_log(LOG_SEVERITY_WARNING, "TerminateRenderProcess: Failed to terminate PID=%d", pid);
            }
        }
    }

    closedir(proc_dir);

#elif defined(__APPLE__)
    // macOS implementation
    pid_t current_pid = getpid();
    int num_pids = proc_listpids(PROC_ALL_PIDS, 0, NULL, 0);

    if (num_pids <= 0) {
        printf_log(LOG_SEVERITY_ERROR, "TerminateRenderProcess: Failed to get process list");
        return -1;
    }

    std::vector<pid_t> pids(num_pids);
    proc_listpids(PROC_ALL_PIDS, 0, pids.data(), sizeof(pid_t) * num_pids);

    for (int i = 0; i < num_pids; i++) {
        if (pids[i] <= 0 || pids[i] == current_pid) {
            continue;
        }

        // Get process information to check parent PID
        struct kinfo_proc proc_info;
        size_t proc_info_size = sizeof(proc_info);
        int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, pids[i]};

        if (sysctl(mib, 4, &proc_info, &proc_info_size, NULL, 0) != 0) {
            continue;
        }

        // Only process child processes of current process
        if (proc_info.kp_eproc.e_ppid != current_pid) {
            continue;
        }

        // Get process arguments
        char args_buffer[MAXPATHLEN * 4];
        int mib_args[3] = {CTL_KERN, KERN_PROCARGS2, pids[i]};
        size_t args_size = sizeof(args_buffer);

        if (sysctl(mib_args, 3, args_buffer, &args_size, NULL, 0) != 0) {
            continue;
        }

        // Parse arguments
        int argc;
        memcpy(&argc, args_buffer, sizeof(argc));

        char* args_end = args_buffer + args_size;
        char* exe_path = args_buffer + sizeof(argc);
        char* current_arg = exe_path + strlen(exe_path) + 1;

        // Skip to first argument
        while (current_arg < args_end && *current_arg == '\0') {
            current_arg++;
        }

        // Check all arguments for --type=renderer
        bool is_renderer = false;
        char* arg = current_arg;
        for (int j = 0; j < argc && arg < args_end; j++) {
            if (strstr(arg, "--type=renderer")) {
                is_renderer = true;
                break;
            }

            // Move to next argument
            arg += strlen(arg) + 1;
            while (arg < args_end && *arg == '\0') {
                arg++;
            }
        }

        if (is_renderer) {
            printf_log(LOG_SEVERITY_INFO, "TerminateRenderProcess: Found renderer process PID=%d", pids[i]);

            // Terminate the process
            if (kill(pids[i], SIGTERM) == 0) {
                terminated_count++;
                printf_log(LOG_SEVERITY_INFO, "TerminateRenderProcess: Terminated renderer process PID=%d", pids[i]);
            } else {
                printf_log(LOG_SEVERITY_WARNING, "TerminateRenderProcess: Failed to terminate PID=%d", pids[i]);
            }
        }
    }
#else
    printf_log(LOG_SEVERITY_ERROR, "TerminateRenderProcess: Not implemented for this platform");
    return -1;
#endif

    printf_log(LOG_SEVERITY_INFO, "TerminateRenderProcess: Terminated %d renderer process(es)", terminated_count);
    return terminated_count;
}

// Count renderer processes using platform-specific APIs
// Returns the number of renderer processes, or -1 on error
int CountRenderProcess() {
    int renderer_count = 0;

#if defined(OS_WIN)
    // Windows implementation
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        printf_log(LOG_SEVERITY_ERROR, "CountRenderProcess: Failed to create process snapshot");
        return -1;
    }

    PROCESSENTRY32W pe32;
    pe32.dwSize = sizeof(PROCESSENTRY32W);

    if (!Process32FirstW(snapshot, &pe32)) {
        CloseHandle(snapshot);
        printf_log(LOG_SEVERITY_ERROR, "CountRenderProcess: Failed to get first process");
        return -1;
    }

    DWORD current_pid = GetCurrentProcessId();

    // Dynamically load NtQueryInformationProcess for reading command line
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    PFN_NtQueryInformationProcess pNtQueryInformationProcess = nullptr;
    if (ntdll) {
        pNtQueryInformationProcess = reinterpret_cast<PFN_NtQueryInformationProcess>(
            GetProcAddress(ntdll, "NtQueryInformationProcess"));
    }

    do {
        if (pe32.th32ProcessID == current_pid) {
            continue;
        }

        // Only process child processes of current process
        if (pe32.th32ParentProcessID != current_pid) {
            continue;
        }

        HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pe32.th32ProcessID);
        if (!process) {
            continue;
        }

        bool is_renderer = false;

        if (pNtQueryInformationProcess) {
            MY_PROCESS_BASIC_INFORMATION pbi;

            pbi.Reserved1 = nullptr;
            pbi.PebBaseAddress = nullptr;
            pbi.Reserved2[0] = nullptr;
            pbi.Reserved2[1] = nullptr;
            pbi.UniqueProcessId = 0;
            pbi.Reserved3 = nullptr;

            NTSTATUS status = pNtQueryInformationProcess(process, ProcessBasicInformation,
                                                         &pbi, sizeof(pbi), nullptr);
            if (status == 0 && pbi.PebBaseAddress) {
                // Check if PEB memory is readable before reading
                if (!IsMemoryReadable(process, pbi.PebBaseAddress)) {
                    printf_log(LOG_SEVERITY_WARNING,
                              "CountRenderProcess: PEB memory not readable for PID=%d",
                              pe32.th32ProcessID);
                    CloseHandle(process);
                    continue;
                }

                MY_PEB peb;
                SIZE_T bytes_read;
                if (ReadProcessMemory(process, pbi.PebBaseAddress, &peb, sizeof(peb), &bytes_read)) {
                    // Check if ProcessParameters memory is readable before reading
                    if (!IsMemoryReadable(process, peb.ProcessParameters)) {
                        printf_log(LOG_SEVERITY_WARNING,
                                  "CountRenderProcess: ProcessParameters memory not readable for PID=%d",
                                  pe32.th32ProcessID);
                        CloseHandle(process);
                        continue;
                    }

                    MY_RTL_USER_PROCESS_PARAMETERS params;
                    if (ReadProcessMemory(process, peb.ProcessParameters, &params, sizeof(params), &bytes_read)) {
                        // Check if CommandLine.Buffer memory is readable before reading
                        if (!IsMemoryReadable(process, params.CommandLine.Buffer)) {
                            printf_log(LOG_SEVERITY_WARNING,
                                      "CountRenderProcess: CommandLine.Buffer memory not readable for PID=%d",
                              pe32.th32ProcessID);
                            CloseHandle(process);
                            continue;
                        }

                        // Read command line string
                        WCHAR* cmd_line = (WCHAR*)malloc(
                            params.CommandLine.MaximumLength + sizeof(WCHAR));
                        if (cmd_line) {
                          if (ReadProcessMemory(
                                  process, params.CommandLine.Buffer, cmd_line,
                                  params.CommandLine.Length, &bytes_read)) {
                            // Null-terminate the string
                            cmd_line[params.CommandLine.Length /
                                     sizeof(WCHAR)] = L'\0';

                            // Convert to narrow string for easier comparison
                            int cmd_line_len = WideCharToMultiByte(
                                CP_UTF8, 0, cmd_line, -1, NULL, 0, NULL, NULL);
                            if (cmd_line_len > 0) {
                              char* cmd_line_utf8 = (char*)malloc(cmd_line_len);
                              if (cmd_line_utf8) {
                                WideCharToMultiByte(CP_UTF8, 0, cmd_line, -1,
                                                    cmd_line_utf8, cmd_line_len,
                                                    NULL, NULL);

                                // Check if command line contains
                                // --type=renderer
                                if (strstr(cmd_line_utf8, "--type=renderer")) {
                                  is_renderer = true;
                                  printf_log(LOG_SEVERITY_INFO,
                                             "CountRenderProcess: Found renderer process PID=%d", pe32.th32ProcessID);
                                }

                                free(cmd_line_utf8);
                              }
                            }
                          }
                          free(cmd_line);
                        }
                    }
                }
            }
        }

        if (!is_renderer && !pNtQueryInformationProcess) {
            char exe_name[MAX_PATH];
            if (GetModuleBaseNameA(process, NULL, exe_name, MAX_PATH)) {
                if (strstr(exe_name, "cefclient.exe")) {
                    is_renderer = true;
                }
            }
        }

        if (is_renderer) {
            renderer_count++;
            printf_log(LOG_SEVERITY_INFO, "CountRenderProcess: Found renderer process PID=%d", pe32.th32ProcessID);
        }

        CloseHandle(process);

    } while (Process32NextW(snapshot, &pe32));

    CloseHandle(snapshot);

#elif defined(__linux__)
    // Linux implementation
    DIR* proc_dir = opendir("/proc");
    if (!proc_dir) {
        printf_log(LOG_SEVERITY_ERROR, "CountRenderProcess: Failed to open /proc");
        return -1;
    }

    pid_t current_pid = getpid();

    struct dirent* entry;
    while ((entry = readdir(proc_dir)) != NULL) {
        if (!isdigit(entry->d_name[0])) {
            continue;
        }

        pid_t pid = atoi(entry->d_name);

        if (pid == current_pid) {
            continue;
        }

        // Check if it's a child process of current process
        char status_path[256];
        snprintf(status_path, sizeof(status_path), "/proc/%d/status", pid);

        FILE* status_file = fopen(status_path, "r");
        if (!status_file) {
            continue;
        }

        char status_line[256];
        pid_t parent_pid = 0;
        while (fgets(status_line, sizeof(status_line), status_file)) {
            if (strncmp(status_line, "PPid:", 5) == 0) {
                sscanf(status_line, "PPid:\t%d", &parent_pid);
                break;
            }
        }
        fclose(status_file);

        // Only process child processes of current process
        if (parent_pid != current_pid) {
            continue;
        }

        char cmdline_path[256];
        snprintf(cmdline_path, sizeof(cmdline_path), "/proc/%d/cmdline", pid);

        FILE* cmdline_file = fopen(cmdline_path, "r");
        if (!cmdline_file) {
            continue;
        }

        char cmdline[8192];
        size_t bytes_read = fread(cmdline, 1, sizeof(cmdline) - 1, cmdline_file);
        cmdline[bytes_read] = '\0';
        fclose(cmdline_file);

        if (strstr(cmdline, "--type=renderer")) {
            renderer_count++;
            printf_log(LOG_SEVERITY_INFO, "CountRenderProcess: Found renderer process PID=%d", pid);
        }
    }

    closedir(proc_dir);

#elif defined(__APPLE__)
    // macOS implementation
    pid_t current_pid = getpid();
    int num_pids = proc_listpids(PROC_ALL_PIDS, 0, NULL, 0);

    if (num_pids <= 0) {
        printf_log(LOG_SEVERITY_ERROR, "CountRenderProcess: Failed to get process list");
        return -1;
    }

    std::vector<pid_t> pids(num_pids);
    proc_listpids(PROC_ALL_PIDS, 0, pids.data(), sizeof(pid_t) * num_pids);

    for (int i = 0; i < num_pids; i++) {
        if (pids[i] <= 0 || pids[i] == current_pid) {
            continue;
        }

        // Get process information to check parent PID
        struct kinfo_proc proc_info;
        size_t proc_info_size = sizeof(proc_info);
        int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, pids[i]};

        if (sysctl(mib, 4, &proc_info, &proc_info_size, NULL, 0) != 0) {
            continue;
        }

        // Only process child processes of current process
        if (proc_info.kp_eproc.e_ppid != current_pid) {
            continue;
        }

        char args_buffer[MAXPATHLEN * 4];
        int mib_args[3] = {CTL_KERN, KERN_PROCARGS2, pids[i]};
        size_t args_size = sizeof(args_buffer);

        if (sysctl(mib_args, 3, args_buffer, &args_size, NULL, 0) != 0) {
            continue;
        }

        int argc;
        memcpy(&argc, args_buffer, sizeof(argc));

        char* args_end = args_buffer + args_size;
        char* exe_path = args_buffer + sizeof(argc);
        char* current_arg = exe_path + strlen(exe_path) + 1;

        while (current_arg < args_end && *current_arg == '\0') {
            current_arg++;
        }

        bool is_renderer = false;
        char* arg = current_arg;
        for (int j = 0; j < argc && arg < args_end; j++) {
            if (strstr(arg, "--type=renderer")) {
                is_renderer = true;
                break;
            }
            arg += strlen(arg) + 1;
        }

        if (is_renderer) {
            renderer_count++;
            printf_log(LOG_SEVERITY_INFO, "CountRenderProcess: Found renderer process PID=%d", pids[i]);
        }
    }
#else
    printf_log(LOG_SEVERITY_ERROR, "CountRenderProcess: Unsupported platform");
    return -1;
#endif

    printf_log(LOG_SEVERITY_INFO, "CountRenderProcess: Found %d renderer process(es)", renderer_count);
    return renderer_count;
}

