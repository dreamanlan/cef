// Copyright (c) 2026 The Chromium Embedded Framework Authors. All rights
// reserved. Use of this source code is governed by a BSD-style license that
// can be found in the LICENSE file.

#ifndef CEF_TESTS_CEFCLIENT_COMMON_CUSTOM_SCHEME_COMMON_H_
#define CEF_TESTS_CEFCLIENT_COMMON_CUSTOM_SCHEME_COMMON_H_
#pragma once

#include "include/cef_scheme.h"

namespace client::custom_scheme {

// Name of the general-purpose custom scheme declared at startup. Content served
// through it is produced by managed code (C#/DSL) via the on_custom_scheme
// callback, with a C++ built-in fallback. Used for the tab-bar UI page and any
// other app resources the script wants to expose.
extern const char kCustomSchemeName[];

// Register the custom scheme name/type. This must be done in ALL processes and
// only during OnRegisterCustomSchemes (the single point where AddCustomScheme
// may be called). See browser/custom_scheme.h for the handler factory that only
// runs in the browser process. Called from client_app_delegates_common.cc.
void RegisterCustomSchemes(CefRawPtr<CefSchemeRegistrar> registrar);

}  // namespace client::custom_scheme

#endif  // CEF_TESTS_CEFCLIENT_COMMON_CUSTOM_SCHEME_COMMON_H_
