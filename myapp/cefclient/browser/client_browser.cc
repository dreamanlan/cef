// Copyright (c) 2016 The Chromium Embedded Framework Authors. All rights
// reserved. Use of this source code is governed by a BSD-style license that
// can be found in the LICENSE file.

#include "myapp/cefclient/browser/client_browser.h"

#include "include/base/cef_logging.h"
#include "include/cef_command_line.h"
#include "include/cef_crash_util.h"
#include "include/cef_file_util.h"
#include "myapp/cefclient/browser/client_prefs.h"
#include "myapp/cefclient/browser/default_client_handler.h"
#include "myapp/cefclient/browser/main_context.h"
#include "myapp/cefclient/browser/root_window_manager.h"
#include "myapp/shared/common/client_switches.h"
#include "myapp/shared/common/client_app.h"
#include "myapp/cefclient/hostclr/HostCLR.h"

namespace client::browser {

namespace {

class ClientBrowserDelegate : public ClientAppBrowser::Delegate {
 public:
  ClientBrowserDelegate() = default;

  ClientBrowserDelegate(const ClientBrowserDelegate&) = delete;
  ClientBrowserDelegate& operator=(const ClientBrowserDelegate&) = delete;

  void OnRegisterCustomPreferences(
      CefRefPtr<client::ClientAppBrowser> app,
      cef_preferences_type_t type,
      CefRawPtr<CefPreferenceRegistrar> registrar) override {
    if (type == CEF_PREFERENCES_TYPE_GLOBAL) {
      // Register global preferences with default values.
      prefs::RegisterGlobalPreferences(registrar);
    }
  }

  void OnContextInitialized(CefRefPtr<ClientAppBrowser> app) override {
    if (CefCrashReportingEnabled()) {
      // Set some crash keys for testing purposes. Keys must be defined in the
      // "crash_reporter.cfg" file. See cef_crash_util.h for details.
      CefSetCrashKeyValue("testkey_small1", "value1_small_browser");
      CefSetCrashKeyValue("testkey_small2", "value2_small_browser");
      CefSetCrashKeyValue("testkey_medium1", "value1_medium_browser");
      CefSetCrashKeyValue("testkey_medium2", "value2_medium_browser");
      CefSetCrashKeyValue("testkey_large1", "value1_large_browser");
      CefSetCrashKeyValue("testkey_large2", "value2_large_browser");
    }

    const std::string& crl_sets_path =
        CefCommandLine::GetGlobalCommandLine()->GetSwitchValue(
            switches::kCRLSetsPath);
    if (!crl_sets_path.empty()) {
      // Load the CRLSets file from the specified path.
      CefLoadCRLSetsFile(crl_sets_path);
    }

    // Start heartbeat timer for browser process (process_type=0)
    StartHeartbeat(0);
  }

  void OnBeforeCommandLineProcessing(
      CefRefPtr<ClientAppBrowser> app,
      CefRefPtr<CefCommandLine> command_line) override {
    // Append Chromium command line parameters if touch events are enabled
    if (client::MainContext::Get()->TouchEventsEnabled()) {
      command_line->AppendSwitchWithValue("touch-events", "enabled");
    }

    // Call C# callback to allow DSL script to process command line
    if (on_before_command_line_processing_fptr) {
      int process_type = static_cast<int>(ClientApp::GetProcessType(command_line));
      on_before_command_line_processing_fptr(process_type, command_line.get());
    }
  }

  void OnBeforeChildProcessLaunch(
      CefRefPtr<ClientAppBrowser> app,
      CefRefPtr<CefCommandLine> command_line) override {
    // Call C# callback to allow DSL script to process child process command line
    if (on_before_child_process_launch_fptr) {
      int process_type = static_cast<int>(ClientApp::GetProcessType(command_line));
      on_before_child_process_launch_fptr(process_type, command_line.get());
    }
  }

  bool OnAlreadyRunningAppRelaunch(
      CefRefPtr<ClientAppBrowser> app,
      CefRefPtr<CefCommandLine> command_line,
      const CefString& current_directory) override {
    // Add logging for some common switches that the user may attempt to use.
    static const char* kIgnoredSwitches[] = {
        switches::kMultiThreadedMessageLoop,
        switches::kOffScreenRenderingEnabled,
        switches::kUseViews,
    };
    for (auto& kIgnoredSwitche : kIgnoredSwitches) {
      if (command_line->HasSwitch(kIgnoredSwitche)) {
        LOG(WARNING) << "The --" << kIgnoredSwitche
                     << " command-line switch is ignored on app relaunch.";
      }
    }

    // Call C# callback to allow DSL script to handle app relaunch
    if (on_already_running_app_relaunch_fptr) {
      std::string cur_dir = current_directory.ToString();
      if (on_already_running_app_relaunch_fptr(command_line.get(), cur_dir.c_str())) {
        return false;
      }
    }

    // Create a new root window based on |command_line|.
    auto config = std::make_unique<RootWindowConfig>(command_line->Copy());

    MainContext::Get()->GetRootWindowManager()->CreateRootWindow(
        std::move(config));

    // Relaunch was handled.
    return true;
  }

  CefRefPtr<CefClient> GetDefaultClient(
      CefRefPtr<ClientAppBrowser> app) override {
    // Default client handler for unmanaged browser windows. Used with
    // Chrome style only.
    LOG(INFO) << "Creating a chrome browser with the default client";
    return new DefaultClientHandler();
  }

 private:
  IMPLEMENT_REFCOUNTING(ClientBrowserDelegate);
};

}  // namespace

void CreateDelegates(ClientAppBrowser::DelegateSet& delegates) {
  delegates.insert(new ClientBrowserDelegate);
}

}  // namespace client::browser
