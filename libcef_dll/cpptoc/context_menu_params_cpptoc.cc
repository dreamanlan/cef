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
// $hash=dbedbe557d28ffb247be6e185f838a9cbc999eb8$
//

#include "libcef_dll/cpptoc/context_menu_params_cpptoc.h"
#include "libcef_dll/shutdown_checker.h"
#include "libcef_dll/transfer_util.h"

namespace {

// MEMBER FUNCTIONS - Body may be edited by hand.

int CEF_CALLBACK
context_menu_params_get_xcoord(struct _cef_context_menu_params_t* self) {
  shutdown_checker::AssertNotShutdown();

  // AUTO-GENERATED CONTENT - DELETE THIS COMMENT BEFORE MODIFYING

  DCHECK(self);
  if (!self) {
    return 0;
  }

  // Execute
  int _retval = CefContextMenuParamsCppToC::Get(self)->GetXCoord();

  // Return type: simple
  return _retval;
}

int CEF_CALLBACK
context_menu_params_get_ycoord(struct _cef_context_menu_params_t* self) {
  shutdown_checker::AssertNotShutdown();

  // AUTO-GENERATED CONTENT - DELETE THIS COMMENT BEFORE MODIFYING

  DCHECK(self);
  if (!self) {
    return 0;
  }

  // Execute
  int _retval = CefContextMenuParamsCppToC::Get(self)->GetYCoord();

  // Return type: simple
  return _retval;
}

cef_context_menu_type_flags_t CEF_CALLBACK
context_menu_params_get_type_flags(struct _cef_context_menu_params_t* self) {
  shutdown_checker::AssertNotShutdown();

  // AUTO-GENERATED CONTENT - DELETE THIS COMMENT BEFORE MODIFYING

  DCHECK(self);
  if (!self) {
    return CM_TYPEFLAG_NONE;
  }

  // Execute
  cef_context_menu_type_flags_t _retval =
      CefContextMenuParamsCppToC::Get(self)->GetTypeFlags();

  // Return type: simple
  return _retval;
}

cef_string_userfree_t CEF_CALLBACK
context_menu_params_get_link_url(struct _cef_context_menu_params_t* self) {
  shutdown_checker::AssertNotShutdown();

  // AUTO-GENERATED CONTENT - DELETE THIS COMMENT BEFORE MODIFYING

  DCHECK(self);
  if (!self) {
    return NULL;
  }

  // Execute
  CefString _retval = CefContextMenuParamsCppToC::Get(self)->GetLinkUrl();

  // Return type: string
  return _retval.DetachToUserFree();
}

cef_string_userfree_t CEF_CALLBACK context_menu_params_get_unfiltered_link_url(
    struct _cef_context_menu_params_t* self) {
  shutdown_checker::AssertNotShutdown();

  // AUTO-GENERATED CONTENT - DELETE THIS COMMENT BEFORE MODIFYING

  DCHECK(self);
  if (!self) {
    return NULL;
  }

  // Execute
  CefString _retval =
      CefContextMenuParamsCppToC::Get(self)->GetUnfilteredLinkUrl();

  // Return type: string
  return _retval.DetachToUserFree();
}

cef_string_userfree_t CEF_CALLBACK
context_menu_params_get_source_url(struct _cef_context_menu_params_t* self) {
  shutdown_checker::AssertNotShutdown();

  // AUTO-GENERATED CONTENT - DELETE THIS COMMENT BEFORE MODIFYING

  DCHECK(self);
  if (!self) {
    return NULL;
  }

  // Execute
  CefString _retval = CefContextMenuParamsCppToC::Get(self)->GetSourceUrl();

  // Return type: string
  return _retval.DetachToUserFree();
}

int CEF_CALLBACK context_menu_params_has_image_contents(
    struct _cef_context_menu_params_t* self) {
  shutdown_checker::AssertNotShutdown();

  // AUTO-GENERATED CONTENT - DELETE THIS COMMENT BEFORE MODIFYING

  DCHECK(self);
  if (!self) {
    return 0;
  }

  // Execute
  bool _retval = CefContextMenuParamsCppToC::Get(self)->HasImageContents();

  // Return type: bool
  return _retval;
}

cef_string_userfree_t CEF_CALLBACK
context_menu_params_get_title_text(struct _cef_context_menu_params_t* self) {
  shutdown_checker::AssertNotShutdown();

  // AUTO-GENERATED CONTENT - DELETE THIS COMMENT BEFORE MODIFYING

  DCHECK(self);
  if (!self) {
    return NULL;
  }

  // Execute
  CefString _retval = CefContextMenuParamsCppToC::Get(self)->GetTitleText();

  // Return type: string
  return _retval.DetachToUserFree();
}

cef_string_userfree_t CEF_CALLBACK
context_menu_params_get_page_url(struct _cef_context_menu_params_t* self) {
  shutdown_checker::AssertNotShutdown();

  // AUTO-GENERATED CONTENT - DELETE THIS COMMENT BEFORE MODIFYING

  DCHECK(self);
  if (!self) {
    return NULL;
  }

  // Execute
  CefString _retval = CefContextMenuParamsCppToC::Get(self)->GetPageUrl();

  // Return type: string
  return _retval.DetachToUserFree();
}

cef_string_userfree_t CEF_CALLBACK
context_menu_params_get_frame_url(struct _cef_context_menu_params_t* self) {
  shutdown_checker::AssertNotShutdown();

  // AUTO-GENERATED CONTENT - DELETE THIS COMMENT BEFORE MODIFYING

  DCHECK(self);
  if (!self) {
    return NULL;
  }

  // Execute
  CefString _retval = CefContextMenuParamsCppToC::Get(self)->GetFrameUrl();

  // Return type: string
  return _retval.DetachToUserFree();
}

cef_string_userfree_t CEF_CALLBACK
context_menu_params_get_frame_charset(struct _cef_context_menu_params_t* self) {
  shutdown_checker::AssertNotShutdown();

  // AUTO-GENERATED CONTENT - DELETE THIS COMMENT BEFORE MODIFYING

  DCHECK(self);
  if (!self) {
    return NULL;
  }

  // Execute
  CefString _retval = CefContextMenuParamsCppToC::Get(self)->GetFrameCharset();

  // Return type: string
  return _retval.DetachToUserFree();
}

cef_context_menu_media_type_t CEF_CALLBACK
context_menu_params_get_media_type(struct _cef_context_menu_params_t* self) {
  shutdown_checker::AssertNotShutdown();

  // AUTO-GENERATED CONTENT - DELETE THIS COMMENT BEFORE MODIFYING

  DCHECK(self);
  if (!self) {
    return CM_MEDIATYPE_NONE;
  }

  // Execute
  cef_context_menu_media_type_t _retval =
      CefContextMenuParamsCppToC::Get(self)->GetMediaType();

  // Return type: simple
  return _retval;
}

cef_context_menu_media_state_flags_t CEF_CALLBACK
context_menu_params_get_media_state_flags(
    struct _cef_context_menu_params_t* self) {
  shutdown_checker::AssertNotShutdown();

  // AUTO-GENERATED CONTENT - DELETE THIS COMMENT BEFORE MODIFYING

  DCHECK(self);
  if (!self) {
    return CM_MEDIAFLAG_NONE;
  }

  // Execute
  cef_context_menu_media_state_flags_t _retval =
      CefContextMenuParamsCppToC::Get(self)->GetMediaStateFlags();

  // Return type: simple
  return _retval;
}

cef_string_userfree_t CEF_CALLBACK context_menu_params_get_selection_text(
    struct _cef_context_menu_params_t* self) {
  shutdown_checker::AssertNotShutdown();

  // AUTO-GENERATED CONTENT - DELETE THIS COMMENT BEFORE MODIFYING

  DCHECK(self);
  if (!self) {
    return NULL;
  }

  // Execute
  CefString _retval = CefContextMenuParamsCppToC::Get(self)->GetSelectionText();

  // Return type: string
  return _retval.DetachToUserFree();
}

cef_string_userfree_t CEF_CALLBACK context_menu_params_get_misspelled_word(
    struct _cef_context_menu_params_t* self) {
  shutdown_checker::AssertNotShutdown();

  // AUTO-GENERATED CONTENT - DELETE THIS COMMENT BEFORE MODIFYING

  DCHECK(self);
  if (!self) {
    return NULL;
  }

  // Execute
  CefString _retval =
      CefContextMenuParamsCppToC::Get(self)->GetMisspelledWord();

  // Return type: string
  return _retval.DetachToUserFree();
}

int CEF_CALLBACK context_menu_params_get_dictionary_suggestions(
    struct _cef_context_menu_params_t* self,
    cef_string_list_t suggestions) {
  shutdown_checker::AssertNotShutdown();

  // AUTO-GENERATED CONTENT - DELETE THIS COMMENT BEFORE MODIFYING

  DCHECK(self);
  if (!self) {
    return 0;
  }
  // Verify param: suggestions; type: string_vec_byref
  DCHECK(suggestions);
  if (!suggestions) {
    return 0;
  }

  // Translate param: suggestions; type: string_vec_byref
  std::vector<CefString> suggestionsList;
  transfer_string_list_contents(suggestions, suggestionsList);

  // Execute
  bool _retval =
      CefContextMenuParamsCppToC::Get(self)->GetDictionarySuggestions(
          suggestionsList);

  // Restore param: suggestions; type: string_vec_byref
  cef_string_list_clear(suggestions);
  transfer_string_list_contents(suggestionsList, suggestions);

  // Return type: bool
  return _retval;
}

int CEF_CALLBACK
context_menu_params_is_editable(struct _cef_context_menu_params_t* self) {
  shutdown_checker::AssertNotShutdown();

  // AUTO-GENERATED CONTENT - DELETE THIS COMMENT BEFORE MODIFYING

  DCHECK(self);
  if (!self) {
    return 0;
  }

  // Execute
  bool _retval = CefContextMenuParamsCppToC::Get(self)->IsEditable();

  // Return type: bool
  return _retval;
}

int CEF_CALLBACK context_menu_params_is_spell_check_enabled(
    struct _cef_context_menu_params_t* self) {
  shutdown_checker::AssertNotShutdown();

  // AUTO-GENERATED CONTENT - DELETE THIS COMMENT BEFORE MODIFYING

  DCHECK(self);
  if (!self) {
    return 0;
  }

  // Execute
  bool _retval = CefContextMenuParamsCppToC::Get(self)->IsSpellCheckEnabled();

  // Return type: bool
  return _retval;
}

cef_context_menu_edit_state_flags_t CEF_CALLBACK
context_menu_params_get_edit_state_flags(
    struct _cef_context_menu_params_t* self) {
  shutdown_checker::AssertNotShutdown();

  // AUTO-GENERATED CONTENT - DELETE THIS COMMENT BEFORE MODIFYING

  DCHECK(self);
  if (!self) {
    return CM_EDITFLAG_NONE;
  }

  // Execute
  cef_context_menu_edit_state_flags_t _retval =
      CefContextMenuParamsCppToC::Get(self)->GetEditStateFlags();

  // Return type: simple
  return _retval;
}

int CEF_CALLBACK
context_menu_params_is_custom_menu(struct _cef_context_menu_params_t* self) {
  shutdown_checker::AssertNotShutdown();

  // AUTO-GENERATED CONTENT - DELETE THIS COMMENT BEFORE MODIFYING

  DCHECK(self);
  if (!self) {
    return 0;
  }

  // Execute
  bool _retval = CefContextMenuParamsCppToC::Get(self)->IsCustomMenu();

  // Return type: bool
  return _retval;
}

}  // namespace

// CONSTRUCTOR - Do not edit by hand.

CefContextMenuParamsCppToC::CefContextMenuParamsCppToC() {
  GetStruct()->get_xcoord = context_menu_params_get_xcoord;
  GetStruct()->get_ycoord = context_menu_params_get_ycoord;
  GetStruct()->get_type_flags = context_menu_params_get_type_flags;
  GetStruct()->get_link_url = context_menu_params_get_link_url;
  GetStruct()->get_unfiltered_link_url =
      context_menu_params_get_unfiltered_link_url;
  GetStruct()->get_source_url = context_menu_params_get_source_url;
  GetStruct()->has_image_contents = context_menu_params_has_image_contents;
  GetStruct()->get_title_text = context_menu_params_get_title_text;
  GetStruct()->get_page_url = context_menu_params_get_page_url;
  GetStruct()->get_frame_url = context_menu_params_get_frame_url;
  GetStruct()->get_frame_charset = context_menu_params_get_frame_charset;
  GetStruct()->get_media_type = context_menu_params_get_media_type;
  GetStruct()->get_media_state_flags =
      context_menu_params_get_media_state_flags;
  GetStruct()->get_selection_text = context_menu_params_get_selection_text;
  GetStruct()->get_misspelled_word = context_menu_params_get_misspelled_word;
  GetStruct()->get_dictionary_suggestions =
      context_menu_params_get_dictionary_suggestions;
  GetStruct()->is_editable = context_menu_params_is_editable;
  GetStruct()->is_spell_check_enabled =
      context_menu_params_is_spell_check_enabled;
  GetStruct()->get_edit_state_flags = context_menu_params_get_edit_state_flags;
  GetStruct()->is_custom_menu = context_menu_params_is_custom_menu;
}

// DESTRUCTOR - Do not edit by hand.

CefContextMenuParamsCppToC::~CefContextMenuParamsCppToC() {
  shutdown_checker::AssertNotShutdown();
}

template <>
CefRefPtr<CefContextMenuParams> CefCppToCRefCounted<
    CefContextMenuParamsCppToC,
    CefContextMenuParams,
    cef_context_menu_params_t>::UnwrapDerived(CefWrapperType type,
                                              cef_context_menu_params_t* s) {
  DCHECK(false) << "Unexpected class type: " << type;
  return nullptr;
}

template <>
CefWrapperType CefCppToCRefCounted<CefContextMenuParamsCppToC,
                                   CefContextMenuParams,
                                   cef_context_menu_params_t>::kWrapperType =
    WT_CONTEXT_MENU_PARAMS;
