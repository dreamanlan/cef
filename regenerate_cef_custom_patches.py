#!/usr/bin/env python3

# Regenerate myapp/patch/*.patch from the current working tree.
#
# Use this AFTER upgrading CEF and re-running apply_cef_custom_patches.py, to
# refresh each patch's line numbers and context against the new upstream source
# (i.e. fix line-number drift). Each patch is re-exported via `git diff` for the
# exact file(s) that patch touches.
#
# Typical upgrade workflow:
#   1. Upgrade / re-sync the CEF source tree.
#   2. python3 ./apply_cef_custom_patches.py
#      (git apply --ignore-whitespace tolerates drifted line numbers and still
#       applies the changes onto the new source.)
#   3. python3 ./regenerate_cef_custom_patches.py
#      (re-export every patch so its hunks match the new source exactly.)
#   4. Review `git diff -- myapp/patch` and commit the refreshed patches.
#
# Safety:
#   * A patch is only regenerated when it is currently applied (its reverse
#     diff applies cleanly, ignoring whitespace) AND `git diff` for its files is
#     non-empty. Otherwise the patch file is left untouched, so an un-applied
#     patch is never clobbered with empty content.
#   * The OnBeforeResourceResponse API header edit is NOT a static patch (it is
#     handled by apply_cef_custom_patches.py plus the official CEF generators),
#     so it is intentionally ignored here.

import argparse
import re
import subprocess
import sys
from pathlib import Path

PATCH_SUBDIR = "myapp/patch"
DIFF_GIT_RE = re.compile(r"^diff --git a/.+? b/(.+)$")


def run_git(cef_root, arguments):
    return subprocess.run(
        ["git", *arguments],
        cwd=cef_root,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )


def read_normalized(path):
    return path.read_text(encoding="utf-8").replace("\r\n", "\n")


def parse_target_paths(patch_text):
    """Extract the repo-relative target file paths from a git-style patch."""
    paths = []
    for line in patch_text.splitlines():
        match = DIFF_GIT_RE.match(line)
        if match:
            paths.append(match.group(1))
    # Preserve order while removing duplicates.
    seen = set()
    unique = []
    for path in paths:
        if path not in seen:
            seen.add(path)
            unique.append(path)
    return unique


def is_applied(cef_root, patch_path):
    result = run_git(
        cef_root,
        ["apply", "--ignore-whitespace", "--reverse", "--check", str(patch_path)],
    )
    return result.returncode == 0


def export_diff(cef_root, paths):
    result = run_git(cef_root, ["--no-pager", "diff", "--", *paths])
    if result.returncode != 0:
        raise RuntimeError(
            "git diff failed:\n"
            + result.stderr.decode("utf-8", "replace").strip()
        )
    return result.stdout.decode("utf-8", "replace").replace("\r\n", "\n")


def parse_arguments():
    parser = argparse.ArgumentParser(
        description=(
            "Regenerate myapp/patch/*.patch from the current working tree to "
            "fix line-number drift after a CEF upgrade. Run apply first."
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
    return parser.parse_args()


def main():
    args = parse_arguments()
    cef_root = args.cef_root.expanduser().resolve()

    patch_dir = cef_root / PATCH_SUBDIR
    if not patch_dir.is_dir():
        raise RuntimeError(f"Patch directory does not exist: {patch_dir}")

    patch_files = sorted(patch_dir.glob("*.patch"))
    if not patch_files:
        print(f"No patches found under {patch_dir}")
        return

    updated = []
    unchanged = []
    skipped = []

    for patch_path in patch_files:
        name = patch_path.name
        old_text = read_normalized(patch_path)
        target_paths = parse_target_paths(old_text)

        if not target_paths:
            print(f"Skipped (no 'diff --git' target found): {name}")
            skipped.append(name)
            continue

        if not is_applied(cef_root, patch_path):
            print(
                f"Skipped (not currently applied, won't overwrite): {name}"
            )
            skipped.append(name)
            continue

        new_text = export_diff(cef_root, target_paths)
        if not new_text.strip():
            print(f"Skipped (git diff is empty, won't overwrite): {name}")
            skipped.append(name)
            continue

        if new_text == old_text:
            print(f"Unchanged: {name}")
            unchanged.append(name)
            continue

        with patch_path.open("w", encoding="utf-8", newline="\n") as file:
            file.write(new_text)
        print(f"Regenerated: {name}")
        updated.append(name)

    print("")
    print(
        f"Done. Regenerated: {len(updated)}, "
        f"unchanged: {len(unchanged)}, skipped: {len(skipped)}."
    )
    if skipped:
        print(
            "Review skipped patches manually (a skipped patch usually means it "
            "was not applied; run apply_cef_custom_patches.py first)."
        )
    if updated:
        print("Review `git diff -- myapp/patch` and commit the refreshed patches.")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError) as error:
        print(f"Error: {error}", file=sys.stderr)
        sys.exit(1)
