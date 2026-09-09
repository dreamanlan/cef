#ifndef JAVASCRIPT_CALLER_H_
#define JAVASCRIPT_CALLER_H_

#include "include/cef_browser.h"
#include "include/cef_frame.h"
#include "include/cef_v8.h"
#include "include/cef_app.h"
#include "include/base/cef_logging.h"
#include "HostCLR.h"
#include "js_arg_v8.h"
#include <cmath>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

class JavaScriptCaller {
public:
    // ---- JsArg (typed) variants -------------------------------------------

    // Call a JS function with typed pre-order node arguments.
    static bool SendCallNodes(CefRefPtr<CefBrowser> browser,
                              CefRefPtr<CefFrame> frame,
                              const std::string& function_name,
                              const std::vector<JsArgNode>& args) {
        if (!frame) {
            if (browser) {
                frame = browser->GetMainFrame();
            }
            if (!frame) {
                printf_log(LOG_SEVERITY_ERROR, "Frame is null");
                return false;
            }
        }
        if (CefCurrentlyOn(TID_RENDERER)) {
            return CallWithV8Nodes(frame, function_name, args);
        } else {
            return CallWithExecuteJavaScriptNodes(frame, function_name, args);
        }
    }

    // Call a JS function in the renderer and return its result as typed nodes.
    static std::vector<JsArgNode> CallInRendererNodes(
        CefRefPtr<CefBrowser> browser,
        CefRefPtr<CefFrame> frame,
        const std::string& function_name,
        const std::vector<JsArgNode>& args) {
        std::vector<JsArgNode> out;
        if (!frame) {
            if (browser) {
                frame = browser->GetMainFrame();
            }
            if (!frame) {
                printf_log(LOG_SEVERITY_ERROR, "Frame is null");
                return out;
            }
        }
        if (!CefCurrentlyOn(TID_RENDERER)) {
            printf_log(LOG_SEVERITY_ERROR, "CallInRendererNodes must be called in renderer process");
            return out;
        }
        CefRefPtr<CefV8Context> context = frame->GetV8Context();
        if (!context || !context->Enter()) {
            printf_log(LOG_SEVERITY_ERROR, "Failed to enter V8 context");
            return out;
        }
        CefRefPtr<CefV8Value> global = context->GetGlobal();
        CefRefPtr<CefV8Value> func = GetNestedFunction(global, function_name);
        if (func && func->IsFunction()) {
            CefV8ValueList v8_args;
            NodesToV8List(context, args, v8_args);
            CefRefPtr<CefV8Value> result = func->ExecuteFunction(global, v8_args);
            if (result) {
                V8ToNodes(context, result, out);
            } else {
                printf_log(LOG_SEVERITY_ERROR, "V8 function execution failed: %s", function_name.c_str());
            }
        } else {
            printf_log(LOG_SEVERITY_ERROR, "Function not found or not callable: %s", function_name.c_str());
        }
        context->Exit();
        return out;
    }

    // Eval a piece of JS code in the renderer and return its result as nodes.
    static std::vector<JsArgNode> ExecuteInRendererNodes(
        CefRefPtr<CefBrowser> browser,
        CefRefPtr<CefFrame> frame,
        const std::string& code) {
        std::vector<JsArgNode> out;
        if (!frame) {
            if (browser) {
                frame = browser->GetMainFrame();
            }
            if (!frame) {
                printf_log(LOG_SEVERITY_ERROR, "Frame is null");
                return out;
            }
        }
        if (!CefCurrentlyOn(TID_RENDERER)) {
            printf_log(LOG_SEVERITY_ERROR, "ExecuteInRendererNodes must be called in renderer process");
            return out;
        }
        CefRefPtr<CefV8Context> context = frame->GetV8Context();
        if (!context || !context->Enter()) {
            printf_log(LOG_SEVERITY_ERROR, "Failed to enter V8 context");
            return out;
        }
        CefRefPtr<CefV8Value> retval;
        CefRefPtr<CefV8Exception> exception;
        if (context->Eval(code, frame->GetURL(), 0, retval, exception)) {
            if (retval) {
                V8ToNodes(context, retval, out);
            }
        } else if (exception) {
            printf_log(LOG_SEVERITY_ERROR, "ExecuteInRendererNodes failed: %s",
                       exception->GetMessage().ToString().c_str());
        }
        context->Exit();
        return out;
    }

private:
    // Call using V8 API with typed node arguments (renderer process).
    static bool CallWithV8Nodes(CefRefPtr<CefFrame> frame,
                                const std::string& function_name,
                                const std::vector<JsArgNode>& args) {
        CefRefPtr<CefV8Context> context = frame->GetV8Context();
        if (!context || !context->Enter()) {
            printf_log(LOG_SEVERITY_ERROR, "Failed to enter V8 context");
            return false;
        }
        bool success = false;
        CefRefPtr<CefV8Value> global = context->GetGlobal();
        CefRefPtr<CefV8Value> func = GetNestedFunction(global, function_name);
        if (func && func->IsFunction()) {
            CefV8ValueList v8_args;
            NodesToV8List(context, args, v8_args);
            CefRefPtr<CefV8Value> result = func->ExecuteFunction(global, v8_args);
            success = (result != nullptr);
            if (!success) {
                printf_log(LOG_SEVERITY_ERROR, "V8 function execution failed: %s", function_name.c_str());
            }
        } else {
            printf_log(LOG_SEVERITY_ERROR, "Function not found or not callable: %s", function_name.c_str());
        }
        context->Exit();
        return success;
    }

    // Call using ExecuteJavaScript with typed node arguments (browser process).
    // Nodes are rendered as JS literals since there is no V8 access here.
    static bool CallWithExecuteJavaScriptNodes(CefRefPtr<CefFrame> frame,
                                               const std::string& function_name,
                                               const std::vector<JsArgNode>& args) {
        if (!frame) {
            printf_log(LOG_SEVERITY_ERROR, "Frame is null");
            return false;
        }
        std::string script = function_name + "(";
        size_t cur = 0;
        bool first = true;
        while (cur < args.size()) {
            if (!first) {
                script += ", ";
            }
            first = false;
            script += NodeToJsLiteral(args, cur);
        }
        script += ");";
        printf_log(LOG_SEVERITY_INFO, "Executing JavaScript: %s", script.c_str());
        frame->ExecuteJavaScript(script, frame->GetURL(), 0);
        return true;
    }

    // Render a double as a JS numeric literal (handles NaN/Infinity).
    static std::string DoubleToJsLiteral(double d) {
        if (std::isnan(d)) {
            return "NaN";
        }
        if (std::isinf(d)) {
            return d < 0 ? "-Infinity" : "Infinity";
        }
        std::ostringstream oss;
        oss << std::setprecision(17) << d;
        return oss.str();
    }

    // Consume one pre-order node and render it as a JS literal, advancing cur.
    static std::string NodeToJsLiteral(const std::vector<JsArgNode>& nodes,
                                       size_t& cur) {
        if (cur >= nodes.size()) {
            return "undefined";
        }
        const JsArgNode& n = nodes[cur++];
        switch (n.tag) {
            case JsTag::Null:
                return "null";
            case JsTag::Undefined:
                return "undefined";
            case JsTag::Bool:
                return n.i != 0 ? "true" : "false";
            case JsTag::Int32:
                return std::to_string(static_cast<int32_t>(n.i));
            case JsTag::UInt32:
                return std::to_string(static_cast<uint32_t>(n.i));
            case JsTag::Double:
                return DoubleToJsLiteral(n.d);
            case JsTag::BigInt:
                return (n.s.empty() ? std::string("0") : n.s) + "n";
            case JsTag::String:
                return "'" + EscapeJavaScriptString(n.s) + "'";
            case JsTag::DateTime:
                return "new Date('" + EscapeJavaScriptString(n.s) + "')";
            case JsTag::Binary: {
                std::string b64 = CefBase64Encode(n.s.data(), n.s.size()).ToString();
                return "Uint8Array.from(atob('" + EscapeJavaScriptString(b64) +
                       "'),function(c){return c.charCodeAt(0);}).buffer";
            }
            case JsTag::Array: {
                std::string out = "[";
                for (int k = 0; k < n.child_count; ++k) {
                    if (k > 0) {
                        out += ",";
                    }
                    out += NodeToJsLiteral(nodes, cur);
                }
                out += "]";
                return out;
            }
            case JsTag::Object: {
                std::string out = "{";
                for (int k = 0; k < n.child_count; ++k) {
                    if (k > 0) {
                        out += ",";
                    }
                    std::string key;
                    if (cur < nodes.size()) {
                        key = nodes[cur].s;
                    }
                    NodeToJsLiteral(nodes, cur);
                    out += "'" + EscapeJavaScriptString(key) + "':";
                    out += NodeToJsLiteral(nodes, cur);
                }
                out += "}";
                return out;
            }
        }
        return "undefined";
    }

    // Escape JavaScript string to prevent injection and syntax errors
    static std::string EscapeJavaScriptString(const std::string& str) {
        std::string result;
        result.reserve(str.length() * 1.2); // Pre-allocate space

        for (char c : str) {
            switch (c) {
                case '\'':
                    result += "\\'";
                    break;
                case '"':
                    result += "\\\"";
                    break;
                case '\\':
                    result += "\\\\";
                    break;
                case '\n':
                    result += "\\n";
                    break;
                case '\r':
                    result += "\\r";
                    break;
                case '\t':
                    result += "\\t";
                    break;
                case '\b':
                    result += "\\b";
                    break;
                case '\f':
                    result += "\\f";
                    break;
                case '\0':
                    result += "\\0";
                    break;
                default:
                    // Handle other control characters
                    if (c < 32 || c == 127) {
                        char buf[8];
                        snprintf(buf, sizeof(buf), "\\x%02x", (unsigned char)c);
                        result += buf;
                    } else {
                        result += c;
                    }
                    break;
            }
        }

        return result;
    }

    // Get nested function (supports calls like "console.log")
    static CefRefPtr<CefV8Value> GetNestedFunction(CefRefPtr<CefV8Value> object,
                                                   const std::string& function_name) {
        size_t pos = function_name.find('.');

        if (pos == std::string::npos) {
            // Simple function name
            return object->GetValue(function_name);
        }

        // Nested function name like "console.log"
        std::string parent_name = function_name.substr(0, pos);
        std::string child_name = function_name.substr(pos + 1);

        CefRefPtr<CefV8Value> parent = object->GetValue(parent_name);
        if (!parent || !parent->IsObject()) {
            return nullptr;
        }

        // Recursively handle deeper nesting
        return GetNestedFunction(parent, child_name);
    }
};

#endif  // JAVASCRIPT_CALLER_H_