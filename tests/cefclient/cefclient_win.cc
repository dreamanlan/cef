// Copyright (c) 2015 The Chromium Embedded Framework Authors. All rights
// reserved. Use of this source code is governed by a BSD-style license that
// can be found in the LICENSE file.

#include <windows.h>

#include <algorithm>
#include <memory>

#include "include/cef_command_line.h"
#include "include/cef_sandbox_win.h"
#include "include/wrapper/cef_certificate_util_win.h"
#include "include/wrapper/cef_library_loader.h"
#include "include/wrapper/cef_util_win.h"
#include "tests/cefclient/browser/main_context_impl.h"
#include "tests/cefclient/browser/main_message_loop_multithreaded_win.h"
#include "tests/cefclient/browser/resource.h"
#include "tests/cefclient/browser/root_window_manager.h"
#include "tests/cefclient/browser/test_runner.h"
#include "tests/cefclient/hostclr/HostCLR.h"
#include "tests/shared/browser/client_app_browser.h"
#include "tests/shared/browser/main_message_loop_external_pump.h"
#include "tests/shared/browser/main_message_loop_std.h"
#include "tests/shared/browser/util_win.h"
#include "tests/shared/common/client_app_other.h"
#include "tests/shared/common/client_switches.h"
#include "tests/shared/renderer/client_app_renderer.h"

namespace client {
namespace {

// Process type enumeration (independent of CEF)
enum SimpleProcessType {
  PROCESS_TYPE_BROWSER = 0,
  PROCESS_TYPE_RENDERER = 1,
  PROCESS_TYPE_OTHER = 2,
};

// Determine process type from raw command line without CEF dependency
SimpleProcessType GetProcessTypeFromCommandLine(const char* command_line) {
  if (!command_line) {
    return PROCESS_TYPE_BROWSER;
  }

  // Look for --type= switch in command line
  const char* type_switch = strstr(command_line, "--type=");
  if (!type_switch) {
    return PROCESS_TYPE_BROWSER;
  }

  // Move past "--type="
  type_switch += 7;

  // Check for renderer process
  if (strncmp(type_switch, "renderer", 8) == 0) {
    return PROCESS_TYPE_RENDERER;
  }

  return PROCESS_TYPE_OTHER;
}

// Configure code signing requirements. For a code signing example see
// https://github.com/chromiumembedded/cef/issues/3824#issuecomment-2892139995

// TODO(client): Optionally require that the primary certificate match a
// specific thumbprint by setting this value to the SHA1 hash (e.g. 40 character
// upper-case hex-encoded value). If this valus is empty and |kAllowUnsigned| is
// false then any valid signature will be allowed. This is the "Thumbprint"
// output reported by some Windows PowerShell commands. It can also be retrieved
// directly with a PowerShell command like: > (Get-ChildItem
// Cert:\CurrentUser\My -CodeSigningCert)[0].Thumbprint
constexpr char kRequiredThumbprint[] = "";

// TODO(client): Optionally disallow unsigned binaries by setting this value to
// false. This value is disregarded if |kRequiredThumbprint| is specified.
constexpr bool kAllowUnsigned = true;

// TODO(client): Optionally require that all binaries be signed with the same
// primary thumbprint. This value is ignored when |kRequiredThumbprint| is
// specified or if |kAllowUnsigned| is true.
constexpr bool kRequireMatchingThumbprints = false;

static_assert(sizeof(kRequiredThumbprint) == 1 ||
                  sizeof(kRequiredThumbprint) ==
                      cef_certificate_util::kThumbprintLength + 1,
              "invalid size for kRequiredThumbprint");

const char* RequiredThumbprint(std::string* exe_thumbprint) {
  if constexpr (sizeof(kRequiredThumbprint) ==
                cef_certificate_util::kThumbprintLength + 1) {
    return kRequiredThumbprint;
  }

  if (!kAllowUnsigned && kRequireMatchingThumbprints && exe_thumbprint &&
      exe_thumbprint->length() == cef_certificate_util::kThumbprintLength) {
    return exe_thumbprint->c_str();
  }

  return nullptr;
}

bool VerifyCodeSigningAndLoad(CefScopedLibraryLoader& library_loader,
                              cef_version_info_t* version_info) {
  // Enable early logging support (required before libcef is loaded).
  // The *Assert() calls below will output a FATAL error and crash on failure.
  cef::logging::ScopedEarlySupport::Config config = {
      cef::logging::LOG_WARNING,  // min_log_level
      0,                         // vlog_level
      "[HostCLR]",              // log_prefix
      true,                      // log_process_id
      true,                      // log_thread_id
      true,                      // log_timestamp
      false,                     // log_tickcount
      nullptr                    // formatted_log_handler
  };
  cef::logging::ScopedEarlySupport scoped_logging(config);

  if (library_loader.LoadInSubProcessAssert(version_info)) {
    // Running as a sub-process. We may be sandboxed. Nothing more to be done.
    return true;
  }

  std::string exe_thumbprint;

  // Check signatures for the already loaded executable. This may be the
  // bootstrap, or the client executable if not using the bootstrap.
  const std::wstring& exe_path = cef_util::GetExePath();
  cef_certificate_util::ThumbprintsInfo exe_info;
  cef_certificate_util::ValidateCodeSigningAssert(
      exe_path, RequiredThumbprint(nullptr), kAllowUnsigned, &exe_info);
  if (exe_info.IsSignedAndValid()) {
    exe_thumbprint = exe_info.valid_thumbprints[0];
    CHECK_EQ(cef_certificate_util::kThumbprintLength, exe_thumbprint.length());
  }

#if defined(CEF_USE_BOOTSTRAP)
  // Using a separate bootstrap executable that loaded a client DLL. Check
  // signatures for the already loaded client DLL.
  const std::wstring& client_dll_path =
      cef_util::GetModulePath(client::GetCodeModuleHandle());
  cef_certificate_util::ValidateCodeSigningAssert(
      client_dll_path, RequiredThumbprint(&exe_thumbprint), kAllowUnsigned);
#endif  // defined(CEF_USE_BOOTSTRAP)

  // Require libcef.dll in the same directory as the executable.
  auto sep_pos = exe_path.find_last_of(L"/\\");
  CHECK(sep_pos != std::wstring::npos);
  const auto& libcef_dll_path = exe_path.substr(0, sep_pos + 1) + L"libcef.dll";

  // Validate code signing requirements for libcef.dll before loading, and
  // then load.
  return library_loader.LoadInMainAssert(libcef_dll_path.c_str(),
                                         RequiredThumbprint(&exe_thumbprint),
                                         kAllowUnsigned, version_info);
}

int RunMain(HINSTANCE hInstance,
            int nCmdShow,
            void* sandbox_info,
            cef_version_info_t* version_info) {
  SimpleProcessType simple_process_type = PROCESS_TYPE_BROWSER;
  // Scope block for ScopedEarlySupport - must end before CEF library is loaded,
  // otherwise all LOG() calls will go through ScopedEarlySupport (stderr) instead
  // of cef_log (debug.log file) after CEF initialization.
  {
  cef::logging::ScopedEarlySupport::Config config = {
      cef::logging::LOG_WARNING,  // min_log_level
      0,                         // vlog_level
      "[HostCLR]",              // log_prefix
      true,                      // log_process_id
      true,                      // log_thread_id
      true,                      // log_timestamp
      false,                     // log_tickcount
      nullptr                    // formatted_log_handler
  };
  cef::logging::ScopedEarlySupport scoped_logging(config);

  // Get wide char command line and convert to UTF-8 (before CEF is loaded)
  const wchar_t* raw_command_line_w = ::GetCommandLineW();
  std::string raw_command_line_utf8 = WideStringToUtf8(raw_command_line_w);

  // Determine process type from UTF-8 command line
  simple_process_type = GetProcessTypeFromCommandLine(raw_command_line_utf8.c_str());

  // Load hostfxr and initialize CLR before loading CEF
  int r = 0;
  int detailed_rc = 0;
  const int max_attempts = 5;
  const DWORD fixed_ms = 3000;
  const DWORD delta_ms = 6000;

  std::string exeLastDirName = GetExeLastDirName();
  bool is_debug = (exeLastDirName == "cefclientdbg");
  for (int attempt = 1; attempt <= max_attempts; ++attempt) {
    r = load_hostfxr(is_debug, detailed_rc);
    if (r == 0) {
      break;
    }

    printf_log(LOG_SEVERITY_ERROR, "Failed to load hostfxr: %d, detailed_rc: %d, attempt: %d/%d, process_type: %d",
             r, detailed_rc, attempt, max_attempts, static_cast<int>(simple_process_type));

    if (attempt < max_attempts) {
      const DWORD delay_ms = fixed_ms + (::GetTickCount() % delta_ms);
      ::Sleep(delay_ms);
    }
  }

  if (r != 0) {
    // Only show error in browser process to avoid crashes in sub-processes
    if (simple_process_type == PROCESS_TYPE_BROWSER) {
      printf_log(LOG_SEVERITY_ERROR, "CLR initialization failed after %d attempts. "
                      "Return code: %d, Detailed error code: %d (0x%08X). "
                      "Please check .NET runtime installation and restart later.",
              max_attempts, r, detailed_rc, static_cast<unsigned int>(detailed_rc));
    }
    return 1;  // Exit with error code before CEF initialization
  }

  // Load .NET methods
  r = load_dotnet_method(is_debug, detailed_rc);
  if (r != 0) {
    printf_log(LOG_SEVERITY_ERROR, "Failed to load dotnet method: %d, detailed_rc: %d, process_type: %d",
             r, detailed_rc, static_cast<int>(simple_process_type));
    return 1;
  }

  // Call on_init callback with UTF-8 strings
  if (on_init_fptr) {
    std::string baseDir = GetExeDir();
    std::string appDir = GetExeDir();
    std::string lastDirName = GetExeLastDirName();
    if (lastDirName == "cefclientdbg") {
      baseDir += "/../cefclient";
    }
    on_init_fptr(raw_command_line_utf8.c_str(), baseDir.c_str(), static_cast<int>(simple_process_type), appDir.c_str(), false);
  }

  }  // End of ScopedEarlySupport scope - LOG() will now use cef_log after CEF loads.

  // Now load CEF library after CLR initialization succeeded
  CefMainArgs main_args(hInstance);

  // Dynamically load the CEF library after code signing verification.
  CefScopedLibraryLoader library_loader;
  if (!VerifyCodeSigningAndLoad(library_loader, version_info)) {
    // The verification or load failed. We'll crash before reaching this line.
    if (simple_process_type == PROCESS_TYPE_BROWSER) {
      ::MessageBoxW(nullptr, L"Failed to verify code signing or load libcef.dll", L"CEF Load Error", MB_OK | MB_ICONERROR);
    }
    return 1;
  }

  // The CEF library (libcef) is loaded at this point.

  // Parse command-line arguments.
  CefRefPtr<CefCommandLine> command_line = CefCommandLine::CreateCommandLine();
  command_line->InitFromString(::GetCommandLineW());

  // Create a ClientApp of the correct type.
  CefRefPtr<CefApp> app;
  ClientApp::ProcessType process_type = ClientApp::GetProcessType(command_line);
  if (process_type == ClientApp::BrowserProcess) {
    app = new ClientAppBrowser();
  } else if (process_type == ClientApp::RendererProcess) {
    app = new ClientAppRenderer();
  } else if (process_type == ClientApp::OtherProcess) {
    app = new ClientAppOther();
  }

  // Execute the secondary process, if any.
  int exit_code = CefExecuteProcess(main_args, app, sandbox_info);
  if (exit_code >= 0) {
    return exit_code;
  }

  // Create the main context object.
  auto context = std::make_unique<MainContextImpl>(command_line, true);

  CefSettings settings;

  if (!sandbox_info) {
    settings.no_sandbox = true;
  }

  // Populate the settings based on command line arguments.
  context->PopulateSettings(&settings);

  // Set log severity to INFO to enable all log levels (INFO, WARNING, ERROR, FATAL)
  // By default, only WARNING and above are written to the log file
  if (settings.log_severity == LOGSEVERITY_DEFAULT) {
    settings.log_severity = LOGSEVERITY_INFO;
  }

  // Set the ID for the ICON resource that will be loaded from the main
  // executable and used when creating default Chrome windows such as DevTools
  // and Task Manager. Only used with the Chrome runtime.
#if defined(CEF_USE_BOOTSTRAP)
  // Use the default icon from bootstrap.exe.
  settings.chrome_app_icon_id = 32512;  // IDI_APPLICATION
#else
  // Use the default icon from cefclient.exe.
  settings.chrome_app_icon_id = IDR_MAINFRAME;
#endif

  // Create the main message loop object.
  std::unique_ptr<MainMessageLoop> message_loop;
  if (settings.multi_threaded_message_loop) {
    message_loop = std::make_unique<MainMessageLoopMultithreadedWin>();
  } else if (settings.external_message_pump) {
    message_loop = MainMessageLoopExternalPump::Create();
  } else {
    message_loop = std::make_unique<MainMessageLoopStd>();
  }

  // Initialize the CEF browser process. May return false if initialization
  // fails or if early exit is desired (for example, due to process singleton
  // relaunch behavior).
  if (!context->Initialize(main_args, settings, app, sandbox_info)) {
    return CefGetExitCode();
  }

  // Register scheme handlers.
  test_runner::RegisterSchemeHandlers();

  auto window_config = std::make_unique<RootWindowConfig>();
  window_config->always_on_top =
      command_line->HasSwitch(switches::kAlwaysOnTop);
  window_config->with_osr =
      settings.windowless_rendering_enabled ? true : false;

  // Create the first window.
  context->GetRootWindowManager()->CreateRootWindow(std::move(window_config));

  // Run the message loop. This will block until Quit() is called by the
  // RootWindowManager after all windows have been destroyed.
  int result = message_loop->Run();

  // Shut down CEF.
  context->Shutdown();

  // Release objects in reverse order of creation.
  message_loop.reset();
  context.reset();

  return result;
}

}  // namespace
}  // namespace client

#if defined(CEF_USE_BOOTSTRAP)

// Entry point called by bootstrap.exe when built as a DLL.
CEF_BOOTSTRAP_EXPORT int RunWinMain(HINSTANCE hInstance,
                                    LPTSTR lpCmdLine,
                                    int nCmdShow,
                                    void* sandbox_info,
                                    cef_version_info_t* version_info) {
  int exit_code = client::RunMain(hInstance, nCmdShow, sandbox_info, version_info);
  if (on_finalize_fptr) {
    on_finalize_fptr();
  }
  cleanup_browser_ids();
  return exit_code;
}

#else  // !defined(CEF_USE_BOOTSTRAP)

// Program entry point function.
int APIENTRY wWinMain(HINSTANCE hInstance,
                      HINSTANCE hPrevInstance,
                      LPTSTR lpCmdLine,
                      int nCmdShow) {
  UNREFERENCED_PARAMETER(hPrevInstance);
  UNREFERENCED_PARAMETER(lpCmdLine);

#if defined(ARCH_CPU_32_BITS)
  // Run the main thread on 32-bit Windows using a fiber with the preferred 4MiB
  // stack size. This function must be called at the top of the executable entry
  // point function (`main()` or `wWinMain()`). It is used in combination with
  // the initial stack size of 0.5MiB configured via the `/STACK:0x80000` linker
  // flag on executable targets. This saves significant memory on threads (like
  // those in the Windows thread pool, and others) whose stack size can only be
  // controlled via the linker flag.
  int exit_code = CefRunWinMainWithPreferredStackSize(wWinMain, hInstance,
                                                      lpCmdLine, nCmdShow);
  if (exit_code >= 0) {
    // The fiber has completed so return here.
    return exit_code;
  }
#endif

  void* sandbox_info = nullptr;

#if defined(CEF_USE_SANDBOX)
  // Manage the life span of the sandbox information object. This is necessary
  // for sandbox support on Windows. See cef_sandbox_win.h for complete details.
  CefScopedSandboxInfo scoped_sandbox;
  sandbox_info = scoped_sandbox.sandbox_info();
#endif

  cef_version_info_t version_info = {};
  CEF_POPULATE_VERSION_INFO(&version_info);

  int exit_code = client::RunMain(hInstance, nCmdShow, sandbox_info, &version_info);
  if (on_finalize_fptr) {
    on_finalize_fptr();
  }
  cleanup_browser_ids();
  return exit_code;
}

#endif  // !defined(CEF_USE_BOOTSTRAP)
