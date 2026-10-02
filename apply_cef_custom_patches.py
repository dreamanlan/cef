#!/usr/bin/env python3

# Apply custom CEF source patches used by this project:
#   1. OnBeforeResourceResponse: a new CefResourceRequestHandler callback that
#      lets clients inspect/modify response headers before CEF processes them.
#      This one introduces a brand-new CEF API, so it is handled by bespoke
#      logic below (insert the added=next versioned method, then run the
#      official CEF generation tools). It cannot be a static unified diff
#      because the generation tools rewrite added=next into a concrete,
#      environment-determined API version number.
#   2. Every *.patch file under myapp/patch/: static unified diffs that do not
#      introduce any CEF API (e.g. the use-chrome-window window.open->tab
#      merge, the response-header override plumbing in the .cc files). These
#      are applied with `git apply` and are idempotent: if a patch already
#      reverse-applies cleanly it is considered applied and skipped. When a
#      patch is not yet applied and the tree is git-managed, the files it
#      touches are first restored to their HEAD state, so stale working-tree
#      edits (e.g. an older revision of the same patch) cannot block it.
#
# CEF has no built-in mechanism to auto-apply patches against its own source
# (only against Chromium), so this script performs the edits directly.
#
# To add a new static patch later, just drop a *.patch file into myapp/patch/
# (e.g. `git diff -- path/to/file > myapp/patch/NN-name.patch`). No code change
# is needed here or in the PowerShell script.

import argparse
import re
import subprocess
import sys
from pathlib import Path

PATCH_SUBDIR = "myapp/patch"


def read_normalized(path):
    if not path.is_file():
        raise RuntimeError(f"Required file does not exist: {path}")
    return path.read_text(encoding="utf-8").replace("\r\n", "\n")


def write_normalized(path, text):
    with path.open("w", encoding="utf-8", newline="\n") as file:
        file.write(text)


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


def git_apply_ok(cef_root, extra_args):
    # --ignore-whitespace makes git apply tolerate line-ending (CRLF vs LF)
    # differences. On Windows core.autocrlf checks out source as CRLF while the
    # *.patch files are LF, and without this the context lines fail to match
    # (e.g. BUILD.gn hunk @@ -306 ...). It is a no-op where line endings match.
    result = subprocess.run(
        ["git", "apply", "--ignore-whitespace", *extra_args],
        cwd=cef_root,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    return result.returncode == 0, result.stderr.decode("utf-8", "replace")


DIFF_GIT_LINE_RE = re.compile(r"^diff --git (\S+) (\S+)$")


def is_git_worktree(cef_root):
    try:
        result = subprocess.run(
            ["git", "rev-parse", "--is-inside-work-tree"],
            cwd=cef_root,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
    except OSError:
        return False
    return result.returncode == 0 and result.stdout.strip() == b"true"


def get_patch_target_files(patch_path):
    """Extract the file paths that a patch modifies from its headers.

    Handles both standard "a/... b/..." prefixes and --no-prefix diffs.
    """
    targets = []
    text = patch_path.read_text(encoding="utf-8", errors="replace")
    for line in text.splitlines():
        match = DIFF_GIT_LINE_RE.match(line)
        if not match:
            continue
        old_path, new_path = match.groups()
        if old_path.startswith("a/") and new_path.startswith("b/"):
            old_path = old_path[2:]
            new_path = new_path[2:]
        for path in (old_path, new_path):
            if path != "/dev/null" and path not in targets:
                targets.append(path)
    return targets


def restore_patch_targets_to_head(cef_root, patch_path):
    """Restore the files a patch touches to their git HEAD state.

    Only files that are tracked by git and actually differ from HEAD are
    restored; everything else is left alone. Returns the list of restored
    paths (empty when there was nothing to restore).

    This makes `git apply` robust against stale working-tree changes such as
    an older revision of the same patch. Note: it assumes that no two patches
    under myapp/patch/ modify the same file, otherwise a later restore would
    undo an earlier patch.
    """
    targets = get_patch_target_files(patch_path)
    if not targets:
        return []

    result = subprocess.run(
        ["git", "ls-files", "--", *targets],
        cwd=cef_root,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    # Only restore paths exactly as written in the patch; if the repo root is
    # not the CEF root, git rewrites them with a different prefix.
    tracked = [
        path
        for path in result.stdout.decode("utf-8", "replace").splitlines()
        if path in targets
    ]
    if not tracked:
        return []

    status = subprocess.run(
        ["git", "status", "--porcelain", "--", *tracked],
        cwd=cef_root,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    dirty = [
        line[3:].split(" -> ")[-1]
        for line in status.stdout.decode("utf-8", "replace").splitlines()
        if line
    ]
    if not dirty:
        return []

    subprocess.run(
        ["git", "checkout", "HEAD", "--", *dirty],
        cwd=cef_root,
        check=True,
    )
    return dirty


def apply_patch_file(cef_root, patch_path, git_managed):
    """Idempotently apply a single unified-diff patch via git apply.

    Returns True when the patch was freshly applied, False when it was already
    applied. When the patch is not already applied and the tree is
    git-managed, the files it touches are first restored to their HEAD state
    so that stale working-tree edits cannot block the application. Raises
    RuntimeError when the patch still cannot be applied (e.g. the source
    anchor changed upstream).
    """
    description = patch_path.name
    patch_arg = str(patch_path)

    reverse_ok, _ = git_apply_ok(
        cef_root, ["--reverse", "--check", patch_arg]
    )
    if reverse_ok:
        print(f"Already applied: {description}")
        return False

    if git_managed:
        restored = restore_patch_targets_to_head(cef_root, patch_path)
        if restored:
            print(
                f"Restored to HEAD before applying: {description} "
                f"({', '.join(restored)})"
            )

    forward_ok, forward_err = git_apply_ok(cef_root, ["--check", patch_arg])
    if not forward_ok:
        raise RuntimeError(
            f"Patch does not apply and is not already applied: {description}\n"
            f"{forward_err.strip()}"
        )

    applied_ok, applied_err = git_apply_ok(cef_root, [patch_arg])
    if not applied_ok:
        raise RuntimeError(
            f"git apply failed for {description}\n{applied_err.strip()}"
        )
    print(f"Applied: {description}")
    return True


def apply_static_patches(cef_root, git_managed):
    patch_dir = cef_root / PATCH_SUBDIR
    if not patch_dir.is_dir():
        raise RuntimeError(f"Patch directory does not exist: {patch_dir}")

    patch_files = sorted(patch_dir.glob("*.patch"))
    if not patch_files:
        print(f"No static patches found under {patch_dir}")
        return False

    source_changed = False
    for patch_path in patch_files:
        if apply_patch_file(cef_root, patch_path, git_managed):
            source_changed = True
    return source_changed


def parse_arguments():
    parser = argparse.ArgumentParser(
        description=(
            "Apply custom CEF source patches: the OnBeforeResourceResponse API "
            "callback (with official CEF generation) plus every static unified "
            "diff under myapp/patch/."
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


def apply_api_header_patch(header_path):
    """Apply the OnBeforeResourceResponse API edit.

    Returns (source_changed, introduced_next). This edit introduces a new CEF
    API, so it is kept as bespoke logic: generation tools rewrite added=next
    into a concrete version number, which a static diff cannot express.
    """
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
            return True, True
        return False, True

    if header_method_body in header_text:
        if replace_exact(
            header_path,
            header_method_body,
            versioned_header_method_body,
            "public API version metadata",
        ):
            return True, True
        return False, True

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

    if "CEF_API_ADDED(CEF_NEXT)" in header_text and "added=next" in header_text:
        print("Already applied: public API method with NEXT metadata")
        return False, True

    print("Already applied: public API method with an exact API version")
    return False, False


def main():
    args = parse_arguments()
    cef_root = args.cef_root.expanduser().resolve()

    header_path = cef_root / "include/cef_resource_request_handler.h"
    translator_path = cef_root / "tools/translator.py"
    version_manager_path = cef_root / "tools/version_manager.py"

    for required_path in (header_path, translator_path, version_manager_path):
        if not required_path.is_file():
            raise RuntimeError(f"Required file does not exist: {required_path}")

    git_managed = is_git_worktree(cef_root)
    if not git_managed:
        print(
            "Not a git worktree: patch target files will not be "
            "restored to HEAD before applying."
        )

    source_changed = False
    introduced_next = False

    # 1. The OnBeforeResourceResponse API edit (bespoke; may trigger CEF
    #    generation tools below).
    header_changed, introduced_next = apply_api_header_patch(header_path)
    source_changed = source_changed or header_changed

    # 2. Every static unified diff under myapp/patch/. These never introduce a
    #    CEF API, so they never set introduced_next.
    if apply_static_patches(cef_root, git_managed):
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
