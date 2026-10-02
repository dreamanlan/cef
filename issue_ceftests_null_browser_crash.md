# ceftests: crash in `RedirectResponseTest` — `OnBeforeResourceLoad` dereferences a possibly-NULL browser

## Environment

- CEF branch 8037 (Chromium 154); the same test code exists unchanged on current master
- macOS arm64, ceftests (release build)
- Deterministic in our environment; the crash reproduces on every run of the affected tests

## Steps to reproduce

```bash
autoninja -C out/Release_GN_arm64 ceftests
out/Release_GN_arm64/ceftests.app/Contents/MacOS/ceftests \
  --gtest_filter='ResourceRequestHandlerTest.RedirectURLViaContext'
```

In our environment the crash also occurs when running the full `ResourceRequestHandlerTest.*` suite: 149 tests pass, then the process dies in the first `*ViaContext` test.

## Result

```
./ceftests: Segmentation fault: 11
```

Crash report (faulting thread = `Chrome_IOThread`):

```
0-3  CefBrowserCToCpp::GetIdentifier()                    ← this == nullptr
4    RedirectResponseTest::ResourceRequestHandler::OnBeforeResourceLoad
5    resource_request_handler_on_before_resource_load(...)
6    CefResourceRequestHandler_CToCpp::OnBeforeResourceLoad
7    InterceptedRequestHandlerWrapper::ShouldInterceptRequest
8    InterceptedRequest::BeforeRequestReceived
11   InterceptedRequest::Restart()
12   ProxyURLLoaderFactory::CreateLoaderAndStart
```

`EXC_BAD_ACCESS (SIGSEGV), KERN_INVALID_ADDRESS at 0xfffffffffffffff0` — i.e. `nullptr - 0x10`: the C++ wrapper object is null and `GetIdentifier()` reads wrapper metadata at a negative offset.

## Root cause

`OnBeforeResourceLoad` was called with a NULL `browser` value. This is **valid per the API contract** — the header documentation states:

> "The |browser| and |frame| values represent the source of the request, and may be NULL for requests originating from service workers or CefURLRequest."

Requests can also arrive without an associated frame/browser when racing page navigation or close (see the comment in commit `0da9e7222`, "Fix service worker request handler fallback (fixes #3593)": "Service workers can run in a process without any registered frames, including after their originating page has navigated or closed").

The test handler in `tests/ceftests/resource_request_handler_unittest.cc` dereferences `browser` without a null check:

```cpp
cef_return_value_t OnBeforeResourceLoad(...) override {
  ...
  EXPECT_EQ(test_->browser_id_, browser->GetIdentifier());   // ← crashes when browser is NULL
```

Note that some other callbacks in the same class already contain `EXPECT_TRUE(browser.get())` — acknowledging that the value can be NULL — but without an early return, so they would crash the same way; and `OnBeforeResourceLoad` has no check at all.

## Suggested fix

Guard all callbacks of `RedirectResponseTest::ResourceRequestHandler` that dereference `browser`, treating a NULL browser like the existing favicon handling (ignore/cancel). We carry the following locally (with this change our full `ResourceRequestHandlerTest.*` suite passes 169/169):

```diff
--- a/tests/ceftests/resource_request_handler_unittest.cc
+++ b/tests/ceftests/resource_request_handler_unittest.cc
@@ void OnBeforeResourceLoad(...) override {
       if (request->GetResourceType() == RT_FAVICON) {
         // Ignore favicon requests.
         return RV_CANCEL;
       }

+      if (!browser.get()) {
+        // The browser value may be NULL for requests originating from
+        // service workers or CefURLRequest, or racing page navigation/close.
+        // See the OnBeforeResourceLoad documentation.
+        return RV_CANCEL;
+      }
+
       EXPECT_EQ(test_->browser_id_, browser->GetIdentifier());
```

The same pattern applies to `GetResourceHandler`, `OnResourceRedirect`, `OnResourceResponse`, `GetResourceResponseFilter` and `OnResourceLoadComplete` (replacing the `EXPECT_TRUE(browser.get())` calls that lack an early return).

Happy to send a PR with the complete fix if that is useful.
