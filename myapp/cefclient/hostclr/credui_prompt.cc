// Copyright (c) 2013 The Chromium Embedded Framework Authors. All rights
// reserved. Use of this source code is governed by a BSD-style license that
// can be found in the LICENSE file.

#include "myapp/cefclient/hostclr/credui_prompt.h"

#include "myapp/cefclient/hostclr/HostCLR.h"
#include "myapp/cefclient/hostclr/native_callbacks.h"

#if defined(_WIN32)

#include <windows.h>
#include <wincred.h>

#include <string>

#pragma comment(lib, "credui.lib")
#pragma comment(lib, "advapi32.lib")

namespace {

bool CredentialStoreRead(const std::wstring& target,
                         std::string* user_utf8,
                         std::string* pass_utf8) {
    PCREDENTIALW cred = nullptr;
    if (!::CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &cred) || !cred) {
        return false;
    }
    std::wstring user(cred->UserName ? cred->UserName : L"");
    std::wstring pass;
    if (cred->CredentialBlob && cred->CredentialBlobSize >= sizeof(wchar_t)) {
        pass.assign(reinterpret_cast<const wchar_t*>(cred->CredentialBlob),
                    cred->CredentialBlobSize / sizeof(wchar_t));
    }
    ::CredFree(cred);
    *user_utf8 = WideStringToUtf8(user.c_str());
    *pass_utf8 = WideStringToUtf8(pass.c_str());
    return true;
}

bool CredentialStoreWrite(const std::wstring& target,
                          const std::wstring& user,
                          const std::wstring& pass) {
    CREDENTIALW cred = {};
    cred.Type = CRED_TYPE_GENERIC;
    cred.TargetName = const_cast<LPWSTR>(target.c_str());
    cred.UserName = const_cast<LPWSTR>(user.c_str());
    cred.CredentialBlob =
        reinterpret_cast<LPBYTE>(const_cast<LPWSTR>(pass.c_str()));
    cred.CredentialBlobSize =
        static_cast<DWORD>(pass.size() * sizeof(wchar_t));
    cred.Persist = CRED_PERSIST_LOCAL_MACHINE;
    return ::CredWriteW(&cred, 0) != FALSE;
}

void CredentialStoreDelete(const std::wstring& target) {
    ::CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0);
}

// Shows the standard Windows credential prompt modally on the calling
// thread. Returns true on OK, false on Cancel / error.
// The credui-owned auto-persist path is disabled (CREDUI_FLAGS_DO_NOT_PERSIST)
// because we do our own persistence via CredWriteW after a successful
// prompt.
bool ShowCredUI(HWND parent,
                const std::wstring& caption,
                const std::wstring& message,
                std::wstring* user_out,
                std::wstring* pass_out) {
    CREDUI_INFOW info = {};
    info.cbSize = sizeof(info);
    info.hwndParent = parent;
    info.pszMessageText = message.c_str();
    info.pszCaptionText = caption.c_str();

    wchar_t user_buf[CREDUI_MAX_USERNAME_LENGTH + 1] = {};
    wchar_t pass_buf[CREDUI_MAX_PASSWORD_LENGTH + 1] = {};
    if (!user_out->empty()) {
        wcsncpy_s(user_buf, user_out->c_str(), _TRUNCATE);
    }
    BOOL save = FALSE;
    DWORD flags = CREDUI_FLAGS_GENERIC_CREDENTIALS |
                  CREDUI_FLAGS_ALWAYS_SHOW_UI |
                  CREDUI_FLAGS_DO_NOT_PERSIST;
    DWORD rc = ::CredUIPromptForCredentialsW(
        &info, L"CefClientAuthSession", nullptr, 0,
        user_buf, ARRAYSIZE(user_buf), pass_buf, ARRAYSIZE(pass_buf),
        &save, flags);
    if (rc != NO_ERROR) {
        ::SecureZeroMemory(pass_buf, sizeof(pass_buf));
        return false;
    }
    *user_out = user_buf;
    *pass_out = pass_buf;
    ::SecureZeroMemory(pass_buf, sizeof(pass_buf));
    return true;
}

}  // namespace

void RunCredUIFallback(int64_t handle,
                       const std::string& target_key,
                       bool is_proxy,
                       const std::string& host,
                       int port,
                       const std::string& realm,
                       int attempt,
                       void* parent_hwnd) {
    HWND hwnd = static_cast<HWND>(parent_hwnd);
    const std::wstring wtarget = Utf8ToWstring(target_key.c_str());

    // Step 1: try saved credentials on the first attempt.
    if (attempt == 0) {
        std::string user_utf8;
        std::string pass_utf8;
        if (CredentialStoreRead(wtarget, &user_utf8, &pass_utf8) &&
            !user_utf8.empty()) {
            printf_log(LOG_SEVERITY_INFO,
                       "[credui] using saved credentials for %s (user=%s)",
                       target_key.c_str(), user_utf8.c_str());
            const std::string data = user_utf8 + "\n" + pass_utf8;
            CompleteNativeCallback(handle, true, data, 0);
            return;
        }
    } else {
        // Retry: the previously supplied credentials were rejected. Purge
        // them so we do not offer them again this session.
        printf_log(LOG_SEVERITY_WARNING,
                   "[credui] purging stale credentials for %s (attempt=%d)",
                   target_key.c_str(), attempt);
        CredentialStoreDelete(wtarget);
    }

    // Step 2: show the prompt.
    std::wstring caption = is_proxy
        ? L"Proxy authentication required"
        : L"Authentication required";
    std::wstring message = is_proxy ? L"Enter credentials for proxy "
                                    : L"Enter credentials for ";
    message += Utf8ToWstring(host.c_str());
    message += L":";
    message += std::to_wstring(port);
    if (!realm.empty()) {
        message += L" (";
        message += Utf8ToWstring(realm.c_str());
        message += L")";
    }

    std::wstring wuser;
    std::wstring wpass;
    if (!ShowCredUI(hwnd, caption, message, &wuser, &wpass)) {
        printf_log(LOG_SEVERITY_INFO,
                   "[credui] user cancelled prompt for %s",
                   target_key.c_str());
        CompleteNativeCallback(handle, false, std::string(), 0);
        return;
    }

    // Step 3: persist for future runs and complete the callback.
    if (!CredentialStoreWrite(wtarget, wuser, wpass)) {
        printf_log(LOG_SEVERITY_WARNING,
                   "[credui] failed to persist credentials for %s "
                   "(GetLastError=%lu)",
                   target_key.c_str(),
                   static_cast<unsigned long>(::GetLastError()));
    }
    const std::string user_utf8 = WideStringToUtf8(wuser.c_str());
    const std::string pass_utf8 = WideStringToUtf8(wpass.c_str());
    printf_log(LOG_SEVERITY_INFO,
               "[credui] prompted for %s, got user=%s",
               target_key.c_str(), user_utf8.c_str());
    const std::string data = user_utf8 + "\n" + pass_utf8;
    CompleteNativeCallback(handle, true, data, 0);
}

#elif !defined(__APPLE__)

// macOS has its own implementation in credui_prompt_mac.mm.
// This branch is the fallback for other POSIX platforms (e.g. Linux):
// simply cancel the challenge so the request fails deterministically.

void RunCredUIFallback(int64_t handle,
                       const std::string& target_key,
                       bool /*is_proxy*/,
                       const std::string& /*host*/,
                       int /*port*/,
                       const std::string& /*realm*/,
                       int /*attempt*/,
                       void* /*parent_hwnd*/) {
    printf_log(LOG_SEVERITY_WARNING,
               "[credui] not implemented on this platform, cancelling %s",
               target_key.c_str());
    CompleteNativeCallback(handle, false, std::string(), 0);
}

#endif  // _WIN32 / __APPLE__
