from pathlib import Path
from shutil import copy2
import hashlib
import os
import subprocess
import sys
import time
from uuid import uuid4

repo = Path(__file__).resolve().parent.parent
if repo.name != "EZ4Connectpro" or not (repo / ".git").is_dir():
    raise SystemExit("Run from the EZ4Connectpro development checkout")


def project_directory(root_name):
    root = (repo.parent / root_name).resolve(strict=True)
    if root != repo.parent / root_name:
        raise SystemExit(f"Unexpected delivery root: {root}")
    target = root / repo.name
    target.mkdir(exist_ok=True)
    if target.resolve(strict=True) != target:
        raise SystemExit(f"Unexpected project destination: {target}")
    return target


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def copy_verified(source, target, root):
    if not target.resolve().is_relative_to(root):
        raise SystemExit(f"Unexpected path: {target}")
    target.parent.mkdir(parents=True, exist_ok=True)
    copy2(source, target)
    if digest(source) != digest(target):
        raise SystemExit(f"Copy mismatch: {target}")


started = time.monotonic()
candidate = repo / "out" / "EZ4Connectpro-windows-x64.exe"
if len(sys.argv) != 2:
    raise SystemExit("Usage: sync-delivery.py VERIFIED_SHA256")
if digest(candidate) != sys.argv[1].lower():
    raise SystemExit("Candidate hash mismatch")

source_root = project_directory("Github_Source")
release_root = project_directory("Github_Releases")
retired = {
    "infrastructure/coreprocess/openvpnconfig.cpp",
    "infrastructure/coreprocess/openvpnconfig.h",
    "infrastructure/coreprocess/openvpnprocess.cpp",
    "infrastructure/coreprocess/openvpnprocess.h",
    "infrastructure/platform/externalvpnclient.cpp",
    "infrastructure/platform/externalvpnclient.h",
    "resource/icon.rc",
    "scripts/stage-openvpn-windows.py",
    "tests/openvpnconfig_test.cpp",
}
retired.update(p for p in subprocess.check_output(
    ["git", "ls-files", "--deleted", "-z"], cwd=repo,
).decode("utf-8").split("\0") if p)
for relative in retired:
    if (repo / relative).exists():
        raise SystemExit(f"Retired source is still present in project: {relative}")
    destination = source_root / relative
    if destination.resolve() != destination or not destination.is_relative_to(source_root):
        raise SystemExit(f"Unexpected retired source path: {destination}")
relative_paths = subprocess.check_output(
    ["git", "ls-files", "-z", "--cached", "--others", "--exclude-standard"], cwd=repo,
).decode("utf-8").split("\0")
files = {Path(p) for p in relative_paths if p}
copied = 0
for relative in sorted(files):
    if relative.is_absolute() or any(part in {".git", "build", "out"} for part in relative.parts):
        raise SystemExit(f"Unexpected source entry: {relative}")
    source = repo / relative
    if source.is_file() and source.resolve().is_relative_to(repo):
        copy_verified(source, source_root / relative, source_root)
        copied += 1
for relative in retired:
    (source_root / relative).unlink(missing_ok=True)

artifact = release_root / candidate.name
if artifact.resolve() != artifact or any(path != artifact for path in release_root.iterdir()):
    raise SystemExit("Expected one canonical EXE in the release directory")
staged = release_root / f".{artifact.name}.{uuid4().hex}.tmp"
try:
    copy_verified(candidate, staged, release_root)
    os.replace(staged, artifact)
finally:
    staged.unlink(missing_ok=True)
sha = digest(artifact)
if sha != digest(candidate) or list(release_root.iterdir()) != [artifact]:
    raise SystemExit("Canonical release verification failed")
print(f"{source_root} ({copied} files)")
print(f"{artifact} ({artifact.stat().st_size} bytes)")
print(f"SHA-256 {sha}")
print(f"Delivery seconds: {time.monotonic() - started:.2f}")
