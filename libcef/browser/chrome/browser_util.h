// Copyright 2024 The Chromium Embedded Framework Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CEF_LIBCEF_BROWSER_CHROME_BROWSER_UTIL_H_
#define CEF_LIBCEF_BROWSER_CHROME_BROWSER_UTIL_H_
#pragma once

#include "base/check.h"
#include "chrome/browser/ui/browser.h"

namespace cef {

// BrowserWindowInterface is always implemented by Browser on desktop
// platforms. These helpers provide a safe checked downcast.
inline Browser* BrowserForBWI(BrowserWindowInterface* bwi) {
  auto* browser = static_cast<Browser*>(bwi);
  DCHECK(!bwi || browser);
  return browser;
}

inline const Browser* BrowserForBWI(const BrowserWindowInterface* bwi) {
  auto* browser = static_cast<const Browser*>(bwi);
  DCHECK(!bwi || browser);
  return browser;
}

}  // namespace cef

#endif  // CEF_LIBCEF_BROWSER_CHROME_BROWSER_UTIL_H_
