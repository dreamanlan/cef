// Copyright (c) 2013 The Chromium Embedded Framework Authors. All rights
// reserved. Use of this source code is governed by a BSD-style license that
// can be found in the LICENSE file.

#include "tests/cefclient/browser/cef_query_handler.h"

#include <string>
#include <vector>
#include <filesystem>
#include <thread>
#include <chrono>

#include "include/base/cef_callback.h"
#include "include/base/cef_logging.h"
#include "include/wrapper/cef_closure_task.h"
#include "include/cef_parser.h"
#include "tests/cefclient/browser/main_context.h"
#include "tests/cefclient/browser/root_window.h"
#include "tests/cefclient/browser/root_window_manager.h"
#include "tests/cefclient/browser/test_runner.h"
#include "tests/cefclient/hostclr/HostCLR.h"

namespace client::cef_query_handler {

namespace {

// Structure to hold hot reload request data
struct HotReloadRequest {
  std::vector<FileCopyInfo> files;
  std::string url;  // Target URL to load after hot reload (empty = use current URL)
  bool custom_process_killer = false;  // If true, use custom TerminateRenderProcess
};

// Convert a JSON string to a dictionary value.
static CefRefPtr<CefDictionaryValue> ParseJSON(const CefString& string) {
  CefRefPtr<CefValue> value = CefParseJSON(string, JSON_PARSER_RFC);
  if (value.get() && value->GetType() == VTYPE_DICTIONARY) {
    return value->GetDictionary();
  }
  return nullptr;
}

// Parse JSON request to extract hot reload data using CEF JSON parser.
HotReloadRequest ParseHotReloadRequest(const std::string& request) {
  HotReloadRequest reload_request;

  // Parse JSON using CEF parser (request is already the JSON string)
  CefRefPtr<CefDictionaryValue> request_dict = ParseJSON(request);
  if (!request_dict) {
    printf_log(LOG_SEVERITY_ERROR, "Hot reload: Failed to parse JSON request: %s", request.c_str());
    return reload_request;
  }

  // Verify the "files" key exists
  if (!request_dict->HasKey("files") ||
      request_dict->GetType("files") != VTYPE_LIST) {
    printf_log(LOG_SEVERITY_ERROR, "Hot reload: Missing or incorrectly formatted 'files' key");
    return reload_request;
  }

  // Get the files list
  CefRefPtr<CefListValue> files_list = request_dict->GetList("files");
  if (!files_list.get()) {
    printf_log(LOG_SEVERITY_ERROR, "Hot reload: Failed to get files list");
    return reload_request;
  }

  // Parse each file entry
  size_t files_count = files_list->GetSize();
  for (size_t i = 0; i < files_count; ++i) {
    if (files_list->GetType(i) != VTYPE_DICTIONARY) {
      printf_log(LOG_SEVERITY_INFO, "Hot reload: Invalid file entry at index %zu", i);
      continue;
    }

    CefRefPtr<CefDictionaryValue> file_entry = files_list->GetDictionary(i);
    if (!file_entry.get()) {
      printf_log(LOG_SEVERITY_INFO, "Hot reload: Failed to get file entry at index %zu", i);
      continue;
    }

    // Verify source and dest keys exist
    if (!file_entry->HasKey("source") || !file_entry->HasKey("dest")) {
      printf_log(LOG_SEVERITY_INFO, "Hot reload: Missing 'source' or 'dest' in file entry at index %zu", i);
      continue;
    }

    if (file_entry->GetType("source") != VTYPE_STRING ||
        file_entry->GetType("dest") != VTYPE_STRING) {
      printf_log(LOG_SEVERITY_INFO, "Hot reload: Incorrectly formatted 'source' or 'dest' in file entry at index %zu", i);
      continue;
    }

    FileCopyInfo info;
    info.source = file_entry->GetString("source").ToString();
    info.dest = file_entry->GetString("dest").ToString();

    if (!info.source.empty() && !info.dest.empty()) {
      reload_request.files.push_back(info);
    } else {
      printf_log(LOG_SEVERITY_INFO, "Hot reload: Empty source or dest path in file entry at index %zu", i);
    }
  }

  // Parse optional "url" field
  if (request_dict->HasKey("url") && request_dict->GetType("url") == VTYPE_STRING) {
    reload_request.url = request_dict->GetString("url").ToString();
  }

  // Parse optional "custom_process_killer" field
  if (request_dict->HasKey("custom_process_killer") &&
      request_dict->GetType("custom_process_killer") == VTYPE_BOOL) {
    reload_request.custom_process_killer =
        request_dict->GetBool("custom_process_killer");
    printf_log(LOG_SEVERITY_INFO,
              "Hot reload: custom_process_killer=%s",
              reload_request.custom_process_killer ? "true" : "false");
  }

  return reload_request;
}

// Copy file from source to destination with retry mechanism
bool CopyFile(const std::string& source, const std::string& dest) {
  const int kMaxRetries = 5;
  const int kRetryDelayMs = 200;

  for (int retry = 0; retry < kMaxRetries; ++retry) {
    std::error_code ec;

    // Check if source file exists
    if (!std::filesystem::exists(source, ec)) {
      if (ec) {
        printf_log(LOG_SEVERITY_ERROR, "Hot reload: Error checking source file: %s, error: %s", source.c_str(), ec.message().c_str());
      } else {
        printf_log(LOG_SEVERITY_ERROR, "Hot reload: Source file does not exist: %s", source.c_str());
      }
      return false;
    }

    // Try to copy the file
    std::filesystem::copy_file(source, dest,
                              std::filesystem::copy_options::overwrite_existing, ec);

    if (!ec) {
      // Success
      printf_log(LOG_SEVERITY_INFO, "Hot reload: Successfully copied %s to %s", source.c_str(), dest.c_str());
      return true;
    }

    // Copy failed
    printf_log(LOG_SEVERITY_INFO, "Hot reload: Copy attempt %d/%d failed: %s (code: %d)", retry + 1, kMaxRetries, ec.message().c_str(), ec.value());

    if (retry < kMaxRetries - 1) {
      // Wait before retry
      std::this_thread::sleep_for(std::chrono::milliseconds(kRetryDelayMs));
    } else {
      printf_log(LOG_SEVERITY_ERROR, "Hot reload: Failed to copy file after %d attempts: %s -> %s", kMaxRetries, source.c_str(), dest.c_str());
      return false;
    }
  }

  return false;
}

// Handle messages in the browser process for hot reload
class Handler : public CefMessageRouterBrowserSide::Handler {
 public:
  Handler() = default;

  bool OnQuery(CefRefPtr<CefBrowser> browser,
                CefRefPtr<CefFrame> frame,
                int64_t query_id,
                const CefString& request,
                bool persistent,
                CefRefPtr<Callback> callback) override {
    // We are the final handlers, responsible for processing all queries..
    printf_log(LOG_SEVERITY_INFO, "[CefQueryHandler] calling RunOnMainThread with request: %lld(%s) persistent:%d", query_id, request.ToString().c_str(), persistent);
    RunOnMainThread(browser, frame, query_id, request.ToString(), persistent, callback);
    return true;
  }

  private:
  static void RunOnMainThread(CefRefPtr<CefBrowser> browser,
                CefRefPtr<CefFrame> frame,
                int64_t query_id,
                const CefString& request,
                bool persistent,
                CefRefPtr<Callback> callback) {
    if (!CURRENTLY_ON_MAIN_THREAD()) {
      MAIN_POST_CLOSURE(base::BindOnce(&Handler::RunOnMainThread, browser, frame, query_id,
                                       request, persistent, callback));
      return;
    }

    if (!frame.get()) {
      frame = browser->GetMainFrame();
    }
    // Parse request as JSON
    CefRefPtr<CefDictionaryValue> request_dict = ParseJSON(request);
    if (request_dict &&
        request_dict->HasKey("action") &&
        request_dict->GetType("action") == VTYPE_STRING &&
        request_dict->GetString("action") == "hot_reload") {
      printf_log(LOG_SEVERITY_INFO, "[HotReload] Detected hot_reload action, %s", request.ToString().c_str());
      // Handle hot reload request
      auto root_window_manager = MainContext::Get()->GetRootWindowManager();

      // Parse hot reload request
      HotReloadRequest reload_request = ParseHotReloadRequest(request.ToString());

      if (reload_request.files.empty()) {
        printf_log(LOG_SEVERITY_ERROR, "Hot reload: No files specified in request");
        callback->Failure(-1, "ERROR: No files specified");
        return;
      }

      // If no URL specified, use current browser's URL
      if (reload_request.url.empty() && browser.get()) {
        if (frame.get()) {
          reload_request.url = frame->GetURL().ToString();
        }
      }

      printf_log(LOG_SEVERITY_INFO, "Hot reload: Starting with %zu file(s), URL: %s",
                reload_request.files.size(),
                (reload_request.url.empty() ? "[keep current]" : reload_request.url.c_str()));

      // Create copy callback
      auto copy_callback = base::BindOnce(&Handler::CopyFiles, reload_request);

      // Create completion callback
      auto completion_callback = base::BindOnce(&Handler::OnWindowCreated, callback);

      // Execute hot reload flow through RootWindowManager
      root_window_manager->ExecuteHotReload(reload_request.url,
                                          reload_request.files,
                                          std::move(copy_callback),
                                          std::move(completion_callback),
                                          reload_request.custom_process_killer);
      return;
    }

    // try to call C# handler
    if (on_browser_cef_query_fptr) {
      printf_log(LOG_SEVERITY_INFO, "[CefQueryHandler] try to call C# handler: %lld", query_id);
      int result = on_browser_cef_query_fptr(browser.get(), frame.get(), query_id, request.ToString().c_str(), persistent);
      if (result == 0) {
        callback->Success("OK");
      }
      else {
        callback->Failure(result, "ERROR: C# handler failed");
      }
    }
  }

  static void CopyFiles(const HotReloadRequest& reload_request) {
    printf_log(LOG_SEVERITY_INFO, "Hot reload: CopyFiles starting, %zu file(s) to copy", reload_request.files.size());

    // Call the hot reload copy files function
    bool skip_copy = false;
    if (on_browser_hot_reload_copyfiles_fptr) {
      skip_copy = on_browser_hot_reload_copyfiles_fptr(reload_request.url.c_str());
      printf_log(LOG_SEVERITY_INFO, "Hot reload: on_browser_hot_reload_copyfiles returned %s",
                skip_copy ? "true (skip copy)" : "false (execute copy)");
    } else {
      printf_log(LOG_SEVERITY_WARNING, "Hot reload: on_browser_hot_reload_copyfiles_fptr is null, proceeding with default copy");
    }

    if (!skip_copy) {
      int success_count = 0;
      int failure_count = 0;

      for (const auto& file : reload_request.files) {
        printf_log(LOG_SEVERITY_INFO, "Hot reload: Copying file: %s -> %s", file.source.c_str(), file.dest.c_str());

        if (CopyFile(file.source, file.dest)) {
          success_count++;
        } else {
          failure_count++;
          printf_log(LOG_SEVERITY_ERROR, "Hot reload: Failed to copy file: %s", file.source.c_str());
        }
      }

      printf_log(LOG_SEVERITY_INFO, "Hot reload: CopyFiles completed. Success: %d, Failed: %d", success_count, failure_count);
    }
  }

  static void OnWindowCreated(CefRefPtr<Callback> callback,
                              scoped_refptr<RootWindow> root_window) {
    printf_log(LOG_SEVERITY_INFO, "Hot reload: Window created, calling C# hot reload callback");

    if (!root_window.get()) {
      printf_log(LOG_SEVERITY_INFO, "Hot reload: root_window is null");
      callback->Success("WARNING: Failed to create window");
      return;
    }

    CefRefPtr<CefBrowser> browser = root_window->GetBrowser();
    if (!browser.get()) {
      printf_log(LOG_SEVERITY_INFO, "Hot reload: browser is not ready");
      callback->Success("WARNING: Browser not ready");
      return;
    }

    // Get current URL
    std::string url;
    CefRefPtr<CefFrame> frame = browser->GetMainFrame();
    if (frame.get()) {
      url = frame->GetURL().ToString();
    }

    // Call C# hot reload callback
    if (on_browser_hot_reload_completed_fptr) {
      on_browser_hot_reload_completed_fptr(browser.get(), frame.get(), url.c_str());
      callback->Success("OK");
    } else {
      printf_log(LOG_SEVERITY_INFO, "Hot reload: on_browser_hot_reload_completed_fptr is null");
      callback->Success("WARNING: C# callback not available");
    }
  }

  DISALLOW_COPY_AND_ASSIGN(Handler);
};

}  // namespace

void CreateMessageHandlers(test_runner::MessageHandlerSet& handlers) {
  handlers.insert(new Handler());
}

}  // namespace client::cef_query_handler
