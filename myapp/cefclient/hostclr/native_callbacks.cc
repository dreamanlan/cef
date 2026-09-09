#include "myapp/cefclient/hostclr/native_callbacks.h"

#include "myapp/cefclient/hostclr/HostCLR.h"

#include "include/base/cef_callback.h"
#include "include/wrapper/cef_closure_task.h"

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

namespace {

struct NativeCallbackEntry {
    NativeCallbackFn fn;
    int browser_id = 0;
    cef_thread_id_t thread_id = TID_UI;
    // Zero means the entry never expires.
    std::chrono::steady_clock::time_point deadline;
};

// Trampoline for CefPostTask. A free function is used (instead of a lambda) to
// match the base::BindOnce usage found elsewhere in cefclient.
void RunCallbackTask(NativeCallbackFn fn, bool ok, std::string data, int code) {
    if (fn) {
        fn(ok, data, code);
    }
}

// Meyers local-static + heap allocation so the container is never destroyed
// during static teardown (the same pattern the ref maps in HostCLR.cpp use:
// destroying entries after CEF shutdown would release CefRefPtrs too late).
std::mutex& GetCallbackMutex() {
    static auto* s_m = new std::mutex();
    return *s_m;
}

std::map<int64_t, NativeCallbackEntry>& GetCallbackMap() {
    static auto* s_map = new std::map<int64_t, NativeCallbackEntry>();
    return *s_map;
}

std::atomic<int64_t>& GetNextHandle() {
    static auto* s_next = new std::atomic<int64_t>(1);
    return *s_next;
}

// Takes the entry out of the map. Returns false when the handle is unknown
// (already completed, discarded or cancelled).
bool TakeEntry(int64_t handle, NativeCallbackEntry* out_entry) {
    if (handle == 0 || !out_entry) {
        return false;
    }
    std::lock_guard<std::mutex> lock(GetCallbackMutex());
    auto& m = GetCallbackMap();
    auto it = m.find(handle);
    if (it == m.end()) {
        return false;
    }
    *out_entry = std::move(it->second);
    m.erase(it);
    return true;
}

// Runs the closure on the thread it was registered for. The entry is already
// removed from the map at this point, so the map lock is not held here: the
// closure calls into CEF (CefJSDialogCallback::Continue, CefCallback::Continue)
// and must never run while holding our mutex.
void RunEntry(NativeCallbackEntry entry,
              bool ok,
              const std::string& data,
              int code) {
    if (!entry.fn) {
        return;
    }
    if (CefCurrentlyOn(entry.thread_id)) {
        entry.fn(ok, data, code);
        return;
    }
    // Copy into the task: |entry| is a local and |data| may be a temporary.
    NativeCallbackFn fn = std::move(entry.fn);
    std::string payload = data;
    CefPostTask(entry.thread_id, base::BindOnce(&RunCallbackTask, std::move(fn),
                                                ok, std::move(payload), code));
}

}  // namespace

int64_t RegisterNativeCallback(int browser_id,
                              cef_thread_id_t thread_id,
                              NativeCallbackFn fn,
                              int timeout_ms) {
    if (!fn) {
        return 0;
    }
    NativeCallbackEntry entry;
    entry.fn = std::move(fn);
    entry.browser_id = browser_id;
    entry.thread_id = thread_id;
    if (timeout_ms > 0) {
        entry.deadline = std::chrono::steady_clock::now() +
                         std::chrono::milliseconds(timeout_ms);
    }

    const int64_t handle = GetNextHandle().fetch_add(1);
    {
        std::lock_guard<std::mutex> lock(GetCallbackMutex());
        GetCallbackMap()[handle] = std::move(entry);
    }
    return handle;
}

void DiscardNativeCallback(int64_t handle) {
    NativeCallbackEntry entry;
    // Destroy the closure (and the CefRefPtr it captured) outside the lock.
    if (TakeEntry(handle, &entry)) {
        entry.fn = nullptr;
    }
}

bool CompleteNativeCallback(int64_t handle,
                            bool ok,
                            const std::string& data,
                            int code) {
    NativeCallbackEntry entry;
    if (!TakeEntry(handle, &entry)) {
        printf_log(LOG_SEVERITY_INFO,
                   "[native] native callback %lld already completed or unknown",
                   static_cast<long long>(handle));
        return false;
    }
    RunEntry(std::move(entry), ok, data, code);
    return true;
}

void CancelBrowserCallbacks(int browser_id) {
    std::vector<std::pair<int64_t, NativeCallbackEntry>> to_cancel;
    {
        std::lock_guard<std::mutex> lock(GetCallbackMutex());
        auto& m = GetCallbackMap();
        for (auto it = m.begin(); it != m.end();) {
            if (it->second.browser_id == browser_id) {
                to_cancel.emplace_back(it->first, std::move(it->second));
                it = m.erase(it);
            } else {
                ++it;
            }
        }
    }
    // Cancel outside the lock.
    for (auto& pair : to_cancel) {
        printf_log(LOG_SEVERITY_INFO,
                   "[native] canceling pending native callback %lld (browser %d)",
                   static_cast<long long>(pair.first), browser_id);
        RunEntry(std::move(pair.second), false, std::string(), 0);
    }
}

size_t GetPendingNativeCallbackCount() {
    std::lock_guard<std::mutex> lock(GetCallbackMutex());
    return GetCallbackMap().size();
}

void SweepExpiredNativeCallbacks() {
    std::vector<std::pair<int64_t, NativeCallbackEntry>> expired;
    {
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(GetCallbackMutex());
        auto& m = GetCallbackMap();
        for (auto it = m.begin(); it != m.end();) {
            const bool has_deadline =
                it->second.deadline != std::chrono::steady_clock::time_point();
            if (has_deadline && it->second.deadline <= now) {
                expired.emplace_back(it->first, std::move(it->second));
                it = m.erase(it);
            } else {
                ++it;
            }
        }
    }
    // Complete outside the lock, same as CancelBrowserCallbacks.
    for (auto& pair : expired) {
        printf_log(LOG_SEVERITY_WARNING,
                   "[native] native callback %lld timed out, canceling it "
                   "(managed code never completed the operation)",
                   static_cast<long long>(pair.first));
        RunEntry(std::move(pair.second), false, std::string(), 0);
    }
}

int native_callback_complete(int64_t handle, int ok, const char* data, int code) {
    const std::string payload = data ? std::string(data) : std::string();
    return CompleteNativeCallback(handle, ok != 0, payload, code) ? 1 : 0;
}
