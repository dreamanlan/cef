#include "HostCLR.h"
#include "include/base/cef_logging.h"
#include "include/cef_command_line.h"
#include "include/cef_browser.h"
#include "include/cef_frame.h"
#include "JavaScriptCaller.h"
#include "path_utils.h"

#include <iostream>
#include <mutex>
#include <string>
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
    buffer[len] = '\0';
    va_end(vl);

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
    // On macOS, return .app directory path with trailing separator
    std::string appPath = GetMacAppDirPath();
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
    printf("[native] hostfxr path: %s\n", hostfxr_path);
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
on_receive_js_message_fn on_receive_js_message_fptr = nullptr;
on_execute_metadsl_fn on_execute_metadsl_fptr = nullptr;
on_before_command_line_processing_fn on_before_command_line_processing_fptr = nullptr;

// Native api
typedef void (*host_native_log_fn)(const char* msg, void* browser, void* frame);
typedef void (*send_javascript_code_fn)(const char* code, void* browser, void* frame);

typedef void (*send_cef_message_fn)(const char* msg, const char** args, int argCount, void* browser, void* frame, int source_process_id);
typedef void (*send_javascript_call_fn)(const char* func, const char** args, int argCount, void* browser, void* frame);
typedef const char* (*call_javascript_func_in_renderer_fn)(const char* func, const char** args, int argCount, void* browser, void* frame);
typedef void (*free_native_string_fn)(const char* str);

typedef bool (*command_line_has_switch_fn)(void* command_line, const char* name);
typedef const char* (*command_line_get_switch_value_fn)(void* command_line, const char* name);
typedef void (*command_line_append_switch_fn)(void* command_line, const char* name);
typedef void (*command_line_append_switch_with_value_fn)(void* command_line, const char* name, const char* value);
typedef void (*command_line_remove_switch_fn)(void* command_line, const char* name);

// Browser traversal
typedef const char* (*get_all_browser_ids_fn)();
typedef void* (*get_browser_by_id_fn)(int browser_id);
typedef void (*notify_browser_created_fn)(void* browser);
typedef void (*notify_browser_destroyed_fn)(void* browser);

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

typedef struct {
    host_native_log_fn NativeLog;
    send_cef_message_fn SendCefMessage;
    send_javascript_code_fn SendJavascriptCode;
    send_javascript_call_fn SendJavascriptCall;
    call_javascript_func_in_renderer_fn CallJavascriptFuncInRenderer;
    free_native_string_fn FreeNativeString;
    command_line_has_switch_fn CommandLineHasSwitch;
    command_line_get_switch_value_fn CommandLineGetSwitchValue;
    command_line_append_switch_fn CommandLineAppendSwitch;
    command_line_append_switch_with_value_fn CommandLineAppendSwitchWithValue;
    command_line_remove_switch_fn CommandLineRemoveSwitch;
    // Browser traversal
    get_all_browser_ids_fn GetAllBrowserIds;
    get_browser_by_id_fn GetBrowserById;
    notify_browser_created_fn NotifyBrowserCreated;
    notify_browser_destroyed_fn NotifyBrowserDestroyed;
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

// Global browser id list (maintained by NotifyBrowserCreated/NotifyBrowserDestroyed)
static std::vector<int>* g_browser_ids = new std::vector<int>();
static std::mutex* g_browser_ids_mutex = new std::mutex();
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

void notify_browser_created(void* browser)
{
    if (!browser) return;
    int id = reinterpret_cast<CefBrowser*>(browser)->GetIdentifier();
    std::lock_guard<std::mutex> lock(*g_browser_ids_mutex);
    for (int existing : *g_browser_ids) {
        if (existing == id) return;
    }
    g_browser_ids->push_back(id);
}

void notify_browser_destroyed(void* browser)
{
    if (!browser) return;
    int id = reinterpret_cast<CefBrowser*>(browser)->GetIdentifier();
    std::lock_guard<std::mutex> lock(*g_browser_ids_mutex);
    g_browser_ids->erase(std::remove(g_browser_ids->begin(), g_browser_ids->end(), id), g_browser_ids->end());
}

const char* get_all_browser_ids()
{
    std::lock_guard<std::mutex> lock(*g_browser_ids_mutex);
    if (g_browser_ids->empty()) return nullptr;
    std::string result;
    for (int id : *g_browser_ids) {
        if (!result.empty()) result += "\n";
        result += std::to_string(id);
    }
    return alloc_string(result);
}

void cleanup_browser_ids()
{
    delete g_browser_ids_mutex;
    g_browser_ids_mutex = nullptr;
    delete g_browser_ids;
    g_browser_ids = nullptr;
}

void* get_browser_by_id(int browser_id)
{
if (!is_browser_process()) return nullptr;
    auto browser = CefBrowserHost::GetBrowserByIdentifier(browser_id);
    return browser.get();
}

// --- Browser properties ---

int browser_get_id(void* browser)
{
    if (!browser) return 0;
    return reinterpret_cast<CefBrowser*>(browser)->GetIdentifier();
}

const char* browser_get_url(void* browser)
{
    if (!browser) return nullptr;
    auto* pBrowser = reinterpret_cast<CefBrowser*>(browser);
    auto frame = pBrowser->GetMainFrame();
    if (!frame) return nullptr;
    std::string url = frame->GetURL().ToString();
    if (url.empty()) return nullptr;
    return alloc_string(url);
}

bool browser_is_loading(void* browser)
{
    if (!browser) return false;
    return reinterpret_cast<CefBrowser*>(browser)->IsLoading();
}

bool browser_is_popup(void* browser)
{
    if (!browser) return false;
    return reinterpret_cast<CefBrowser*>(browser)->IsPopup();
}

bool browser_has_document(void* browser)
{
    if (!browser) return false;
    return reinterpret_cast<CefBrowser*>(browser)->HasDocument();
}

// --- Browser frame access ---

int browser_get_frame_count(void* browser)
{
    if (!browser) return 0;
    return static_cast<int>(reinterpret_cast<CefBrowser*>(browser)->GetFrameCount());
}

const char* browser_get_frame_identifiers(void* browser)
{
    if (!browser) return nullptr;
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
    if (!browser) return nullptr;
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
    if (!browser) return nullptr;
    return reinterpret_cast<CefBrowser*>(browser)->GetMainFrame().get();
}

void* browser_get_focused_frame(void* browser)
{
    if (!browser) return nullptr;
    return reinterpret_cast<CefBrowser*>(browser)->GetFocusedFrame().get();
}

void* browser_get_frame_by_identifier(void* browser, const char* identifier)
{
    if (!browser || !identifier) return nullptr;
    return reinterpret_cast<CefBrowser*>(browser)->GetFrameByIdentifier(identifier).get();
}

void* browser_get_frame_by_name(void* browser, const char* name)
{
    if (!browser || !name) return nullptr;
    return reinterpret_cast<CefBrowser*>(browser)->GetFrameByName(name).get();
}

// --- Browser actions ---

void browser_reload(void* browser)
{
    if (!browser) return;
    reinterpret_cast<CefBrowser*>(browser)->Reload();
}

void browser_reload_ignore_cache(void* browser)
{
    if (!browser) return;
    reinterpret_cast<CefBrowser*>(browser)->ReloadIgnoreCache();
}

void browser_stop_load(void* browser)
{
    if (!browser) return;
    reinterpret_cast<CefBrowser*>(browser)->StopLoad();
}

// --- Browser host actions (browser process only) ---

void browser_close(void* browser, int force_close)
{
    if (!browser || !is_browser_process()) return;
    auto host = reinterpret_cast<CefBrowser*>(browser)->GetHost();
    if (host) {
        host->CloseBrowser(force_close != 0);
    }
}

void browser_set_focus(void* browser, int focus)
{
    if (!browser || !is_browser_process()) return;
    auto host = reinterpret_cast<CefBrowser*>(browser)->GetHost();
    if (host) {
        host->SetFocus(focus != 0);
    }
}

int browser_get_opener_id(void* browser)
{
    if (!browser || !is_browser_process()) return 0;
    auto host = reinterpret_cast<CefBrowser*>(browser)->GetHost();
    if (host) {
        return host->GetOpenerIdentifier();
    }
    return 0;
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

// Function to call .NET Core method
int load_dotnet_method(bool is_debug, int& rc)
{
    string_t dotnet_assembly_path = BuildAssemblyPath(is_debug);
    const char_t* dotnet_class_name = c_dotnet_class_name;
    // native api
    HostApi api;
    api.NativeLog = &host_native_log;
    api.SendCefMessage = &send_cef_message;
    api.SendJavascriptCode = &send_javascript_code;
    api.SendJavascriptCall = &send_javascript_call;
    api.CallJavascriptFuncInRenderer = &call_javascript_func_in_renderer;
    api.FreeNativeString = &free_native_string;
    api.CommandLineHasSwitch = &command_line_has_switch;
    api.CommandLineGetSwitchValue = &command_line_get_switch_value;
    api.CommandLineAppendSwitch = &command_line_append_switch;
    api.CommandLineAppendSwitchWithValue = &command_line_append_switch_with_value;
    api.CommandLineRemoveSwitch = &command_line_remove_switch;
    api.GetAllBrowserIds = &get_all_browser_ids;
    api.GetBrowserById = &get_browser_by_id;
    api.NotifyBrowserCreated = &notify_browser_created;
    api.NotifyBrowserDestroyed = &notify_browser_destroyed;
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
    api.FrameGetUrl = &frame_get_url;
    api.FrameGetName = &frame_get_name;
    api.FrameGetIdentifier = &frame_get_identifier;
    api.FrameIsMain = &frame_is_main;
    api.FrameIsValid = &frame_is_valid;
    api.FrameIsFocused = &frame_is_focused;
    api.FrameGetParent = &frame_get_parent;
    api.FrameGetBrowser = &frame_get_browser;
    api.FrameLoadUrl = &frame_load_url;

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
    CHAR_T_LITERAL("OnReceiveJsMessage"),
    CHAR_T_LITERAL("DotNetLib.Lib+OnReceiveJsMessageDelegation, CefDotnetApp"),
    nullptr,
    (void**)&on_receive_js_message_fptr);
    if (rc || !on_receive_js_message_fptr) {
        printf_log(LOG_SEVERITY_ERROR, "Failure: load on_receive_js_message");
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

