// Copyright (c) 2026 The Chromium Embedded Framework Authors. All rights
// reserved. Use of this source code is governed by a BSD-style license that
// can be found in the LICENSE file.

#ifndef CEF_TESTS_CEFCLIENT_BROWSER_COOKIE_LIST_BRIDGE_H_
#define CEF_TESTS_CEFCLIENT_BROWSER_COOKIE_LIST_BRIDGE_H_
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "include/base/cef_macros.h"
#include "include/base/cef_ref_counted.h"

// Ref-counted cookie-jar snapshot passed to C# via
// on_resource_cookie_list_fptr. Filled by the cookie visitor during
// VisitUrlCookies enumeration and delivered once complete. All fields are
// plain copies (no CefString / CefTime) so the object is safe across the
// async cookie-store round trip. The raw pointer received by C# is valid
// only for the duration of the callback; the visitor holds the last
// reference and releases it when the callback returns.
class CookieListBridge : public CefBaseRefCounted {
 public:
  struct Entry {
    std::string name;
    std::string value;
    std::string domain;
    std::string path;
    bool secure = false;
    bool httponly = false;
    int same_site = 0;
    // Chrome time (microseconds since 1601-01-01 UTC); useful for relative
    // comparison (e.g. whether a session cookie's creation moves).
    int64_t creation = 0;
    int64_t last_access = 0;
  };

  CookieListBridge() = default;

  // Query context captured at trigger time: the URL the jar was queried
  // with and the upstream response status observed then (0 when unknown).
  std::string url;
  int status = 0;
  std::vector<Entry> entries;

  IMPLEMENT_REFCOUNTING(CookieListBridge);
  DISALLOW_COPY_AND_ASSIGN(CookieListBridge);
};

#endif  // CEF_TESTS_CEFCLIENT_BROWSER_COOKIE_LIST_BRIDGE_H_
