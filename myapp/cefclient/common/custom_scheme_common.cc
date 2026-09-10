// Copyright (c) 2026 The Chromium Embedded Framework Authors. All rights
// reserved. Use of this source code is governed by a BSD-style license that
// can be found in the LICENSE file.

#include "myapp/cefclient/common/custom_scheme_common.h"

#include "include/cef_scheme.h"

namespace client::custom_scheme {

const char kCustomSchemeName[] = "webagent";

void RegisterCustomSchemes(CefRawPtr<CefSchemeRegistrar> registrar) {
  // Declared as standard + secure + CORS + fetch so pages served through it
  // behave like ordinary secure web origins (localStorage, fetch, module
  // scripts, mixed-content rules). The request routing / handler factory is
  // registered separately in the browser process (browser/custom_scheme.cc).
  registrar->AddCustomScheme(
      kCustomSchemeName,
      CEF_SCHEME_OPTION_STANDARD | CEF_SCHEME_OPTION_SECURE |
          CEF_SCHEME_OPTION_CORS_ENABLED | CEF_SCHEME_OPTION_FETCH_ENABLED);
}

}  // namespace client::custom_scheme
