#ifndef CEF_TESTS_CEFCLIENT_HOSTCLR_JS_ARG_H_
#define CEF_TESTS_CEFCLIENT_HOSTCLR_JS_ARG_H_

// Cross-boundary argument marshalling shared by the C# host, the native bridge
// and (via js_arg_v8.h) the renderer V8 layer. It replaces the old "every
// argument stringified" convention so that int64/bigint/binary/datetime and
// nested arrays/objects survive the round trip without loss.
//
// A single self-contained byte blob is used as the wire format on EVERY
// boundary:
//   * C# <-> native  : C# passes a pinned byte[] (ptr,len); native deserializes.
//   * cross-process  : the very same blob is carried as one CefBinaryValue, so
//                      send/receive can forward it without re-encoding.
//   * V8 (renderer)  : the blob is turned into JsArgNode and then CefV8Value
//                      (see js_arg_v8.h); this is the only place that decodes.
//
// Container encoding is a pre-order flat sequence:
//   Array : one node with tag=Array, child_count=N, followed by N child nodes.
//   Object: one node with tag=Object, child_count=N, followed by 2*N nodes in
//           key0,val0,key1,val1,... order (keys are String nodes).
// The top level is simply a flat pre-order sequence consumed until exhausted.
//
// Blob layout (host-native endianness; producer and consumer are on the same
// machine):
//   [int32 count]
//   per node: [int32 tag][int32 child_count][int64 i][double d]
//             [int32 slen][slen raw bytes]

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

enum class JsTag : int32_t {
  Null = 0,
  Undefined = 1,
  Bool = 2,
  Int32 = 3,
  UInt32 = 4,
  Double = 5,
  BigInt = 6,   // s = decimal string
  String = 7,   // s = UTF-8 text
  Binary = 8,   // s = raw bytes (may contain embedded NUL, length = slen)
  Array = 9,    // child_count = element count
  Object = 10,  // child_count = key/value pair count
  DateTime = 11 // s = ISO 8601 string
};

// Native hub node. Owns its payload bytes so it can move freely between the
// blob and V8 layers without external lifetime concerns.
struct JsArgNode {
  JsTag tag = JsTag::Null;
  int32_t child_count = 0;
  int64_t i = 0;      // Bool(0/1) / Int32 / UInt32
  double d = 0.0;     // Double
  std::string s;      // BigInt/String/Binary/DateTime payload (raw bytes)
};

namespace js_arg_detail {
inline void AppendBytes(std::vector<uint8_t>& out, const void* p, size_t n) {
  const uint8_t* b = static_cast<const uint8_t*>(p);
  out.insert(out.end(), b, b + n);
}
inline bool ReadBytes(const uint8_t* data, size_t size, size_t& pos, void* out,
                      size_t n) {
  if (pos + n > size) {
    return false;
  }
  std::memcpy(out, data + pos, n);
  pos += n;
  return true;
}
}  // namespace js_arg_detail

// Serializes a pre-order node sequence into the self-contained byte blob.
inline std::vector<uint8_t> SerializeNodes(const std::vector<JsArgNode>& nodes) {
  using namespace js_arg_detail;
  std::vector<uint8_t> out;
  int32_t count = static_cast<int32_t>(nodes.size());
  AppendBytes(out, &count, sizeof(count));
  for (const auto& n : nodes) {
    int32_t tag = static_cast<int32_t>(n.tag);
    int32_t child_count = n.child_count;
    int64_t i = n.i;
    double d = n.d;
    int32_t slen = static_cast<int32_t>(n.s.size());
    AppendBytes(out, &tag, sizeof(tag));
    AppendBytes(out, &child_count, sizeof(child_count));
    AppendBytes(out, &i, sizeof(i));
    AppendBytes(out, &d, sizeof(d));
    AppendBytes(out, &slen, sizeof(slen));
    if (slen > 0) {
      AppendBytes(out, n.s.data(), static_cast<size_t>(slen));
    }
  }
  return out;
}

// Inverse of SerializeNodes. Returns an empty vector on malformed input.
inline std::vector<JsArgNode> DeserializeNodes(const uint8_t* data,
                                               size_t size) {
  using namespace js_arg_detail;
  std::vector<JsArgNode> nodes;
  if (!data || size < sizeof(int32_t)) {
    return nodes;
  }
  size_t pos = 0;
  int32_t count = 0;
  if (!ReadBytes(data, size, pos, &count, sizeof(count)) || count < 0) {
    return nodes;
  }
  nodes.reserve(static_cast<size_t>(count));
  for (int32_t k = 0; k < count; ++k) {
    JsArgNode n;
    int32_t tag = 0;
    int32_t child_count = 0;
    int64_t i = 0;
    double d = 0.0;
    int32_t slen = 0;
    if (!ReadBytes(data, size, pos, &tag, sizeof(tag)) ||
        !ReadBytes(data, size, pos, &child_count, sizeof(child_count)) ||
        !ReadBytes(data, size, pos, &i, sizeof(i)) ||
        !ReadBytes(data, size, pos, &d, sizeof(d)) ||
        !ReadBytes(data, size, pos, &slen, sizeof(slen))) {
      nodes.clear();
      return nodes;
    }
    n.tag = static_cast<JsTag>(tag);
    n.child_count = child_count;
    n.i = i;
    n.d = d;
    if (slen > 0) {
      if (pos + static_cast<size_t>(slen) > size) {
        nodes.clear();
        return nodes;
      }
      n.s.assign(reinterpret_cast<const char*>(data + pos),
                 static_cast<size_t>(slen));
      pos += static_cast<size_t>(slen);
    }
    nodes.push_back(std::move(n));
  }
  return nodes;
}

#endif  // CEF_TESTS_CEFCLIENT_HOSTCLR_JS_ARG_H_
