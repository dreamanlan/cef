// Copyright (c) 2023 The Chromium Embedded Framework Authors. All rights
// reserved. Use of this source code is governed by a BSD-style license that
// can be found in the LICENSE file.
//
// ---------------------------------------------------------------------------
//
// This file was generated by the CEF translator tool. If making changes by
// hand only do so within the body of existing method and function
// implementations. See the translator.README.txt file in the tools directory
// for more information.
//
// $hash=e722a5b9ae2bb6e3d3236a199930600dc3b5e0f8$
//

#ifndef CEF_LIBCEF_DLL_CTOCPP_TASK_CTOCPP_H_
#define CEF_LIBCEF_DLL_CTOCPP_TASK_CTOCPP_H_
#pragma once

#if !defined(BUILDING_CEF_SHARED)
#error This file can be included DLL-side only
#endif

#include "include/capi/cef_task_capi.h"
#include "include/cef_task.h"
#include "libcef_dll/ctocpp/ctocpp_ref_counted.h"

// Wrap a C structure with a C++ class.
// This class may be instantiated and accessed DLL-side only.
class CefTaskCToCpp
    : public CefCToCppRefCounted<CefTaskCToCpp, CefTask, cef_task_t> {
 public:
  CefTaskCToCpp();
  virtual ~CefTaskCToCpp();

  // CefTask methods.
  void Execute() override;
};

#endif  // CEF_LIBCEF_DLL_CTOCPP_TASK_CTOCPP_H_
