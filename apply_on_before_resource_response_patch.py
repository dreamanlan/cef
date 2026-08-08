#!/usr/bin/env python3

import argparse
import re
import subprocess
import sys
from pathlib import Path


def read_normalized(path):
    if not path.is_file():
        raise RuntimeError(f"Required file does not exist: {path}")
    return path.read_text(encoding="utf-8").replace("\r\n", "\n")


def write_normalized(path, text):
    path.write_text(text, encoding="utf-8", newline="\n")


def replace_exact(path, old_text, new_text, description):
    text = read_normalized(path)
    if new_text in text:
        print(f"Already applied: {description}")
        return False

    count = text.count(old_text)
    if count != 1:
        raise RuntimeError(
            f"Expected exactly one source anchor for '{description}', "
            f"found {count} in {path}"
        )

    write_normalized(path, text.replace(old_text, new_text))
    print(f"Applied: {description}")
    return True


def run_python_tool(cef_root, tool_path, arguments):
    command = [sys.executable, str(tool_path), *arguments]
    print("Running:", " ".join(command))
    subprocess.run(command, cwd=cef_root, check=True)


def parse_arguments():
    parser = argparse.ArgumentParser(
        description=(
            "Apply the OnBeforeResourceResponse CEF patch and run the "
            "official CEF generation tools."
        )
    )
    parser.add_argument(
        "--cef-root",
        type=Path,
        default=Path(__file__).resolve().parent,
        help=(
            "CEF source root. Defaults to the directory containing this script."
        ),
    )
    parser.add_argument(
        "--force-generate",
        action="store_true",
        help=(
            "Run translator.py and version_manager.py even when no CEF_NEXT "
            "metadata is introduced."
        ),
    )
    return parser.parse_args()


def main():
    args = parse_arguments()
    cef_root = args.cef_root.expanduser().resolve()

    header_path = cef_root / "include/cef_resource_request_handler.h"
    wrapper_path = (
        cef_root
        / "libcef/browser/net_service/resource_request_handler_wrapper.cc"
    )
    proxy_path = (
        cef_root / "libcef/browser/net_service/proxy_url_loader_factory.cc"
    )
    translator_path = cef_root / "tools/translator.py"
    version_manager_path = cef_root / "tools/version_manager.py"

    for required_path in (
        header_path,
        wrapper_path,
        proxy_path,
        translator_path,
        version_manager_path,
    ):
        if not required_path.is_file():
            raise RuntimeError(f"Required file does not exist: {required_path}")

    source_changed = False
    introduced_next = False

    header_method_body = """\
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

"""

    versioned_header_method_body = """\
#if CEF_API_ADDED(CEF_NEXT)
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
  /*--cef(optional_param=browser,optional_param=frame,added=next)--*/
  virtual void OnBeforeResourceResponse(CefRefPtr<CefBrowser> browser,
                                        CefRefPtr<CefFrame> frame,
                                        CefRefPtr<CefRequest> request,
                                        CefRefPtr<CefResponse> response) {}
#endif

"""

    header_text = read_normalized(header_path)
    if "OnBeforeResourceResponse" not in header_text:
        resource_response_anchor = """\
  ///
  /// Called on the IO thread when a resource response is received. The
"""
        if replace_exact(
            header_path,
            resource_response_anchor,
            versioned_header_method_body + resource_response_anchor,
            "public API method",
        ):
            source_changed = True
            introduced_next = True
    elif header_method_body in header_text:
        if replace_exact(
            header_path,
            header_method_body,
            versioned_header_method_body,
            "public API version metadata",
        ):
            source_changed = True
            introduced_next = True
    else:
        guarded_method_pattern = re.compile(
            r"#if CEF_API_ADDED\((?:CEF_NEXT|\d+)\)\n"
            r".*?/\*--cef\([^\r\n]*added=(?:next|\d+)[^\r\n]*\)--\*/\n"
            r"\s*virtual void OnBeforeResourceResponse\(.*?\n#endif",
            re.DOTALL,
        )
        if not guarded_method_pattern.search(header_text):
            raise RuntimeError(
                "OnBeforeResourceResponse exists but does not have "
                "recognized CEF API version metadata."
            )

        if (
            "CEF_API_ADDED(CEF_NEXT)" in header_text
            and "added=next" in header_text
        ):
            introduced_next = True
            print("Already applied: public API method with NEXT metadata")
        else:
            print("Already applied: public API method with an exact API version")

    reset_old = """\
      pending_request_ = pending_request;
      pending_response_ = nullptr;
      request_ = request;
"""
    reset_new = """\
      pending_request_ = pending_request;
      pending_response_ = nullptr;
      override_response_headers_ = nullptr;
      request_ = request;
"""
    if replace_exact(
        wrapper_path,
        reset_old,
        reset_new,
        "reset response header override state",
    ):
        source_changed = True

    field_old = """\
    CefRefPtr<CefRequestImpl> pending_request_;
    CefRefPtr<CefResponseImpl> pending_response_;
    raw_ptr<network::ResourceRequest> request_;
"""
    field_new = """\
    CefRefPtr<CefRequestImpl> pending_request_;
    CefRefPtr<CefResponseImpl> pending_response_;
    scoped_refptr<net::HttpResponseHeaders> override_response_headers_;
    raw_ptr<network::ResourceRequest> request_;
"""
    if replace_exact(
        wrapper_path,
        field_old,
        field_new,
        "store response header override state",
    ):
        source_changed = True

    process_old = """\
    if (!state->handler_) {
      return;
    }

    if (!state->pending_response_) {
      state->pending_response_ = new CefResponseImpl();
    } else {
      state->pending_response_->SetReadOnly(false);
    }

    if (headers) {
      state->pending_response_->SetResponseHeaders(*headers);
    }

    state->pending_response_->SetReadOnly(true);
  }
"""
    process_new = """\
    state->override_response_headers_ = nullptr;

    if (!state->handler_) {
      return;
    }

    if (!state->pending_response_) {
      state->pending_response_ = new CefResponseImpl();
    } else {
      state->pending_response_->SetReadOnly(false);
    }

    if (headers) {
      state->pending_response_->SetResponseHeaders(*headers);
    }

    const auto original_headers =
        state->pending_response_->GetResponseHeaders();

    state->handler_->OnBeforeResourceResponse(
        init_state_->browser_, init_state_->GetFrame(),
        state->pending_request_.get(), state->pending_response_.get());

    const auto modified_headers =
        state->pending_response_->GetResponseHeaders();
    state->pending_response_->SetReadOnly(true);

    if (original_headers && modified_headers &&
        original_headers->raw_headers() != modified_headers->raw_headers()) {
      state->override_response_headers_ = modified_headers;
    }
  }
"""
    if replace_exact(
        wrapper_path,
        process_old,
        process_new,
        "invoke response callback and capture modified headers",
    ):
        source_changed = True

    redirect_old = """\
    auto exec_callback = base::BindOnce(
        std::move(callback), ResponseMode::CONTINUE, nullptr, new_url);
"""
    redirect_new = """\
    auto exec_callback = base::BindOnce(
        std::move(callback), ResponseMode::CONTINUE,
        std::move(state->override_response_headers_), new_url);
"""
    if replace_exact(
        wrapper_path,
        redirect_old,
        redirect_new,
        "forward redirect response header overrides",
    ):
        source_changed = True

    response_old = """\
    auto exec_callback =
        base::BindOnce(std::move(callback), response_mode, nullptr, new_url);
"""
    response_new = """\
    auto exec_callback =
        base::BindOnce(std::move(callback), response_mode,
                       std::move(state->override_response_headers_), new_url);
"""
    if replace_exact(
        wrapper_path,
        response_old,
        response_new,
        "forward normal response header overrides",
    ):
        source_changed = True

    proxy_old = """\
  override_headers_ = override_headers;
  if (override_headers_ && current_response_) {
    current_response_->headers = override_headers_;
  }
  redirect_url_ = redirect_url;
"""
    proxy_new = """\
  override_headers_ = override_headers;
  if (override_headers_) {
    if (current_response_) {
      // Preserve the override for response paths that do not use
      // OnHeadersReceived.
      current_response_->headers = override_headers_;
    } else {
      // Preserve the override for the subsequent OnReceiveResponse call.
      current_headers_ = override_headers_;
    }
  }
  redirect_url_ = redirect_url;
"""
    if replace_exact(
        proxy_path,
        proxy_old,
        proxy_new,
        "preserve overrides without current_response",
    ):
        source_changed = True

    if introduced_next or args.force_generate:
        run_python_tool(
            cef_root,
            translator_path,
            [
                "--root-dir",
                str(cef_root),
                "--classes",
                "CefResourceRequestHandler",
            ],
        )

        if introduced_next:
            run_python_tool(
                cef_root,
                version_manager_path,
                ["-a", "--replace-next"],
            )
        else:
            run_python_tool(cef_root, version_manager_path, ["-u"])
    else:
        print(
            "No NEXT metadata was introduced. "
            "Official generation was not required."
        )

    final_header = read_normalized(header_path)
    if "OnBeforeResourceResponse" not in final_header:
        raise RuntimeError(
            "Final verification failed: public API method is missing."
        )
    if (
        "CEF_API_ADDED(CEF_NEXT)" in final_header
        or "added=next" in final_header
    ):
        raise RuntimeError(
            "Final verification failed: NEXT metadata was not replaced."
        )

    capi_path = (
        cef_root / "include/capi/cef_resource_request_handler_capi.h"
    )
    capi_text = read_normalized(capi_path)
    if "on_before_resource_response" not in capi_text:
        raise RuntimeError(
            "Final verification failed: generated C API callback is missing."
        )

    print("Patch completed successfully.")
    print(f"Source changed: {source_changed}")
    print("Build was not started.")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"Error: {error}", file=sys.stderr)
        sys.exit(1)
