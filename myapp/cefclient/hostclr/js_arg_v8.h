#ifndef CEF_TESTS_CEFCLIENT_HOSTCLR_JS_ARG_V8_H_
#define CEF_TESTS_CEFCLIENT_HOSTCLR_JS_ARG_V8_H_

// V8 <-> JsArgNode conversion for the renderer process. Kept separate from
// js_arg.h so that browser-process translation units (which only need the wire
// and blob helpers) do not pull in the V8 headers.
//
// Complex JS types have no direct CefV8Value API (there is no IsBigInt, and
// constructing BigInt/Date/ArrayBuffer from native is awkward), so two injected
// global helpers do the heavy lifting:
//   __cefMarshal(x)            -> normalized [tagInt, payload] tree (for V8->nodes)
//   __cefBuild(tagInt, payload)-> reconstructs BigInt/Date/Binary (for nodes->V8)
// Both are installed from the renderer OnContextCreated hook (see
// client_renderer.cc) so they are available before any page script runs. If a
// helper is missing (defensive fallback), values degrade to their String form.

#include "include/cef_parser.h"
#include "include/cef_v8.h"
#include "js_arg.h"

// --- Nodes -> V8 ----------------------------------------------------------

namespace js_arg_detail {

// Invokes global __cefBuild(tag, payload) to reconstruct a complex value.
inline CefRefPtr<CefV8Value> BuildViaHelper(CefRefPtr<CefV8Value> global,
                                            int tag,
                                            const std::string& payload) {
  CefRefPtr<CefV8Value> builder = global ? global->GetValue("__cefBuild") : nullptr;
  if (builder && builder->IsFunction()) {
    CefV8ValueList call_args;
    call_args.push_back(CefV8Value::CreateInt(tag));
    call_args.push_back(CefV8Value::CreateString(payload));
    CefRefPtr<CefV8Value> r = builder->ExecuteFunction(nullptr, call_args);
    if (r) {
      return r;
    }
  }
  // Fallback: expose the payload as a plain string so nothing is lost silently.
  return CefV8Value::CreateString(payload);
}

}  // namespace js_arg_detail

// Consumes one pre-order node (and its children) starting at |cur|, advancing
// |cur| past everything consumed. Returns the constructed V8 value.
inline CefRefPtr<CefV8Value> NodeToV8(CefRefPtr<CefV8Value> global,
                                      const std::vector<JsArgNode>& nodes,
                                      size_t& cur) {
  if (cur >= nodes.size()) {
    return CefV8Value::CreateUndefined();
  }
  const JsArgNode& n = nodes[cur++];
  switch (n.tag) {
    case JsTag::Null:
      return CefV8Value::CreateNull();
    case JsTag::Undefined:
      return CefV8Value::CreateUndefined();
    case JsTag::Bool:
      return CefV8Value::CreateBool(n.i != 0);
    case JsTag::Int32:
      return CefV8Value::CreateInt(static_cast<int32_t>(n.i));
    case JsTag::UInt32:
      return CefV8Value::CreateUInt(static_cast<uint32_t>(n.i));
    case JsTag::Double:
      return CefV8Value::CreateDouble(n.d);
    case JsTag::String:
      return CefV8Value::CreateString(n.s);
    case JsTag::BigInt:
      return js_arg_detail::BuildViaHelper(global, static_cast<int>(JsTag::BigInt), n.s);
    case JsTag::DateTime:
      return js_arg_detail::BuildViaHelper(global, static_cast<int>(JsTag::DateTime), n.s);
    case JsTag::Binary: {
      std::string b64 = CefBase64Encode(n.s.data(), n.s.size()).ToString();
      return js_arg_detail::BuildViaHelper(global, static_cast<int>(JsTag::Binary), b64);
    }
    case JsTag::Array: {
      int count = n.child_count;
      CefRefPtr<CefV8Value> arr = CefV8Value::CreateArray(count);
      for (int k = 0; k < count; ++k) {
        arr->SetValue(k, NodeToV8(global, nodes, cur));
      }
      return arr;
    }
    case JsTag::Object: {
      int count = n.child_count;
      CefRefPtr<CefV8Value> obj = CefV8Value::CreateObject(nullptr, nullptr);
      for (int k = 0; k < count; ++k) {
        // key node (expected String) then value node.
        std::string key;
        if (cur < nodes.size()) {
          key = nodes[cur].s;
        }
        CefRefPtr<CefV8Value> key_v = NodeToV8(global, nodes, cur);
        (void)key_v;
        CefRefPtr<CefV8Value> val_v = NodeToV8(global, nodes, cur);
        obj->SetValue(key, val_v, V8_PROPERTY_ATTRIBUTE_NONE);
      }
      return obj;
    }
  }
  return CefV8Value::CreateUndefined();
}

// Builds a V8 argument list from a top-level pre-order node sequence.
inline void NodesToV8List(CefRefPtr<CefV8Context> context,
                          const std::vector<JsArgNode>& nodes,
                          CefV8ValueList& out) {
  CefRefPtr<CefV8Value> global = context ? context->GetGlobal() : nullptr;
  size_t cur = 0;
  while (cur < nodes.size()) {
    out.push_back(NodeToV8(global, nodes, cur));
  }
}

// --- V8 -> Nodes ----------------------------------------------------------

namespace js_arg_detail {

// Walks a normalized [tagInt, payload] value produced by __cefMarshal and
// appends the corresponding pre-order nodes.
inline void WalkNormalized(CefRefPtr<CefV8Value> norm,
                           std::vector<JsArgNode>& out) {
  // Expected shape: a 2-element array [tagInt, payload].
  if (!norm || !norm->IsArray() || norm->GetArrayLength() < 2) {
    JsArgNode n;
    n.tag = JsTag::String;
    n.s = (norm && norm->IsString()) ? norm->GetStringValue().ToString() : std::string();
    out.push_back(std::move(n));
    return;
  }
  int tag = norm->GetValue(0)->GetIntValue();
  CefRefPtr<CefV8Value> payload = norm->GetValue(1);
  JsArgNode n;
  n.tag = static_cast<JsTag>(tag);
  switch (n.tag) {
    case JsTag::Null:
    case JsTag::Undefined:
      out.push_back(std::move(n));
      return;
    case JsTag::Bool:
      n.i = (payload && payload->GetBoolValue()) ? 1 : 0;
      out.push_back(std::move(n));
      return;
    case JsTag::Int32:
      n.i = payload ? payload->GetIntValue() : 0;
      out.push_back(std::move(n));
      return;
    case JsTag::UInt32:
      n.i = payload ? static_cast<int64_t>(payload->GetUIntValue()) : 0;
      out.push_back(std::move(n));
      return;
    case JsTag::Double:
      n.d = payload ? payload->GetDoubleValue() : 0.0;
      out.push_back(std::move(n));
      return;
    case JsTag::BigInt:
    case JsTag::String:
    case JsTag::DateTime:
      n.s = payload ? payload->GetStringValue().ToString() : std::string();
      out.push_back(std::move(n));
      return;
    case JsTag::Binary: {
      // payload is a base64 string; decode back to raw bytes.
      std::string b64 = payload ? payload->GetStringValue().ToString() : std::string();
      CefRefPtr<CefBinaryValue> bin = CefBase64Decode(b64);
      if (bin && bin->GetSize() > 0) {
        n.s.resize(bin->GetSize());
        bin->GetData(&n.s[0], bin->GetSize(), 0);
      }
      out.push_back(std::move(n));
      return;
    }
    case JsTag::Array: {
      int count = (payload && payload->IsArray()) ? payload->GetArrayLength() : 0;
      n.child_count = count;
      out.push_back(std::move(n));
      for (int k = 0; k < count; ++k) {
        WalkNormalized(payload->GetValue(k), out);
      }
      return;
    }
    case JsTag::Object: {
      // payload is a flat array [k0, v0, k1, v1, ...] of length 2*count.
      int len = (payload && payload->IsArray()) ? payload->GetArrayLength() : 0;
      int count = len / 2;
      n.child_count = count;
      out.push_back(std::move(n));
      for (int k = 0; k < count; ++k) {
        JsArgNode key;
        key.tag = JsTag::String;
        CefRefPtr<CefV8Value> kv = payload->GetValue(2 * k);
        key.s = (kv && kv->IsString()) ? kv->GetStringValue().ToString() : std::string();
        out.push_back(std::move(key));
        WalkNormalized(payload->GetValue(2 * k + 1), out);
      }
      return;
    }
  }
  // Unknown tag: keep a null placeholder rather than dropping the slot.
  n.tag = JsTag::Null;
  out.push_back(std::move(n));
}

}  // namespace js_arg_detail

// Converts a single V8 value into pre-order nodes (appended to |out|), using
// the injected __cefMarshal helper for lossless typing.
inline void V8ToNodes(CefRefPtr<CefV8Context> context,
                      CefRefPtr<CefV8Value> value,
                      std::vector<JsArgNode>& out) {
  CefRefPtr<CefV8Value> global = context ? context->GetGlobal() : nullptr;
  CefRefPtr<CefV8Value> marshal = global ? global->GetValue("__cefMarshal") : nullptr;
  CefRefPtr<CefV8Value> norm;
  if (marshal && marshal->IsFunction()) {
    CefV8ValueList call_args;
    call_args.push_back(value ? value : CefV8Value::CreateUndefined());
    norm = marshal->ExecuteFunction(nullptr, call_args);
  }
  if (norm) {
    js_arg_detail::WalkNormalized(norm, out);
  } else {
    // Fallback when __cefMarshal is unavailable: best-effort string.
    JsArgNode n;
    n.tag = JsTag::String;
    if (value && value->IsString()) {
      n.s = value->GetStringValue().ToString();
    }
    out.push_back(std::move(n));
  }
}

#endif  // CEF_TESTS_CEFCLIENT_HOSTCLR_JS_ARG_V8_H_
