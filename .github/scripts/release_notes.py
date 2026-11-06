#!/usr/bin/env python3
"""Build GitHub release notes from the commits since the previous release tag.

Usage:
  release_notes.py --version v0.5 --tag-glob 'v*' --devices all --footer .github/release/all.md --out notes.md

Only the first-parent history of the branch is read, so a merge of upstream Bruce shows up
as one line (its subject) instead of hundreds of upstream commits.
"""
import argparse
import os
import re
import subprocess
import sys

DEVICE_PATHS = {
    "ats-mini": ("ATS Mini", ["boards/ats-mini/", "src/modules/fm/si4732_fm", "sd_files/interpreter/ats_mini",
                              "appstore/"]),
    "lighttracker-plus": ("LightTracker Plus", ["boards/lighttracker-plus/"]),
}
CI_PATHS = [".github/"]


def git(*args):
    return subprocess.run(["git", *args], check=True, capture_output=True, text=True).stdout.strip()


def previous_tag(glob, current):
    tags = [t for t in git("tag", "-l", glob, "--sort=-creatordate").splitlines() if t and t != current]
    for t in tags:
        if subprocess.run(["git", "merge-base", "--is-ancestor", t, "HEAD"]).returncode == 0:
            return t
    return None


def changed_files(sha):
    parents = git("rev-list", "--parents", "-n", "1", sha).split()[1:]
    if not parents:
        return git("show", "--name-only", "--pretty=format:", sha).splitlines()
    return git("diff", "--name-only", parents[0], sha).splitlines()


def body_bullets(sha):
    """'- ' lines from the commit message body (continuation lines joined), max 12."""
    body = git("log", "-1", "--pretty=format:%b", sha).splitlines()
    items, cur = [], None
    for raw in body:
        line = raw.rstrip()
        if line.startswith(("Co-Authored-By:", "Claude-Session:", "Signed-off-by:")):
            break
        if line.startswith("- "):
            if cur:
                items.append(cur)
            cur = line[2:].strip()
        elif cur is not None and line.startswith("  ") and line.strip():
            cur += " " + line.strip()
        else:
            if cur:
                items.append(cur)
            cur = None
    if cur:
        items.append(cur)
    return items[:12]


def is_upstream_merge(sha, subject):
    parents = git("rev-list", "--parents", "-n", "1", sha).split()[1:]
    return len(parents) > 1 and re.search(r"(BruceDevices|upstream|Bruce dev|Bruce main)", subject, re.I)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", required=True)
    ap.add_argument("--tag-glob", default="v*")
    ap.add_argument("--devices", default="all", help="all, or a comma list of device keys")
    ap.add_argument("--footer", default=None)
    ap.add_argument("--out", required=True)
    ap.add_argument("--file-version", default=None, help="version used in file names (default: --version)")
    a = ap.parse_args()

    devices = list(DEVICE_PATHS) if a.devices == "all" else [d.strip() for d in a.devices.split(",")]
    other_devices = [d for d in DEVICE_PATHS if d not in devices]
    repo = os.environ.get("GITHUB_REPOSITORY", "")

    prev = previous_tag(a.tag_glob, a.version)
    if not prev:
        # first release of this kind: start from the newest release of any kind instead
        prev = previous_tag("*", a.version)
    rng = f"{prev}..HEAD" if prev else "HEAD"
    log = git("log", "--first-parent", "--pretty=format:%H%x1f%s", "-n", "200" if prev else "30", rng)

    sections = {"upstream": [], "ci": [], "other": []}
    for d in devices:
        sections[d] = []

    for line in log.splitlines():
        if not line:
            continue
        sha, subject = line.split("\x1f", 1)
        files = changed_files(sha)
        short = sha[:7]
        link = f"[`{short}`](https://github.com/{repo}/commit/{sha})" if repo else f"`{short}`"
        entry = f"- {subject} ({link})"
        for b in body_bullets(sha):
            entry += f"\n  - {b}"

        if is_upstream_merge(sha, subject):
            sections["upstream"].append(entry)
            continue
        hit_dev = [d for d in devices if any(f.startswith(p) for f in files for p in DEVICE_PATHS[d][1])]
        only_other_dev = files and all(
            any(f.startswith(p) for p in sum((DEVICE_PATHS[o][1] for o in other_devices), [])) for f in files
        )
        if only_other_dev:
            continue  # change for a device that is not part of this release
        if hit_dev:
            for d in hit_dev:
                sections[d].append(entry)
        elif files and all(any(f.startswith(p) for p in CI_PATHS) for f in files):
            sections["ci"].append(entry)
        else:
            sections["other"].append(entry)

    out = []
    out.append(f"## What's changed since {prev}" if prev else "## What's changed")
    out.append("")
    any_change = False
    for d in devices:
        if sections[d]:
            any_change = True
            out.append(f"### {DEVICE_PATHS[d][0]}")
            out += sections[d]
            out.append("")
    titles = [("upstream", "Bruce core (upstream merge)"), ("other", "Core / shared"), ("ci", "Build & release")]
    for key, title in titles:
        if sections[key]:
            any_change = True
            out.append(f"### {title}")
            out += sections[key]
            out.append("")
    if not any_change:
        out.append("_No changes since the previous release (rebuild)._")
        out.append("")
    if prev and repo:
        out.append(f"**Full diff:** https://github.com/{repo}/compare/{prev}...{a.version}")
        out.append("")

    if a.footer:
        with open(a.footer, encoding="utf-8") as f:
            out.append("---")
            out.append("")
            out.append(f.read().replace("${VERSION}", a.file_version or a.version).rstrip())
            out.append("")

    with open(a.out, "w", encoding="utf-8") as f:
        f.write("\n".join(out))
    print("\n".join(out))


if __name__ == "__main__":
    sys.exit(main())
