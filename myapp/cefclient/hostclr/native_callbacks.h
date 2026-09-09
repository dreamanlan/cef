#pragma once
#include "include/cef_base.h"
#include "include/cef_task.h"
#include <cstdint>
#include <functional>
#include <string>

// Generic bridge for CEF async callbacks that managed code (C#/DSL) takes over.
//
// Several CEF handlers follow the same shape: the handler returns a value that
// means "the application takes over" and keeps a callback object which must be
// executed later to resume the pending operation. Examples:
//   CefJSDialogCallback  (OnJSDialog / OnBeforeUnloadDialog, UI thread)
//   CefCallback          (OnBeforeResourceLoad + RV_CONTINUE_ASYNC, IO thread)
//
// Managed code cannot hold a CefRefPtr, so the callback is stored here and an
// opaque int64 handle is passed across the interop boundary. Managed code later
// completes the operation by handle.
//
// Usage from a CEF handler:
//   int64_t handle = RegisterNativeCallback(browser_id, TID_UI, closure);
//   int decision = managed_fptr(..., handle);
//   if (decision == TAKEOVER) return true;   // managed side completes later
//   DiscardNativeCallback(handle);           // not taken over: drop it
//
// Thread affinity is stored per entry because each CEF callback must be
// executed on the thread its handler was invoked on. CompleteNativeCallback may
// be called from any thread; execution is posted to the registered thread.

// Signature of the closure that resumes a pending CEF operation.
// |ok| is the accept/cancel decision, |data| carries optional payload such as
// the text entered in a prompt dialog or a cefQuery response body, and |code|
// carries an optional numeric result for interfaces that need one (for example
// CefMessageRouterBrowserSide::Callback::Failure takes an error code).
// Entry points that do not need |data| / |code| simply ignore them.
using NativeCallbackFn =
    std::function<void(bool ok, const std::string& data, int code)>;

// Pass as |timeout_ms| to keep an entry pending until it is completed or the
// browser goes away.
constexpr int kNativeCallbackNoTimeout = 0;

// Default timeouts of the current entry points. A pending resource load blocks
// the page, so it expires quickly; a cefQuery only blocks its own JS callback.
// JS dialogs legitimately wait for a user, so they get a long stop-gap rather
// than a short timeout.
constexpr int kResourceLoadTimeoutMs = 30 * 1000;
constexpr int kCefQueryTimeoutMs = 60 * 1000;
constexpr int kJsDialogTimeoutMs = 10 * 60 * 1000;

// Registers |fn| and returns a non-zero handle. |thread_id| is the CEF thread
// |fn| must run on. |browser_id| is used by CancelBrowserCallbacks.
// |timeout_ms| expires the entry when managed code never completes it: the
// closure then runs with ok=false, which every entry point treats as a safe
// cancel. Pass kNativeCallbackNoTimeout to wait forever (only sensible when the
// operation cannot block the page, e.g. a dialog waiting for a user click).
// Expiration requires the heartbeat to run in this process (see HostCLR.cpp).
int64_t RegisterNativeCallback(int browser_id,
                              cef_thread_id_t thread_id,
                              NativeCallbackFn fn,
                              int timeout_ms);

// Removes the entry WITHOUT invoking it. Used when managed code declined to
// take over after the handle was already registered. Idempotent.
void DiscardNativeCallback(int64_t handle);

// Completes and unregisters the entry. Callable from any thread: the closure is
// posted to the thread recorded at registration time. Idempotent - completing
// an unknown handle is a no-op (this makes races between a timeout and a user
// action harmless). Returns true if the handle was found.
bool CompleteNativeCallback(int64_t handle,
                            bool ok,
                            const std::string& data,
                            int code);

// Cancels (invokes with ok=false) and unregisters every entry owned by
// |browser_id|. Called on navigation reset and browser close so a pending
// operation can never outlive its browser.
void CancelBrowserCallbacks(int browser_id);

// Number of currently pending entries (diagnostics only).
size_t GetPendingNativeCallbackCount();

// Completes (with ok=false) every entry whose timeout has elapsed. This is the
// last-resort net for managed code that took an operation over and then forgot
// to complete it: without it a pending JS dialog freezes the page and a pending
// resource load leaves it loading forever. Called from the heartbeat task.
void SweepExpiredNativeCallbacks();

// --- Managed entry point, exposed through HostApi ---
// |ok| is 0/1, |data| may be null, |code| is an interface specific numeric
// result (0 when unused). Returns 1 when the handle was found.
int native_callback_complete(int64_t handle, int ok, const char* data, int code);
