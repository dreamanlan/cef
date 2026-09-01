#ifndef JAVASCRIPT_CALLER_H_
#define JAVASCRIPT_CALLER_H_

#include "include/cef_browser.h"
#include "include/cef_frame.h"
#include "include/cef_v8.h"
#include "include/cef_app.h"
#include "include/base/cef_logging.h"
#include "HostCLR.h"
#include <string>
#include <vector>

class JavaScriptCaller {
public:
    // Send call to JavaScript function with multiple string arguments
    static bool SendCall(CefRefPtr<CefBrowser> browser,
                    CefRefPtr<CefFrame> frame,
                    const std::string& function_name,
                    const std::vector<std::string>& args) {

        if (!frame) {
            if (browser) {
                frame = browser->GetMainFrame();
            }
            if (!frame) {
                printf_log(LOG_SEVERITY_ERROR, "Frame is null");
                return false;
            }
        }

        // Check if we're in renderer process
        if (CefCurrentlyOn(TID_RENDERER)) {
            return CallWithV8(frame, function_name, args);
        } else {
            return CallWithExecuteJavaScript(frame, function_name, args);
        }
    }

    // Send call to JavaScript function with single string argument
    static bool SendCall(CefRefPtr<CefBrowser> browser,
                    CefRefPtr<CefFrame> frame,
                    const std::string& function_name,
                    const std::string& arg) {
        std::vector<std::string> args;
        args.push_back(arg);
        return SendCall(browser, frame, function_name, args);
    }

    // Send call to JavaScript function with no arguments
    static bool SendCall(CefRefPtr<CefBrowser> browser,
                    CefRefPtr<CefFrame> frame,
                    const std::string& function_name) {
        std::vector<std::string> args;
        return SendCall(browser, frame, function_name, args);
    }

    // Call JavaScript function in renderer process and return result as string
    static std::string CallInRenderer(CefRefPtr<CefBrowser> browser,
                                     CefRefPtr<CefFrame> frame,
                                     const std::string& function_name,
                                     const std::vector<std::string>& args) {
        if (!frame) {
            if (browser) {
                frame = browser->GetMainFrame();
            }
            if (!frame) {
                printf_log(LOG_SEVERITY_ERROR, "Frame is null");
                return "";
            }
        }

        // Must be called in renderer process
        if (!CefCurrentlyOn(TID_RENDERER)) {
            printf_log(LOG_SEVERITY_ERROR, "CallInRenderer must be called in renderer process");
            return "";
        }

        CefRefPtr<CefV8Context> context = frame->GetV8Context();
        if (!context) {
            printf_log(LOG_SEVERITY_ERROR, "V8 context is null");
            return "";
        }

        if (!context->Enter()) {
            printf_log(LOG_SEVERITY_ERROR, "Failed to enter V8 context");
            return "";
        }

        std::string result_str;

        CefRefPtr<CefV8Value> global = context->GetGlobal();

        // Support nested function calls like "console.log"
        CefRefPtr<CefV8Value> func = GetNestedFunction(global, function_name);

        if (func && func->IsFunction()) {
            // Convert arguments to V8 values
            CefV8ValueList v8_args;
            for (const auto& arg : args) {
                v8_args.push_back(CefV8Value::CreateString(arg));
            }

            // Execute function
            CefRefPtr<CefV8Value> result = func->ExecuteFunction(global, v8_args);

            if (result) {
                result_str = V8ValueToString(context, result);
                printf_log(LOG_SEVERITY_INFO, "CallInRenderer succeeded: %s, result: %s",
                          function_name.c_str(), result_str.c_str());
            } else {
                printf_log(LOG_SEVERITY_ERROR, "V8 function execution failed: %s", function_name.c_str());
            }
        } else {
            printf_log(LOG_SEVERITY_ERROR, "Function not found or not callable: %s", function_name.c_str());
        }

        context->Exit();
        return result_str;
    }

    // Overload: Call with single argument
    static std::string CallInRenderer(CefRefPtr<CefBrowser> browser,
                                     CefRefPtr<CefFrame> frame,
                                     const std::string& function_name,
                                     const std::string& arg) {
        std::vector<std::string> args;
        args.push_back(arg);
        return CallInRenderer(browser, frame, function_name, args);
    }

    // Overload: Call with no arguments
    static std::string CallInRenderer(CefRefPtr<CefBrowser> browser,
                                     CefRefPtr<CefFrame> frame,
                                     const std::string& function_name) {
        std::vector<std::string> args;
        return CallInRenderer(browser, frame, function_name, args);
    }

    // Execute a piece of JavaScript code synchronously in renderer process
    // and return the result as string
    static std::string ExecuteInRenderer(CefRefPtr<CefBrowser> browser,
                                        CefRefPtr<CefFrame> frame,
                                        const std::string& code) {
        if (!frame) {
            if (browser) {
                frame = browser->GetMainFrame();
            }
            if (!frame) {
                printf_log(LOG_SEVERITY_ERROR, "Frame is null");
                return "";
            }
        }

        // Must be called in renderer process
        if (!CefCurrentlyOn(TID_RENDERER)) {
            printf_log(LOG_SEVERITY_ERROR, "ExecuteInRenderer must be called in renderer process");
            return "";
        }

        CefRefPtr<CefV8Context> context = frame->GetV8Context();
        if (!context) {
            printf_log(LOG_SEVERITY_ERROR, "V8 context is null");
            return "";
        }

        if (!context->Enter()) {
            printf_log(LOG_SEVERITY_ERROR, "Failed to enter V8 context");
            return "";
        }

        std::string result_str;

        CefRefPtr<CefV8Value> retval;
        CefRefPtr<CefV8Exception> exception;

        if (context->Eval(code, frame->GetURL(), 0, retval, exception)) {
            if (retval) {
                result_str = V8ValueToString(context, retval);
                printf_log(LOG_SEVERITY_INFO, "ExecuteInRenderer succeeded, result: %s",
                          result_str.c_str());
            } else {
                printf_log(LOG_SEVERITY_ERROR, "ExecuteInRenderer returned null value");
            }
        } else {
            if (exception) {
                printf_log(LOG_SEVERITY_ERROR, "ExecuteInRenderer failed: %s",
                          exception->GetMessage().ToString().c_str());
            } else {
                printf_log(LOG_SEVERITY_ERROR, "ExecuteInRenderer failed with unknown error");
            }
        }

        context->Exit();
        return result_str;
    }

    // Convert a V8 value to string using JS semantics (same as the page's String(x)):
    // 5 -> "5", 5.5 -> "5.5", [1,2,3] -> "1,2,3", {} -> "[object Object]",
    // null -> "null", undefined -> "undefined". Falls back to a manual conversion
    // when String() is unavailable (no context, or String has been overridden).
    static std::string V8ValueToString(CefRefPtr<CefV8Context> context,
                                       CefRefPtr<CefV8Value> value) {
        if (!value) {
            return "null";
        }

        // Fast common path: already a string.
        if (value->IsString()) {
            return value->GetStringValue().ToString();
        }

        // Preferred path: let the page's String() do the conversion so the
        // result matches JavaScript exactly (number formatting, arrays,
        // dates, custom toString, etc.).
        if (context) {
            CefRefPtr<CefV8Value> global = context->GetGlobal();
            CefRefPtr<CefV8Value> fn = global ? global->GetValue("String") : nullptr;
            if (fn && fn->IsFunction()) {
                CefV8ValueList call_args;
                call_args.push_back(value);
                CefRefPtr<CefV8Value> r = fn->ExecuteFunction(nullptr, call_args);
                if (r && r->IsString()) {
                    return r->GetStringValue().ToString();
                }
            }
        }

        // Fallback: String() unavailable, keep JS-like semantics as best we can.
        if (value->IsUndefined()) {
            return "undefined";
        }

        if (value->IsNull()) {
            return "null";
        }

        if (value->IsBool()) {
            return value->GetBoolValue() ? "true" : "false";
        }

        if (value->IsInt()) {
            return std::to_string(value->GetIntValue());
        }

        if (value->IsUInt()) {
            return std::to_string(value->GetUIntValue());
        }

        if (value->IsDouble()) {
            return std::to_string(value->GetDoubleValue());
        }

        if (value->IsArray()) {
            return "[Array]";
        }

        if (value->IsObject()) {
            return "[Object]";
        }

        if (value->IsFunction()) {
            return "[Function]";
        }

        return "[Unknown]";
    }

private:
    // Call using V8 API (in renderer process)
    static bool CallWithV8(CefRefPtr<CefFrame> frame,
                          const std::string& function_name,
                          const std::vector<std::string>& args) {
        CefRefPtr<CefV8Context> context = frame->GetV8Context();
        if (!context) {
            printf_log(LOG_SEVERITY_ERROR, "V8 context is null");
            return false;
        }

        if (!context->Enter()) {
            printf_log(LOG_SEVERITY_ERROR, "Failed to enter V8 context");
            return false;
        }

        bool success = false;

        //try {
            CefRefPtr<CefV8Value> global = context->GetGlobal();

            // Support nested function calls like "console.log"
            CefRefPtr<CefV8Value> func = GetNestedFunction(global, function_name);

            if (func && func->IsFunction()) {
                // Convert arguments to V8 values
                CefV8ValueList v8_args;
                for (const auto& arg : args) {
                    v8_args.push_back(CefV8Value::CreateString(arg));
                }

                // Execute function
                CefRefPtr<CefV8Value> result = func->ExecuteFunction(global, v8_args);

                if (result) {
                    success = true;
                    printf_log(LOG_SEVERITY_INFO, "V8 call succeeded: %s", function_name.c_str());
                } else {
                    printf_log(LOG_SEVERITY_ERROR, "V8 function execution failed: %s", function_name.c_str());
                }
            } else {
                printf_log(LOG_SEVERITY_ERROR, "Function not found or not callable: %s", function_name.c_str());
            }
        //} catch (...) {
        //    printf_log("Exception in V8 call: %s", function_name.c_str());
        //}

        context->Exit();
        return success;
    }

    // Call using ExecuteJavaScript (in browser process)
    static bool CallWithExecuteJavaScript(CefRefPtr<CefFrame> frame,
                                         const std::string& function_name,
                                         const std::vector<std::string>& args) {
        if (!frame) {
            printf_log(LOG_SEVERITY_ERROR, "Frame is null");
            return false;
        }

        // Build JavaScript call statement
        std::string script = BuildJavaScriptCall(function_name, args);

        printf_log(LOG_SEVERITY_INFO, "Executing JavaScript: %s", script.c_str());

        // Execute JavaScript
        frame->ExecuteJavaScript(script, frame->GetURL(), 0);

        return true;
    }

    // Build JavaScript call statement
    static std::string BuildJavaScriptCall(const std::string& function_name,
                                          const std::vector<std::string>& args) {
        std::string script = function_name + "(";

        for (size_t i = 0; i < args.size(); ++i) {
            if (i > 0) {
                script += ", ";
            }
            // Escape and add quotes
            script += "'" + EscapeJavaScriptString(args[i]) + "'";
        }

        script += ");";
        return script;
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