# Feature request: Add `OnBeforeResourceResponse` callback to `CefResourceRequestHandler` (response-side counterpart to `OnBeforeResourceLoad`)

## Motivation

`CefResourceRequestHandler` currently gives embedders a well-defined interception point for **request** headers (`OnBeforeResourceLoad`, where `CefRequest` may be modified) and for **redirects** (`OnResourceRedirect`, where only the new URL may be changed), but no interception point for **response** headers on requests handled by the default network loader:

- `OnResourceResponse()` — "The |response| object cannot be modified in this callback", and redirecting is deprecated.
- `GetResourceHandler()` — requires taking over the entire load (streaming, caching, auth, cookies, range requests) just to adjust response headers, which is heavy and error-prone for embedders who want the browser to perform the load as usual.

Chromium's network service internally exposes exactly this point via `network::ResourceRequestHandler::OnHeadersReceived` semantics, so the capability already exists in the stack — it is just not surfaced through the CEF delegate API. With the network service enabled (the only supported configuration), response-header modification is therefore out of reach for embedders.

## Proposal

Add a callback that mirrors Chromium's `OnHeadersReceived` interception point, symmetric to `OnBeforeResourceLoad` on the request side:

```cpp
///
/// Called on the IO thread after response headers are received and before
/// they are processed by CEF. The |browser| and |frame| values represent the
/// source of the request, and may be NULL for requests originating from
/// service workers or CefURLRequest. The |request| object cannot be modified
/// in this callback. The |response| object may be modified in this callback
/// to change the status code, status text, MIME type, charset or response
/// headers. Changes to other response properties will be ignored. The
/// |response| object will be read-only after this callback returns.
///
/// For responses received from the network this callback is executed before
/// CORS validation. Cached responses may be delivered after CORS validation
/// and modifications are therefore not guaranteed to affect CORS handling.
///
/*--cef(optional_param=browser,optional_param=frame)--*/
virtual void OnBeforeResourceResponse(CefRefPtr<CefBrowser> browser,
                                      CefRefPtr<CefFrame> frame,
                                      CefRefPtr<CefRequest> request,
                                      CefRefPtr<CefResponse> response) {}
```

Typical embedder use cases: adjusting response `Content-Type`/charset for legacy servers, adding or removing response headers (e.g. stripping `Set-Cookie` from specific hosts, adding headers consumed by the embedding application), and recording final response headers after redirect chains. Request interception via `GetResourceHandler()` continues to work unchanged; the new callback covers requests handled by the default network loader.

## Implementation

We have carried the complete implementation (header + generated C API + net_service plumbing) across branches 7871 and 8037 and it has proven stable in production. The changes are limited to three files:

**1. `include/cef_resource_request_handler.h`** — the declaration above, introduced via the official `translator.py` + `version_manager.py` flow (`added=next`).

**2. `libcef/browser/net_service/resource_request_handler_wrapper.cc`** — invoke the client callback at the `OnBeforeHeadersReceived`-equivalent point and propagate modified headers through the existing `ResponseMode` callback chain:

```diff
@@ class InterceptedRequestHandlerWrapper : public InterceptedRequestHandler {
       cookie_filter_ = nullptr;
       pending_request_ = pending_request;
       pending_response_ = nullptr;
+      override_response_headers_ = nullptr;
       request_ = request;
@@
     CefRefPtr<CefCookieAccessFilter> cookie_filter_;
     CefRefPtr<CefRequestImpl> pending_request_;
     CefRefPtr<CefResponseImpl> pending_response_;
+    scoped_refptr<net::HttpResponseHeaders> override_response_headers_;
     raw_ptr<network::ResourceRequest> request_;
@@ void InterceptedRequestHandlerWrapper::OnBeforeHeadersReceived(
     // (existing early-return and handler_ checks)
+    state->override_response_headers_ = nullptr;
@@
     if (state->pending_response_->GetResponseHeaders()) {
       state->pending_response_->SetResponseHeaders(*headers);
     }
 
+    const auto original_headers =
+        state->pending_response_->GetResponseHeaders();
+
+    state->handler_->OnBeforeResourceResponse(
+        init_state_->browser_, init_state_->GetFrame(),
+        state->pending_request_.get(), state->pending_response_.get());
+
+    const auto modified_headers =
+        state->pending_response_->GetResponseHeaders();
     state->pending_response_->SetReadOnly(true);
 
+    if (original_headers && modified_headers &&
+        original_headers->raw_headers() != modified_headers->raw_headers()) {
+      state->override_response_headers_ = modified_headers;
+    }
   }
@@ (both call sites that complete the request)
     auto exec_callback = base::BindOnce(
-        std::move(callback), ResponseMode::CONTINUE, nullptr, new_url);
+        std::move(callback), ResponseMode::CONTINUE,
+        std::move(state->override_response_headers_), new_url);
```

**3. `libcef/browser/net_service/proxy_url_loader_factory.cc`** — propagate the override headers on response paths that do not go through `OnHeadersReceived`:

```diff
@@ void InterceptedRequest::ContinueResponseOrRedirect(
   override_headers_ = override_headers;
   if (override_headers_) {
-    // Make sure to update current_response_, since when OnReceiveResponse
-    // is called we will not use its headers as it might be missing the
-    // Set-Cookie line (which gets stripped by the IPC layer).
-    current_response_->headers = override_headers_;
+    if (current_response_) {
+      // Preserve the override for response paths that do not use
+      // OnHeadersReceived.
+      current_response_->headers = override_headers_;
+    } else {
+      // Preserve the override for the subsequent OnReceiveResponse call.
+      current_headers_ = override_headers_;
+    }
   }
```

The implementation reuses the existing `ResponseMode`/override-headers plumbing that `OnBeforeResourceLoad` redirects already use, so no new network service interfaces are introduced — the client callback result is folded into the existing completion callbacks as override headers.

## Notes

- The before/after raw-header comparison keeps the hot path unchanged when the embedder does not modify anything.
- The CORS/cache caveats in the documentation are inherited from Chromium's `OnHeadersReceived` semantics, so embedder expectations match the underlying network service behavior.
- We can contribute the full patches rebased onto current master, or adjust the API shape/naming if maintainers prefer a different form (we followed the existing `OnBeforeResourceLoad`/`OnResourceResponse` naming conventions).

Happy to send a PR if this is something the maintainers would consider adopting.
