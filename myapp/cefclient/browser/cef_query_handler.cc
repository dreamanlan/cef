// Copyright (c) 2013 The Chromium Embedded Framework Authors. All rights
// reserved. Use of this source code is governed by a BSD-style license that
// can be found in the LICENSE file.

#include "myapp/cefclient/browser/cef_query_handler.h"

#include <string>
#include <vector>
#include <filesystem>
#include <thread>
#include <chrono>
#include <map>
#include <mutex>

#include "include/base/cef_callback.h"
#include "include/base/cef_logging.h"
#include "include/wrapper/cef_closure_task.h"
#include "include/cef_parser.h"
#include "include/cef_task.h"
#include "myapp/cefclient/browser/main_context.h"
#include "myapp/cefclient/browser/default_client_handler.h"
#include "myapp/cefclient/browser/root_window.h"
#include "myapp/cefclient/browser/root_window_manager.h"
#include "myapp/cefclient/browser/test_runner.h"
#include "myapp/cefclient/browser/views_window.h"
#include "myapp/cefclient/hostclr/HostCLR.h"
#include "myapp/cefclient/hostclr/native_callbacks.h"

namespace client::cef_query_handler {

namespace {

// Structure to hold hot reload request data
struct HotReloadRequest {
  std::vector<FileCopyInfo> files;
  std::string url;  // Target URL to load after hot reload (empty = use current URL)
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

// Maps a pending query to the native callback handle parked for it. Needed by
// OnQueryCanceled: once CEF cancels a query the Callback object must be dropped
// without executing any of its methods, so the entry has to be discarded
// instead of completed. Static (not a Handler member) so lookups stay valid
// regardless of Handler lifetime; guarded because completion may run on any
// browser process thread.
std::mutex& GetPendingQueryMutex() {
  static auto* s_m = new std::mutex();
  return *s_m;
}

std::map<int64_t, int64_t>& GetPendingQueryMap() {
  static auto* s_map = new std::map<int64_t, int64_t>();
  return *s_map;
}

void TrackPendingQuery(int64_t query_id, int64_t handle) {
  std::lock_guard<std::mutex> lock(GetPendingQueryMutex());
  GetPendingQueryMap()[query_id] = handle;
}

// Returns the handle and forgets the query, or 0 when it is already gone.
int64_t UntrackPendingQuery(int64_t query_id) {
  std::lock_guard<std::mutex> lock(GetPendingQueryMutex());
  auto& m = GetPendingQueryMap();
  auto it = m.find(query_id);
  if (it == m.end()) {
    return 0;
  }
  const int64_t handle = it->second;
  m.erase(it);
  return handle;
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

  void OnQueryCanceled(CefRefPtr<CefBrowser> browser,
                       CefRefPtr<CefFrame> frame,
                       int64_t query_id) override {
    // The Callback object must not be used any more, so drop the parked entry
    // without running it. Discard is idempotent, so an already completed query
    // is harmless here.
    const int64_t handle = UntrackPendingQuery(query_id);
    if (handle != 0) {
      printf_log(LOG_SEVERITY_INFO,
                "[CefQueryHandler] query %lld canceled, discarding handle %lld",
                query_id, handle);
      DiscardNativeCallback(handle);
      // Pure notification so managed code can drop its state for the dead
      // handle (e.g. stop a subscription feed); it must not try to complete it.
      if (on_browser_cef_query_canceled_fptr) {
        on_browser_cef_query_canceled_fptr(browser.get(), frame.get(),
                                           query_id, handle);
      }
    }
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

    // HTML tab bar strip messages ({ "channel": "tabbar", ... }) are handled
    // entirely in C++ (no DSL/C# hop): height reports drive the docked strip's
    // layout; navigation actions drive the content browser. See §8 of
    // TABBAR_DESIGN.md.
    if (request_dict &&
        request_dict->HasKey("channel") &&
        request_dict->GetType("channel") == VTYPE_STRING &&
        request_dict->GetString("channel") == "tabbar") {
      HandleTabbarQuery(browser, request_dict, callback);
      return;
    }

    // Native file dialog actions handled entirely in C++ (no DSL/C# hop).
    // Payload:
    //   { "action": "show_file_dialog",
    //     "mode":   "open" | "open_multiple" | "open_folder" | "save",
    //     "title":         "..."      (optional)
    //     "default_path":  "..."      (optional)
    //     "accept_filters": ["*.txt", ".png", "image/*"]  (optional; ignored
    //                                                     for open_folder)
    //   }
    // Response: JSON string
    //   { "canceled": true }  or
    //   { "canceled": false, "paths": ["..."] }
    if (request_dict &&
        request_dict->HasKey("action") &&
        request_dict->GetType("action") == VTYPE_STRING &&
        request_dict->GetString("action") == "show_file_dialog") {
      HandleShowFileDialog(browser, request_dict, callback);
      return;
    }

    if (request_dict &&
        request_dict->HasKey("action") &&
        request_dict->GetType("action") == VTYPE_STRING &&
        request_dict->GetString("action") == "hot_reload") {
      printf_log(LOG_SEVERITY_INFO, "[HotReload] Detected hot_reload action, %s", request.ToString().c_str());
      // Handle hot reload request
      auto root_window_manager = MainContext::Get()->GetRootWindowManager();

      // Parse hot reload request
      HotReloadRequest reload_request = ParseHotReloadRequest(request.ToString());

      // An empty file list is allowed: it means "restart only, nothing to copy".
      // CopyFiles() still runs the C# before_restart callback (the page is down
      // either way, so it is a valid moment for managed code to act) - only the
      // default copy loop has nothing to do.
      if (reload_request.files.empty()) {
        printf_log(LOG_SEVERITY_INFO, "Hot reload: No files specified in request, reload only");
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
                                          std::move(completion_callback));
      return;
    }

    // try to call C# handler
    if (on_browser_cef_query_fptr) {
      printf_log(LOG_SEVERITY_INFO, "[CefQueryHandler] try to call C# handler: %lld", query_id);

      // Park the callback before calling managed code so the query can be
      // answered asynchronously. Callback methods may be invoked on any browser
      // process thread, but the handler itself runs on the main thread, so keep
      // the completion there for consistency.
      // A persistent query keeps its Callback across Success calls (CEF runs
      // the page's onSuccess once per push), so it gets a persistent registry
      // entry without timeout; ok=false (Failure) cancels it per CEF rules. A
      // non-persistent query is one-shot and expires after kCefQueryTimeoutMs.
      const int browser_id = browser.get() ? browser->GetIdentifier() : 0;
      const auto park = [callback, query_id, persistent](
                            bool ok, const std::string& response,
                            int error_code) {
        if (!callback) {
          UntrackPendingQuery(query_id);
          return;
        }
        if (ok) {
          callback->Success(CefString(response));
          // Success spends a one-shot Callback; a persistent query stays
          // registered, so keep it tracked for a later OnQueryCanceled and for
          // further complete_native_callback pushes.
          if (!persistent) {
            UntrackPendingQuery(query_id);
          }
          return;
        }
        // Failure cancels the query in both modes. A registry timeout or a
        // browser-close cancel arrives with an empty payload; give the page
        // something diagnosable.
        UntrackPendingQuery(query_id);
        callback->Failure(error_code,
                          response.empty()
                              ? CefString("ERROR: query canceled or timed out")
                              : CefString(response));
      };
      const int64_t handle =
          persistent
              ? RegisterPersistentNativeCallback(browser_id, TID_UI, park)
              : RegisterNativeCallback(browser_id, TID_UI, park,
                                       kCefQueryTimeoutMs);
      TrackPendingQuery(query_id, handle);

      int out_result = 0;
      if (on_browser_cef_query_fptr(browser.get(), frame.get(), query_id,
                                    request.ToString().c_str(), persistent,
                                    handle, out_result)) {
        // Taken over: managed code owns the query and must complete |handle|
        // (persistent queries: ok=true pushes may repeat, ok=false cancels).
        // Safety nets when it never does: OnQueryCanceled discards the entry on
        // navigation / renderer termination / window.cefQueryCancel, a
        // non-persistent entry expires after kCefQueryTimeoutMs, and
        // BaseClientHandler::OnBeforeClose cancels everything the browser owns.
        return;
      }
      // Handled synchronously (also the degraded path when managed code fails,
      // since false is the default return value): answer right away.
      UntrackPendingQuery(query_id);
      DiscardNativeCallback(handle);
      if (out_result == 0) {
        callback->Success("OK");
      }
      else {
        callback->Failure(out_result, "ERROR: C# handler failed");
      }
      return;
    }

    // No managed handler at all: answer so the page's onFailure runs instead of
    // the query hanging forever.
    printf_log(LOG_SEVERITY_WARNING,
              "[CefQueryHandler] no managed handler for query %lld", query_id);
    callback->Failure(-1, "ERROR: no handler");
  }

  // CEF callback that packages RunFileDialog's result into a JSON response and
  // resolves the pending window.cefQuery promise on the JS side. Stored as a
  // CefRefPtr so RunFileDialog can own it until the dialog is dismissed.
  class FileDialogCb : public CefRunFileDialogCallback {
   public:
    explicit FileDialogCb(CefRefPtr<Callback> callback)
        : callback_(callback) {}

    void OnFileDialogDismissed(
        const std::vector<CefString>& file_paths) override {
      CefRefPtr<CefDictionaryValue> resp = CefDictionaryValue::Create();
      if (file_paths.empty()) {
        resp->SetBool("canceled", true);
      } else {
        resp->SetBool("canceled", false);
        CefRefPtr<CefListValue> paths = CefListValue::Create();
        paths->SetSize(file_paths.size());
        for (size_t i = 0; i < file_paths.size(); ++i) {
          paths->SetString(i, file_paths[i]);
        }
        resp->SetList("paths", paths);
      }
      CefRefPtr<CefValue> v = CefValue::Create();
      v->SetDictionary(resp);
      callback_->Success(CefWriteJSON(v, JSON_WRITER_DEFAULT));
    }

   private:
    CefRefPtr<Callback> callback_;
    IMPLEMENT_REFCOUNTING(FileDialogCb);
    DISALLOW_COPY_AND_ASSIGN(FileDialogCb);
  };

  // Resolve the ViewsWindow that owns the tab bar strip |browser|, then apply
  // the requested action (height report -> relayout; navigation -> content
  // browser). Runs on the main (UI) thread.
  static void HandleTabbarQuery(CefRefPtr<CefBrowser> browser,
                                CefRefPtr<CefDictionaryValue> request_dict,
                                CefRefPtr<Callback> callback) {
    // The tab bar owner and all Views operations belong to the UI thread.
    if (!CefCurrentlyOn(TID_UI)) {
      CefPostTask(TID_UI, base::BindOnce(&Handler::HandleTabbarQuery,
                                       browser, request_dict, callback));
      return;
    }

    ViewsWindow* window = nullptr;
    if (browser && browser->GetHost()) {
      CefRefPtr<DefaultClientHandler> handler =
          DefaultClientHandler::GetForClient(browser->GetHost()->GetClient());
      if (handler) {
        window = handler->GetTabbarOwnerWindow();
      }
    }
    if (!window) {
      callback->Failure(-1, "ERROR: no tab bar owner window");
      return;
    }

    std::string action;
    if (request_dict->HasKey("action") &&
        request_dict->GetType("action") == VTYPE_STRING) {
      action = request_dict->GetString("action").ToString();
    }

    if ((action == "newtab" || action == "closetab" ||
         action == "detachtab") &&
        !window->SupportsMultipleTabs()) {
      callback->Failure(-1, "ERROR: tab creation and removal are disabled");
      return;
    }

    if (action == "resize") {
      int height = 0;
      if (request_dict->HasKey("height")) {
        const cef_value_type_t t = request_dict->GetType("height");
        if (t == VTYPE_INT) {
          height = request_dict->GetInt("height");
        } else if (t == VTYPE_DOUBLE) {
          height = static_cast<int>(request_dict->GetDouble("height") + 0.5);
        }
      }
      if (height > 0) {
        window->SetTabbarHeight(height);
      }
      callback->Success("OK");
      return;
    }

    if (action == "ready") {
      if (window->RequestTabSnapshot()) {
        callback->Success("OK");
      } else {
        callback->Failure(-1, "ERROR: tab snapshot request was not accepted");
      }
      return;
    }

    if (action == "back" || action == "forward" || action == "reload" ||
        action == "reload_nocache" || action == "stop" ||
        action == "navigate") {
      std::string url;
      if (request_dict->HasKey("url") &&
          request_dict->GetType("url") == VTYPE_STRING) {
        url = request_dict->GetString("url").ToString();
      }
      window->ExecuteTabbarCommand(action, url);
      callback->Success("OK");
      return;
    }

    if (action == "newtab") {
      std::string url;
      if (request_dict->HasKey("url") &&
          request_dict->GetType("url") == VTYPE_STRING) {
        url = request_dict->GetString("url").ToString();
      }
      if (window->RequestNewTab(url)) {
        callback->Success("OK");
      } else {
        callback->Failure(-1, "ERROR: new tab request was not accepted");
      }
      return;
    }

    if (action == "reordertab") {
      if (request_dict->GetType("id") != VTYPE_INT ||
          request_dict->GetInt("id") <= 0 ||
          request_dict->GetType("beforeId") != VTYPE_INT ||
          request_dict->GetInt("beforeId") < 0) {
        callback->Failure(-1, "ERROR: native id and beforeId are required");
        return;
      }
      if (window->RequestTabReorder(request_dict->GetInt("id"),
                                   request_dict->GetInt("beforeId"))) {
        callback->Success("OK");
      } else {
        callback->Failure(-1, "ERROR: tab reorder was not accepted");
      }
      return;
    }

    if (action == "detachtab") {
      if (request_dict->GetType("id") != VTYPE_INT ||
          request_dict->GetInt("id") <= 0) {
        callback->Failure(-1, "ERROR: a native content browser id is required");
        return;
      }
      if (window->RequestTabDetach(request_dict->GetInt("id"))) {
        callback->Success("OK");
      } else {
        callback->Failure(-1, "ERROR: tab detach was not accepted");
      }
      return;
    }

    if (action == "dragwindow") {
      // Begin the shell drag. |merge| carries the gesture semantics: tab
      // drags (default) may merge into another tab bar, blank-area window
      // moves may not (Chrome parity, M4b).
      bool allow_merge = true;
      if (request_dict->HasKey("merge")) {
        const cef_value_type_t t = request_dict->GetType("merge");
        if (t == VTYPE_BOOL) {
          allow_merge = request_dict->GetBool("merge");
        } else if (t == VTYPE_INT) {
          allow_merge = request_dict->GetInt("merge") != 0;
        }
      }
      if (window->RequestWindowDrag(allow_merge)) {
        callback->Success("OK");
      } else {
        callback->Failure(-1, "ERROR: window drag was not accepted");
      }
      return;
    }

    if (action == "togglemaximize") {
      // Strip double-click affordance for the JS-driven whole-window drag
      // (M4b, Windows).
      window->ToggleMaximize();
      callback->Success("OK");
      return;
    }

    if (action == "windowdragmove" || action == "windowdragend") {
      // Linux JS-fed drag loop: the page keeps receiving mousemove during the
      // implicit X pointer grab and signals each frame / the release.
      if (action == "windowdragmove") {
        window->NotifyTabDragMove();
      } else {
        bool cancel = false;
        if (request_dict->HasKey("cancel")) {
          const cef_value_type_t t = request_dict->GetType("cancel");
          if (t == VTYPE_BOOL) {
            cancel = request_dict->GetBool("cancel");
          } else if (t == VTYPE_INT) {
            cancel = request_dict->GetInt("cancel") != 0;
          }
        }
        window->NotifyTabDragEnd(cancel);
      }
      callback->Success("OK");
      return;
    }

    if (action == "drophover") {
      // Insertion slot reported by the hovered tab bar during a drag session.
      int before_id = 0;
      if (request_dict->HasKey("beforeId")) {
        const cef_value_type_t t = request_dict->GetType("beforeId");
        if (t == VTYPE_INT) {
          before_id = request_dict->GetInt("beforeId");
        } else if (t == VTYPE_DOUBLE) {
          before_id = static_cast<int>(request_dict->GetDouble("beforeId") +
                                       0.5);
        }
      }
      window->ReportTabDropHover(browser, before_id > 0 ? before_id : 0);
      callback->Success("OK");
      return;
    }

    if (action == "closetab" || action == "selecttab") {
      if (request_dict->GetType("id") != VTYPE_INT ||
          request_dict->GetInt("id") <= 0) {
        callback->Failure(-1, "ERROR: a native content browser id is required");
        return;
      }
      if (window->RequestTabCommand(action, request_dict->GetInt("id"))) {
        callback->Success("OK");
      } else {
        callback->Failure(-1, "ERROR: tab command was not accepted");
      }
      return;
    }

    callback->Failure(-1, "ERROR: unknown tab bar action");
  }

  static void HandleShowFileDialog(CefRefPtr<CefBrowser> browser,
                                    CefRefPtr<CefDictionaryValue> request_dict,
                                    CefRefPtr<Callback> callback) {
    if (!browser.get() || !browser->GetHost()) {
      callback->Failure(-1, "ERROR: no browser host");
      return;
    }

    // mode -> cef_file_dialog_mode_t
    cef_file_dialog_mode_t mode = FILE_DIALOG_OPEN;
    if (request_dict->HasKey("mode") &&
        request_dict->GetType("mode") == VTYPE_STRING) {
      const std::string mode_str = request_dict->GetString("mode").ToString();
      if (mode_str == "open") {
        mode = FILE_DIALOG_OPEN;
      } else if (mode_str == "open_multiple") {
        mode = FILE_DIALOG_OPEN_MULTIPLE;
      } else if (mode_str == "open_folder") {
        mode = FILE_DIALOG_OPEN_FOLDER;
      } else if (mode_str == "save") {
        mode = FILE_DIALOG_SAVE;
      } else {
        callback->Failure(-1,
                          "ERROR: unknown mode (want open|open_multiple|"
                          "open_folder|save)");
        return;
      }
    }

    CefString title;
    if (request_dict->HasKey("title") &&
        request_dict->GetType("title") == VTYPE_STRING) {
      title = request_dict->GetString("title");
    }

    CefString default_file_path;
    if (request_dict->HasKey("default_path") &&
        request_dict->GetType("default_path") == VTYPE_STRING) {
      default_file_path = request_dict->GetString("default_path");
    }

    std::vector<CefString> accept_filters;
    if (mode != FILE_DIALOG_OPEN_FOLDER &&
        request_dict->HasKey("accept_filters") &&
        request_dict->GetType("accept_filters") == VTYPE_LIST) {
      CefRefPtr<CefListValue> list = request_dict->GetList("accept_filters");
      if (list.get()) {
        const size_t n = list->GetSize();
        for (size_t i = 0; i < n; ++i) {
          if (list->GetType(i) == VTYPE_STRING) {
            accept_filters.push_back(list->GetString(i));
          }
        }
      }
    }

    printf_log(LOG_SEVERITY_INFO,
              "[FileDialog] mode=%d title=%s default=%s filters=%zu",
              static_cast<int>(mode), title.ToString().c_str(),
              default_file_path.ToString().c_str(), accept_filters.size());

    browser->GetHost()->RunFileDialog(mode, title, default_file_path,
                                      accept_filters,
                                      new FileDialogCb(callback));
  }

  static void CopyFiles(const HotReloadRequest& reload_request) {
    printf_log(LOG_SEVERITY_INFO, "Hot reload: CopyFiles starting, %zu file(s) to copy", reload_request.files.size());

    // The renderer processes are gone and the window has not been recreated
    // yet, so nothing holds the files: this is the moment they can be copied or
    // updated. Returning true skips the default copy of |files|.
    bool skip_copy = false;
    if (on_browser_hot_reload_before_restart_fptr) {
      skip_copy = on_browser_hot_reload_before_restart_fptr(reload_request.url.c_str());
      printf_log(LOG_SEVERITY_INFO, "Hot reload: on_browser_hot_reload_before_restart returned %s",
                skip_copy ? "true (skip copy)" : "false (execute copy)");
    } else {
      printf_log(LOG_SEVERITY_WARNING, "Hot reload: on_browser_hot_reload_before_restart_fptr is null, proceeding with default copy");
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

  // Completion for the hot reload flow, driven with the newly created browser
  // (both window modes report the same way). A null browser means the window
  // never came up: reported as a failure, and C# is not called because there
  // would be no browser/frame context to hand over.
  static void OnWindowCreated(CefRefPtr<Callback> callback,
                              CefRefPtr<CefBrowser> browser) {
    printf_log(LOG_SEVERITY_INFO, "Hot reload: Window created, calling C# hot reload callback");

    if (!browser.get()) {
      printf_log(LOG_SEVERITY_INFO, "Hot reload: browser is not available");
      callback->Failure(-1, "ERROR: Failed to create window");
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
