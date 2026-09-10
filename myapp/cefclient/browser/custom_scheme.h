// Copyright (c) 2026 The Chromium Embedded Framework Authors. All rights
// reserved. Use of this source code is governed by a BSD-style license that
// can be found in the LICENSE file.

#ifndef CEF_TESTS_CEFCLIENT_BROWSER_CUSTOM_SCHEME_H_
#define CEF_TESTS_CEFCLIENT_BROWSER_CUSTOM_SCHEME_H_
#pragma once

#include <string>

namespace client::custom_scheme {

// Create and register the generic custom scheme handler factory for the
// built-in scheme (custom_scheme::kCustomSchemeName). Runs in the browser
// process after CefInitialize. Called from test_runner.cc.
void RegisterSchemeHandlers();

// Runtime (un)registration of the generic handler factory for an arbitrary
// scheme/domain, exposed to managed code as register_custom_scheme /
// unregister_custom_scheme. |domain| may be empty to match all hosts under the
// scheme. Returns true on success. NOTE: a brand-new *standard* scheme must
// still be declared at startup (custom_scheme_common.h); a scheme not declared
// there is handled as a non-standard scheme (opaque origin, limited features).
bool RegisterSchemeFactory(const std::string& scheme,
                           const std::string& domain);
bool UnregisterSchemeFactory(const std::string& scheme,
                             const std::string& domain);

}  // namespace client::custom_scheme

#endif  // CEF_TESTS_CEFCLIENT_BROWSER_CUSTOM_SCHEME_H_
