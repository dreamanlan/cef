// Copyright (c) 2013 The Chromium Embedded Framework Authors. All rights
// reserved. Use of this source code is governed by a BSD-style license that
// can be found in the LICENSE file.

#pragma once

#include <cstdint>
#include <string>

// Windows-only fallback for CEF proxy/HTTP authentication when the DSL/C#
// layer declines. Uses CredUIPromptForCredentialsW for the UI and Windows
// Credential Manager (CredRead/Write/Delete) for persistence.
//
// Contract:
//   * Must be called on the CEF UI thread (blocking modal UI).
//   * Completes |handle| (registered via RegisterNativeCallback) with a
//     "username\npassword" payload on success (ok=true), or with ok=false
//     on user cancel.
//   * attempt == 0 tries the Credential Manager first; on hit the callback
//     is completed silently. On miss the prompt is shown.
//   * attempt >= 1 purges any stale saved entry first, then always prompts.
//   * On non-Windows platforms the function is a no-op that completes the
//     callback with ok=false so the request is cancelled cleanly.
void RunCredUIFallback(int64_t handle,
                       const std::string& target_key,
                       bool is_proxy,
                       const std::string& host,
                       int port,
                       const std::string& realm,
                       int attempt,
                       void* parent_hwnd);
