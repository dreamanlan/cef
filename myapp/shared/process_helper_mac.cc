// Copyright (c) 2012 The Chromium Embedded Framework Authors. All rights
// reserved. Use of this source code is governed by a BSD-style license that can
// be found in the LICENSE file.

#include "include/cef_app.h"
#include "include/base/cef_logging.h"
#include "include/wrapper/cef_library_loader.h"
#include "myapp/cefclient/hostclr/HostCLR.h"
#include "myapp/shared/common/client_app_other.h"
#include "myapp/shared/renderer/client_app_renderer.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

// When generating projects with CMake the CEF_USE_SANDBOX value will be defined
// automatically. Pass -DUSE_SANDBOX=OFF to the CMake command-line to disable
// use of the sandbox.
#if defined(CEF_USE_SANDBOX)
#include "include/cef_sandbox_mac.h"
#endif

namespace client {

int RunMain(int argc, char* argv[]) {
  // Initialize CLR before sandbox and CEF framework loading.
  // CLR initialization must happen before sandbox because the sandbox restricts
  // file system access to /usr/local/share/dotnet/ and /etc/dotnet/ which are
  // needed by get_hostfxr_path() to locate the .NET runtime.
  // Scope block for ScopedEarlySupport - must end before CEF library is loaded.
  {
    cef::logging::ScopedEarlySupport::Config config = {
        cef::logging::LOG_WARNING,  // min_log_level
        0,                         // vlog_level
        "[HostCLR-Helper]",       // log_prefix
        true,                      // log_process_id
        true,                      // log_thread_id
        true,                      // log_timestamp
        false,                     // log_tickcount
        nullptr                    // formatted_log_handler
    };
    cef::logging::ScopedEarlySupport scoped_logging(config);

    // Get command line as UTF-8 string
    std::string raw_command_line_utf8;
    for (int i = 0; i < argc; ++i) {
      if (i > 0) {
        raw_command_line_utf8 += " ";
      }
      raw_command_line_utf8 += argv[i];
    }

    // Determine process type from command line
    int clr_process_type = 2;  // Default to "other" for helper processes
    for (int i = 1; i < argc; ++i) {
      if (strncmp(argv[i], "--type=", 7) == 0) {
        const char* type_value = argv[i] + 7;
        if (strncmp(type_value, "renderer", 8) == 0) {
          clr_process_type = 1;  // renderer
        } else {
          clr_process_type = 2;  // other
        }
        break;
      }
    }

    // Load hostfxr and initialize CLR
    int r = 0;
    int detailed_rc = 0;
    const int max_attempts = 5;
    const unsigned int fixed_ms = 3000;
    const unsigned int delta_ms = 6000;

    std::string appDirName = GetMacMainAppDirName();
    bool is_debug = (appDirName == "webagentdbg.app");

    for (int attempt = 1; attempt <= max_attempts; ++attempt) {
      r = load_hostfxr(is_debug, detailed_rc);
      if (r == 0) {
        break;
      }

      printf_log(LOG_SEVERITY_ERROR,
                 "Helper: Failed to load hostfxr: %d, detailed_rc: %d, "
                 "attempt: %d/%d, process_type: %d",
                 r, detailed_rc, attempt, max_attempts, clr_process_type);

      if (attempt < max_attempts) {
        const unsigned int delay_ms = fixed_ms + (rand() % delta_ms);
        usleep(delay_ms * 1000);
      }
    }

    if (r != 0) {
      printf_log(LOG_SEVERITY_ERROR,
                 "Helper: CLR initialization failed after %d attempts. "
                 "Return code: %d, Detailed error code: %d (0x%08X).",
                 max_attempts, r, detailed_rc,
                 static_cast<unsigned int>(detailed_rc));
      // Don't return error for helper processes - let them continue without CLR
      // so CEF can still function (just without C# callbacks)
    } else {
      // Load .NET methods
      r = load_dotnet_method(is_debug, detailed_rc);
      if (r != 0) {
        printf_log(LOG_SEVERITY_ERROR,
                   "Helper: Failed to load dotnet method: %d, detailed_rc: %d, "
                   "process_type: %d",
                   r, detailed_rc, clr_process_type);
      } else {
        // Call on_init callback
        if (on_init_fptr) {
          std::string mainAppDir = GetMacMainAppDirPath();
          std::string appDir = GetMacMainAppDirPath();
          std::string mainAppDirName = GetMacMainAppDirName();
          std::string baseDir;
          if (mainAppDirName == "webagentdbg.app") {
            baseDir = mainAppDir + "/../webagent.app/Contents";
          } else {
            baseDir = mainAppDir + "/Contents";
          }
          on_init_fptr(raw_command_line_utf8.c_str(), baseDir.c_str(),
                       clr_process_type, appDir.c_str(), true);
        }
      }
    }
  }  // End of ScopedEarlySupport scope

#if defined(CEF_USE_SANDBOX)
  // Initialize the macOS sandbox for this helper process.
  // This must happen after CLR initialization because the sandbox restricts
  // access to /usr/local/share/dotnet/ and /etc/dotnet/ directories.
  CefScopedSandboxContext sandbox_context;
  if (!sandbox_context.Initialize(argc, argv)) {
    return 1;
  }
#endif

  // Load the CEF framework library at runtime instead of linking directly
  // as required by the macOS sandbox implementation.
  CefScopedLibraryLoader library_loader;
  if (!library_loader.LoadInHelper()) {
    return 1;
  }

  CefMainArgs main_args(argc, argv);

  // Parse command-line arguments.
  CefRefPtr<CefCommandLine> command_line = CefCommandLine::CreateCommandLine();
  command_line->InitFromArgv(argc, argv);

  // Create a ClientApp of the correct type.
  CefRefPtr<CefApp> app;
  ClientApp::ProcessType process_type = ClientApp::GetProcessType(command_line);
  if (process_type == ClientApp::RendererProcess) {
    app = new ClientAppRenderer();
  } else if (process_type == ClientApp::OtherProcess) {
    app = new ClientAppOther();
  }

  // Execute the secondary process.
  int result = CefExecuteProcess(main_args, app, nullptr);

  // Call on_finalize callback for helper processes
  if (on_finalize_fptr) {
    on_finalize_fptr();
  }

  return result;
}

}  // namespace client

// Entry point function for sub-processes.
int main(int argc, char* argv[]) {
  return client::RunMain(argc, argv);
}
