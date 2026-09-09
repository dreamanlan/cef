// Copyright (c) 2013 The Chromium Embedded Framework Authors. All rights
// reserved. Use of this source code is governed by a BSD-style license that
// can be found in the LICENSE file.

// macOS implementation of RunCredUIFallback.
//
// Persistence: Keychain (login.keychain-db), kSecClassGenericPassword. The
// target_key is stored as kSecAttrService so entries appear in the
// "Keychain Access.app" UI, semantically equivalent to Windows Credential
// Manager entries created by credui_prompt.cc.
//
// UI: NSAlert with a two-field accessory view (username + secure password).
// The alert is app-modal, mirroring CredUIPromptForCredentialsW on Windows.

#include "myapp/cefclient/hostclr/credui_prompt.h"

#include "myapp/cefclient/hostclr/HostCLR.h"
#include "myapp/cefclient/hostclr/native_callbacks.h"

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#import <Security/Security.h>

#include <string>

namespace {

NSString* NSStringFromUtf8(const std::string& s) {
    return [[NSString alloc] initWithBytes:s.data()
                                    length:s.size()
                                  encoding:NSUTF8StringEncoding];
}

std::string Utf8FromNSString(NSString* s) {
    if (!s) {
        return std::string();
    }
    const char* c = [s UTF8String];
    return c ? std::string(c) : std::string();
}

// Read a generic-password entry keyed by |target_key|. On hit fills
// |user_utf8| / |pass_utf8| and returns true.
bool KeychainRead(const std::string& target_key,
                  std::string* user_utf8,
                  std::string* pass_utf8) {
    NSDictionary* query = @{
        (__bridge id)kSecClass : (__bridge id)kSecClassGenericPassword,
        (__bridge id)kSecAttrService : NSStringFromUtf8(target_key),
        (__bridge id)kSecReturnAttributes : @YES,
        (__bridge id)kSecReturnData : @YES,
        (__bridge id)kSecMatchLimit : (__bridge id)kSecMatchLimitOne,
    };
    CFTypeRef result_ref = nullptr;
    OSStatus status = SecItemCopyMatching((__bridge CFDictionaryRef)query,
                                          &result_ref);
    if (status != errSecSuccess || !result_ref) {
        return false;
    }
    NSDictionary* result = (__bridge_transfer NSDictionary*)result_ref;
    NSString* account = result[(__bridge id)kSecAttrAccount];
    NSData* data = result[(__bridge id)kSecValueData];
    if (!account || !data) {
        return false;
    }
    *user_utf8 = Utf8FromNSString(account);
    *pass_utf8 = std::string(static_cast<const char*>([data bytes]),
                             [data length]);
    return true;
}

// Insert-or-update a generic-password entry keyed by |target_key|.
bool KeychainWrite(const std::string& target_key,
                   const std::string& user_utf8,
                   const std::string& pass_utf8) {
    NSString* service = NSStringFromUtf8(target_key);
    NSString* account = NSStringFromUtf8(user_utf8);
    NSData* password = [NSData dataWithBytes:pass_utf8.data()
                                      length:pass_utf8.size()];

    // Try update first (matches by service alone; account and data change).
    NSDictionary* query = @{
        (__bridge id)kSecClass : (__bridge id)kSecClassGenericPassword,
        (__bridge id)kSecAttrService : service,
    };
    NSDictionary* changes = @{
        (__bridge id)kSecAttrAccount : account,
        (__bridge id)kSecValueData : password,
    };
    OSStatus status = SecItemUpdate((__bridge CFDictionaryRef)query,
                                    (__bridge CFDictionaryRef)changes);
    if (status == errSecSuccess) {
        return true;
    }
    if (status != errSecItemNotFound) {
        return false;
    }

    // Not present yet: add a new item.
    NSDictionary* add = @{
        (__bridge id)kSecClass : (__bridge id)kSecClassGenericPassword,
        (__bridge id)kSecAttrService : service,
        (__bridge id)kSecAttrAccount : account,
        (__bridge id)kSecValueData : password,
        (__bridge id)kSecAttrAccessible :
            (__bridge id)kSecAttrAccessibleAfterFirstUnlock,
    };
    status = SecItemAdd((__bridge CFDictionaryRef)add, nullptr);
    return status == errSecSuccess;
}

void KeychainDelete(const std::string& target_key) {
    NSDictionary* query = @{
        (__bridge id)kSecClass : (__bridge id)kSecClassGenericPassword,
        (__bridge id)kSecAttrService : NSStringFromUtf8(target_key),
    };
    SecItemDelete((__bridge CFDictionaryRef)query);
}

// Runs |block| on the main thread synchronously. Safe when the caller is
// already on the main thread (dispatch_sync would deadlock in that case).
void RunOnMainSync(void (^block)(void)) {
    if ([NSThread isMainThread]) {
        block();
    } else {
        dispatch_sync(dispatch_get_main_queue(), block);
    }
}

// Shows a modal NSAlert with two text fields. Returns true on OK and fills
// |user_out| / |pass_out|; returns false on Cancel.
bool ShowPromptModal(const std::string& caption,
                     const std::string& message,
                     const std::string& initial_user,
                     std::string* user_out,
                     std::string* pass_out) {
    __block BOOL ok = NO;
    __block NSString* user_result = nil;
    __block NSString* pass_result = nil;

    RunOnMainSync(^{
        NSAlert* alert = [[NSAlert alloc] init];
        alert.messageText = NSStringFromUtf8(caption);
        alert.informativeText = NSStringFromUtf8(message);
        [alert addButtonWithTitle:@"OK"];
        [alert addButtonWithTitle:@"Cancel"];

        const CGFloat kWidth = 320.0;
        const CGFloat kFieldH = 22.0;
        const CGFloat kLabelH = 18.0;
        const CGFloat kGap = 4.0;
        const CGFloat kBlockGap = 8.0;
        const CGFloat kTotalH =
            kLabelH + kGap + kFieldH + kBlockGap + kLabelH + kGap + kFieldH;

        NSView* container =
            [[NSView alloc] initWithFrame:NSMakeRect(0, 0, kWidth, kTotalH)];

        CGFloat y = kTotalH;

        y -= kLabelH;
        NSTextField* userLabel =
            [NSTextField labelWithString:@"Username:"];
        userLabel.frame = NSMakeRect(0, y, kWidth, kLabelH);
        [container addSubview:userLabel];

        y -= (kGap + kFieldH);
        NSTextField* userField =
            [[NSTextField alloc] initWithFrame:NSMakeRect(0, y, kWidth,
                                                          kFieldH)];
        userField.stringValue = NSStringFromUtf8(initial_user);
        [container addSubview:userField];

        y -= (kBlockGap + kLabelH);
        NSTextField* passLabel =
            [NSTextField labelWithString:@"Password:"];
        passLabel.frame = NSMakeRect(0, y, kWidth, kLabelH);
        [container addSubview:passLabel];

        y -= (kGap + kFieldH);
        NSSecureTextField* passField =
            [[NSSecureTextField alloc]
                initWithFrame:NSMakeRect(0, y, kWidth, kFieldH)];
        [container addSubview:passField];

        alert.accessoryView = container;

        // Make the username field first responder if empty, otherwise the
        // password field.
        [alert.window setInitialFirstResponder:
            (initial_user.empty() ? (NSView*)userField : (NSView*)passField)];

        NSModalResponse response = [alert runModal];
        if (response == NSAlertFirstButtonReturn) {
            ok = YES;
            user_result = [userField.stringValue copy];
            pass_result = [passField.stringValue copy];
        }
    });

    if (!ok) {
        return false;
    }
    *user_out = Utf8FromNSString(user_result);
    *pass_out = Utf8FromNSString(pass_result);
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
                       void* /*parent_hwnd*/) {
    // Step 1: try saved credentials on the first attempt.
    if (attempt == 0) {
        std::string user_utf8;
        std::string pass_utf8;
        if (KeychainRead(target_key, &user_utf8, &pass_utf8) &&
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
        KeychainDelete(target_key);
    }

    // Step 2: show the prompt.
    std::string caption = is_proxy ? "Proxy authentication required"
                                   : "Authentication required";
    std::string message = is_proxy ? "Enter credentials for proxy "
                                   : "Enter credentials for ";
    message += host;
    message += ":";
    message += std::to_string(port);
    if (!realm.empty()) {
        message += " (";
        message += realm;
        message += ")";
    }

    // Prefill with any leftover account name from a stale entry so the user
    // only has to retype the password. On attempt==0 miss this is a no-op.
    std::string initial_user;
    {
        std::string tmp_pass;
        (void)KeychainRead(target_key, &initial_user, &tmp_pass);
    }

    std::string user_utf8;
    std::string pass_utf8;
    if (!ShowPromptModal(caption, message, initial_user, &user_utf8,
                         &pass_utf8)) {
        printf_log(LOG_SEVERITY_INFO,
                   "[credui] user cancelled prompt for %s",
                   target_key.c_str());
        CompleteNativeCallback(handle, false, std::string(), 0);
        return;
    }

    // Step 3: persist for future runs and complete the callback.
    if (!KeychainWrite(target_key, user_utf8, pass_utf8)) {
        printf_log(LOG_SEVERITY_WARNING,
                   "[credui] failed to persist credentials for %s",
                   target_key.c_str());
    }
    printf_log(LOG_SEVERITY_INFO,
               "[credui] prompted for %s, got user=%s",
               target_key.c_str(), user_utf8.c_str());
    const std::string data = user_utf8 + "\n" + pass_utf8;
    CompleteNativeCallback(handle, true, data, 0);
}
