#!/usr/bin/env python3
"""Fetch, verify and check the vendored libraries listed in deps/manifest.json.

deps/manifest.json is the single source of truth (specification section 4.4.3):
each dependency records its exact source (git commit or archive URL with SHA-256),
the files kept from that source, the patches applied on top, and the post-patch
SHA-256 of every vendored file.

Usage:
    python deps/update_deps.py --pin [NAME ...]      resolve refs to commits, record archive digests
    python deps/update_deps.py --update [NAME ...]   fetch into staging, verify, patch, replace deps/<name>/
    python deps/update_deps.py --check               offline verification of pins, digests and stray files
    python deps/update_deps.py --verify-upstream     compare recorded commits with upstream refs (read-only)

A failed download or patch never leaves a half-updated tree: everything happens in a
staging directory and deps/<name>/ is replaced only after every step succeeded.
"""
from __future__ import annotations

import argparse
import fnmatch
import hashlib
import io
import json
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
import zipfile
from pathlib import Path

DEPS_DIR = Path(__file__).resolve().parent
ROOT_DIR = DEPS_DIR.parent
MANIFEST_PATH = DEPS_DIR / "manifest.json"
PATCHES_DIR = ROOT_DIR / "patches"

# Files in deps/ that belong to the vendoring machinery rather than to a library.
OWN_FILES = {"manifest.json", "update_deps.py", "README.md", "CMakeLists.txt"}


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def load_manifest() -> dict:
    return json.loads(MANIFEST_PATH.read_text(encoding="utf-8"))


def save_manifest(manifest: dict) -> None:
    text = json.dumps(manifest, indent=2, ensure_ascii=False) + "\n"
    temporary = MANIFEST_PATH.with_suffix(".json.tmp")
    temporary.write_text(text, encoding="utf-8", newline="\n")
    temporary.replace(MANIFEST_PATH)


def archive_url(source: dict) -> str:
    if source["type"] == "github":
        return f"https://codeload.github.com/{source['repo']}/tar.gz/{source['commit']}"
    return source["url"]


def resolve_commit(repo: str, ref: str) -> str:
    """Return the commit a tag or branch points at; peeled tags win over tag objects."""
    output = subprocess.run(
        ["git", "ls-remote", f"https://github.com/{repo}", ref, f"{ref}^{{}}"],
        check=True, capture_output=True, text=True).stdout
    lines = [line.split("\t") for line in output.strip().splitlines() if line]
    if not lines:
        raise RuntimeError(f"{repo}: ref {ref!r} not found upstream")
    peeled = [sha for sha, name in lines if name.endswith("^{}")]
    return peeled[0] if peeled else lines[0][0]


def download(url: str) -> bytes:
    request = urllib.request.Request(url, headers={"User-Agent": "mwb-update-deps"})
    with urllib.request.urlopen(request, timeout=120) as response:
        return response.read()


def archive_members(data: bytes, url: str) -> dict[str, bytes]:
    """Return {relative path: bytes} with the archive's single top directory stripped."""
    members: dict[str, bytes] = {}
    if url.endswith(".zip"):
        with zipfile.ZipFile(io.BytesIO(data)) as archive:
            for info in archive.infolist():
                if not info.is_dir():
                    members[info.filename] = archive.read(info)
    else:
        with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as archive:
            for info in archive.getmembers():
                if info.isfile():
                    extracted = archive.extractfile(info)
                    assert extracted is not None
                    members[info.name] = extracted.read()
    for name in members:
        if name.startswith("/") or ".." in Path(name).parts:
            raise RuntimeError(f"unsafe archive member {name!r}")
    tops = {name.split("/", 1)[0] for name in members}
    if len(tops) == 1 and all("/" in name for name in members):
        top = tops.pop() + "/"
        members = {name[len(top):]: content for name, content in members.items()}
    return members


def selected(path: str, patterns: list[str]) -> bool:
    return any(fnmatch.fnmatchcase(path, pattern) for pattern in patterns)


def pin(dependency: dict) -> None:
    source = dependency["source"]
    if source["type"] == "github" and not source.get("commit"):
        source["commit"] = resolve_commit(source["repo"], source["ref"])
        print(f"{dependency['name']}: {source['ref']} -> {source['commit']}")
    if not source.get("archive_sha256"):
        data = download(archive_url(source))
        source["archive_sha256"] = sha256_bytes(data)
        print(f"{dependency['name']}: archive sha256 {source['archive_sha256']}")


def update(dependency: dict) -> None:
    name = dependency["name"]
    source = dependency["source"]
    if source["type"] == "github" and not source.get("commit"):
        raise RuntimeError(f"{name}: no commit pinned; run --pin first")
    url = archive_url(source)
    data = download(url)
    actual = sha256_bytes(data)
    if actual != source.get("archive_sha256"):
        raise RuntimeError(f"{name}: archive digest {actual} does not match the pin "
                           f"{source.get('archive_sha256')} (moved tag or tampered download)")
    members = archive_members(data, url)
    keep = {path: content for path, content in members.items()
            if selected(path, dependency["include"])}
    if not keep:
        raise RuntimeError(f"{name}: include patterns matched no files")
    with tempfile.TemporaryDirectory(prefix=f"mwb-dep-{name}-") as staging_name:
        staging = Path(staging_name) / name
        for path, content in keep.items():
            target = staging / path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(content)
        for patch in dependency.get("patches", []):
            patch_path = PATCHES_DIR / name / patch
            subprocess.run(["git", "apply", "--whitespace=nowarn", "-p1", str(patch_path)],
                           cwd=staging, check=True)
        files = {}
        for file_path in sorted(staging.rglob("*")):
            if file_path.is_file():
                files[file_path.relative_to(staging).as_posix()] = sha256_file(file_path)
        destination = DEPS_DIR / name
        if destination.exists():
            shutil.rmtree(destination)
        shutil.copytree(staging, destination)
    dependency["files"] = files
    print(f"{name}: {len(files)} files vendored")


def check(manifest: dict) -> list[str]:
    problems: list[str] = []
    names = set()
    for dependency in manifest["dependencies"]:
        name = dependency["name"]
        names.add(name)
        source = dependency["source"]
        if source["type"] == "github" and len(source.get("commit", "")) != 40:
            problems.append(f"{name}: source commit is not pinned")
        if len(source.get("archive_sha256", "")) != 64:
            problems.append(f"{name}: archive digest is not pinned")
        for required in ("purpose", "version", "license", "homepage", "license_file"):
            if not dependency.get(required):
                problems.append(f"{name}: manifest field {required!r} is empty")
        for patch in dependency.get("patches", []):
            if not (PATCHES_DIR / name / patch).is_file():
                problems.append(f"{name}: listed patch {patch} is missing")
        directory = DEPS_DIR / name
        recorded = dependency.get("files", {})
        if not recorded:
            problems.append(f"{name}: no vendored files recorded")
        if dependency.get("license_file") not in recorded:
            problems.append(f"{name}: license file {dependency.get('license_file')} is not vendored")
        for path, digest in recorded.items():
            file_path = directory / path
            if not file_path.is_file():
                problems.append(f"{name}: missing vendored file {path}")
            elif sha256_file(file_path) != digest:
                problems.append(f"{name}: {path} differs from its recorded digest (edited by hand?)")
        if directory.is_dir():
            for file_path in directory.rglob("*"):
                if file_path.is_file():
                    relative = file_path.relative_to(directory).as_posix()
                    if relative not in recorded:
                        problems.append(f"{name}: stray file {relative}")
    for entry in DEPS_DIR.iterdir():
        if entry.is_dir() and entry.name not in names and not entry.name.startswith("."):
            problems.append(f"stray directory deps/{entry.name}")
        if entry.is_file() and entry.name not in OWN_FILES:
            problems.append(f"stray file deps/{entry.name}")
    for patch_directory in PATCHES_DIR.iterdir() if PATCHES_DIR.is_dir() else []:
        if patch_directory.is_dir() and patch_directory.name not in names:
            problems.append(f"patches/{patch_directory.name} belongs to no dependency")
    return problems


def verify_upstream(manifest: dict) -> list[str]:
    problems = []
    for dependency in manifest["dependencies"]:
        source = dependency["source"]
        if source["type"] != "github" or source.get("ref") in (None, "", "HEAD"):
            continue
        upstream = resolve_commit(source["repo"], source["ref"])
        if upstream != source.get("commit"):
            problems.append(f"{dependency['name']}: {source['ref']} now points at {upstream}, "
                            f"pinned {source.get('commit')} (moved tag)")
    return problems


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--pin", action="store_true")
    mode.add_argument("--update", action="store_true")
    mode.add_argument("--check", action="store_true")
    mode.add_argument("--verify-upstream", action="store_true")
    parser.add_argument("names", nargs="*")
    arguments = parser.parse_args()

    manifest = load_manifest()
    chosen = [d for d in manifest["dependencies"] if not arguments.names or d["name"] in arguments.names]
    unknown = set(arguments.names) - {d["name"] for d in manifest["dependencies"]}
    if unknown:
        print(f"unknown dependencies: {', '.join(sorted(unknown))}", file=sys.stderr)
        return 1

    if arguments.check:
        problems = check(manifest)
        for problem in problems:
            print(f"error: {problem}", file=sys.stderr)
        print(f"checked {len(manifest['dependencies'])} dependencies: "
              f"{'OK' if not problems else f'{len(problems)} problem(s)'}")
        return 1 if problems else 0
    if arguments.verify_upstream:
        problems = verify_upstream(manifest)
        for problem in problems:
            print(f"error: {problem}", file=sys.stderr)
        print("upstream: " + ("OK" if not problems else f"{len(problems)} problem(s)"))
        return 1 if problems else 0

    action = pin if arguments.pin else update
    failures = 0
    for dependency in chosen:
        try:
            action(dependency)
        except Exception as error:  # report every failing dependency, keep the others
            failures += 1
            print(f"error: {dependency['name']}: {error}", file=sys.stderr)
        save_manifest(manifest)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
