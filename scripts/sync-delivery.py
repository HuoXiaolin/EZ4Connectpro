import argparse
from datetime import date, datetime, timedelta, timezone
import hashlib
import io
import json
import os
from pathlib import Path
import re
from shutil import copy2, copyfileobj, rmtree
import struct
import subprocess
import tempfile
import time
from uuid import uuid4
import zipfile


REPO = Path(__file__).resolve().parent.parent
VERSION_FILE = Path("version.json")
REPORT_FILE = Path("docs/verification.json")
BINARY_NAME = "EZ4Connectpro-windows-x64.exe"
BEIJING = timezone(timedelta(hours=8))
RETIRED = {
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


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def json_bytes(data):
    return (json.dumps(data, ensure_ascii=False, indent=2) + "\n").encode("utf-8")


def release_plan(state, today):
    version = state["version"]
    if not re.fullmatch(r"(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)", version):
        raise ValueError("version.json must contain a three-part version")
    major, minor, patch = map(int, version.split("."))
    last_value = state["last_successful_release_date"]
    last_release = date.fromisoformat(last_value) if last_value else date.min
    release_date = max(today, last_release)
    if last_release < today:
        if patch >= 9:
            minor += 1
            patch = 0
        else:
            patch += 1
    if max(major, minor, patch) > 65535:
        raise ValueError("A Windows version component exceeds 65535")
    return {"version": f"{major}.{minor}.{patch}", "date": release_date.isoformat()}


def project_directory(root_name):
    root = REPO.parent / root_name
    if root.resolve(strict=True) != root:
        raise ValueError(f"Unexpected delivery root: {root}")
    target = root / REPO.name
    if target.resolve() != target:
        raise ValueError(f"Unexpected project destination: {target}")
    target.mkdir(exist_ok=True)
    return target


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def git_paths(*options):
    output = subprocess.check_output(["git", "ls-files", "-z", *options], cwd=REPO)
    return {Path(name) for name in output.decode("utf-8").split("\0") if name}


def destination(root, relative):
    if relative.is_absolute() or any(part in {"..", ".git", "build", "out"} for part in relative.parts):
        raise ValueError(f"Unexpected source entry: {relative}")
    target = root / relative
    if target.resolve() != target:
        raise ValueError(f"Unexpected source path: {target}")
    return target


def source_changes(source_root, deferred):
    retired = {Path(name) for name in RETIRED} | git_paths("--deleted")
    files = git_paths("--cached", "--others", "--exclude-standard")
    files = sorted(relative for relative in files if (REPO / relative).is_file())
    changes = {}
    for relative in files:
        source = destination(REPO, relative)
        target = destination(source_root, relative)
        if relative not in deferred:
            changes[target] = source
    for relative in retired:
        if destination(REPO, relative).exists():
            raise ValueError(f"Retired source is still present in project: {relative}")
        target = destination(source_root, relative)
        if target.exists():
            changes[target] = None
    return changes, len(files)


def portable_facts(candidate):
    # The Go launcher stores the package ZIP in one of its executable sections.
    executable = candidate.read_bytes()
    position = executable.find(b"PK\x05\x06")
    while 0 <= position <= len(executable) - 22:
        _, disk, central_disk, disk_count, count, central_size, central_offset, comment = struct.unpack_from(
            "<4s4H2LH", executable, position
        )
        start = position - central_size - central_offset
        if not (disk or central_disk) and count == disk_count and count >= 3 and start >= 0:
            if executable[start : start + 4] == b"PK\x03\x04":
                try:
                    with zipfile.ZipFile(io.BytesIO(executable[start : position + 22 + comment])) as payload:
                        names = {"EZ4Connectpro.exe", "zju-connect.exe", "wintun.dll"}
                        if names.issubset(payload.namelist()) and payload.testzip() is None:
                            files = [entry for entry in payload.infolist() if not entry.is_dir()]
                            return {
                                "runtime_payload_bytes": sum(entry.file_size for entry in files),
                                "payload_files": len(files),
                                "core_sha256": hashlib.sha256(payload.read("zju-connect.exe")).hexdigest(),
                                "wintun_sha256": hashlib.sha256(payload.read("wintun.dll")).hexdigest(),
                            }
                except (zipfile.BadZipFile, RuntimeError):
                    pass
        position = executable.find(b"PK\x05\x06", position + 4)
    raise ValueError("Could not verify the embedded Windows package")


def stage_replacement(target, source):
    # Stage beside the destination so Windows applies that directory's ACL.
    target.parent.mkdir(parents=True, exist_ok=True)
    staged = target.with_name(f".{target.name}.{uuid4().hex}.tmp")
    try:
        with staged.open("xb") as output:
            if isinstance(source, Path):
                with source.open("rb") as incoming:
                    copyfileobj(incoming, output)
            else:
                output.write(source)
    except BaseException:
        staged.unlink(missing_ok=True)
        raise
    return staged


def apply_changes(changes, verify, stage_root):
    if stage_root.resolve() != stage_root:
        raise ValueError(f"Unexpected delivery staging path: {stage_root}")
    stage_root.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix="delivery-", dir=stage_root))
    ready = []
    applied = []
    staged_files = []
    recovery_errors = []
    try:
        for index, (target, source) in enumerate(changes.items()):
            old_hash = digest(target) if target.is_file() else None
            new_file = None
            if source is not None:
                new_hash = digest(source) if isinstance(source, Path) else hashlib.sha256(source).hexdigest()
                if new_hash == old_hash:
                    continue
                new_file = stage_replacement(target, source)
                staged_files.append(new_file)
                if digest(new_file) != new_hash:
                    raise OSError(f"Staging hash mismatch: {target}")
            else:
                new_hash = None
            backup = None
            if old_hash is not None:
                backup = staging / f"{index}.previous"
                copy2(target, backup)
                if digest(backup) != old_hash:
                    raise OSError(f"Backup hash mismatch: {target}")
            ready.append((target, new_file, backup, new_hash))

        for target, new_file, backup, new_hash in ready:
            if new_file is None:
                target.unlink()
            else:
                os.replace(new_file, target)
            applied.append((target, backup))
            if new_hash is not None and digest(target) != new_hash:
                raise OSError(f"Delivery hash mismatch: {target}")
        verify()
    except BaseException:
        for target, backup in reversed(applied):
            try:
                if backup is None:
                    target.unlink()
                else:
                    restored = stage_replacement(target, backup)
                    staged_files.append(restored)
                    os.replace(restored, target)
            except OSError as error:
                recovery_errors.append(f"{target}: {error}")
        if recovery_errors:
            details = "\n".join(recovery_errors)
            raise OSError(f"Could not restore all previous files. Backups are in {staging}\n{details}")
        raise
    finally:
        for staged in staged_files:
            staged.unlink(missing_ok=True)
        if not recovery_errors and staging.resolve().parent == stage_root:
            rmtree(staging)


def release_report(plan, candidate, sha256, source_root, artifact, copied, facts):
    report_path = REPO / REPORT_FILE
    report = read_json(report_path) if report_path.exists() else {}
    report.setdefault("previous_validation_version", report.get("version"))
    report.update(
        version=plan["version"],
        date=plan["date"],
        platform="Windows x64",
        artifact=artifact.name,
        artifact_bytes=candidate.stat().st_size,
        sha256=sha256,
        runtime_payload_bytes=facts["runtime_payload_bytes"],
        source_delivery=str(source_root),
        binary_delivery=str(artifact),
        scope="Local versioned release. The previous application regression results are retained for reference.",
        build_revision=f'{plan["date"]}-windows-v{plan["version"]}',
        delivery_status="The local EXE and source were delivered and verified.",
        delivery={
            "source_root": str(source_root),
            "release_file": str(artifact),
            "source_files": copied,
            "only_latest_release_file": True,
            "git_commit_or_push": False,
        },
    )
    report["package"] = {
        "file_version": plan["version"],
        "product_version": plan["version"],
        "numeric_windows_version": plan["version"] + ".0",
        "embedded_zip_crc": "passed",
        **{key: facts[key] for key in ("payload_files", "core_sha256", "wintun_sha256")},
    }
    return json_bytes(report)


def deliver(candidate, sha256, plan=None):
    started = time.monotonic()
    if digest(candidate) != sha256:
        raise ValueError("Candidate hash mismatch")

    source_root = project_directory("Github_Source")
    release_root = project_directory("Github_Releases")
    artifact = release_root / BINARY_NAME
    if artifact.resolve() != artifact or any(path != artifact for path in release_root.iterdir()):
        raise ValueError("Expected one canonical EXE in the release directory")
    working_exe = REPO / "out" / BINARY_NAME
    if working_exe.resolve() != working_exe:
        raise ValueError(f"Unexpected working release path: {working_exe}")

    deferred = {VERSION_FILE, REPORT_FILE} if plan else set()
    changes, copied = source_changes(source_root, deferred)
    changes[artifact] = candidate
    if plan:
        facts = portable_facts(candidate)
        changes[working_exe] = candidate
        # Only publish the successful release state after verifying the staged EXEs.
        version = json_bytes({"version": plan["version"], "last_successful_release_date": plan["date"]})
        report = release_report(plan, candidate, sha256, source_root, artifact, copied, facts)
        changes[destination(source_root, REPORT_FILE)] = report
        changes[destination(REPO, REPORT_FILE)] = report
        changes[destination(source_root, VERSION_FILE)] = version
        changes[destination(REPO, VERSION_FILE)] = version

    def verify():
        if list(release_root.iterdir()) != [artifact] or digest(artifact) != sha256 or digest(working_exe) != sha256:
            raise OSError("Canonical release verification failed")

    apply_changes(changes, verify, REPO / "build" / "release-candidate")
    print(f"{source_root} ({copied} files)")
    print(f"{artifact} ({artifact.stat().st_size} bytes)")
    print(f"SHA-256 {sha256}")
    if plan:
        print(f'Published version {plan["version"]} ({plan["date"]})')
    print(f"Delivery seconds: {time.monotonic() - started:.2f}")


def main():
    parser = argparse.ArgumentParser(description="Verify and synchronize the local EZ4Connectpro release.")
    parser.add_argument("sha256", nargs="?")
    parser.add_argument("--plan", action="store_true")
    parser.add_argument("--candidate", type=Path)
    parser.add_argument("--version")
    parser.add_argument("--date", type=date.fromisoformat)
    args = parser.parse_args()
    if REPO.name != "EZ4Connectpro" or not (REPO / ".git").is_dir():
        parser.error("Run from the EZ4Connectpro development checkout")

    if args.plan:
        state = read_json(REPO / VERSION_FILE)
        print(json.dumps(release_plan(state, datetime.now(BEIJING).date())))
        return
    if args.sha256 is None or not re.fullmatch(r"[a-fA-F0-9]{64}", args.sha256):
        parser.error("A verified SHA-256 is required")
    candidate = REPO / "out" / BINARY_NAME
    plan = None
    if args.candidate or args.version or args.date:
        if not (args.candidate and args.version and args.date):
            parser.error("--candidate, --version and --date must be specified together")
        candidate = args.candidate.resolve(strict=True)
        if candidate != REPO / "build" / "release-candidate" / BINARY_NAME:
            parser.error("Unexpected staged release path")
        state = read_json(REPO / VERSION_FILE)
        planned = release_plan(state, args.date)
        plan = release_plan(state, datetime.now(BEIJING).date())
        if args.version != planned["version"] or args.version != plan["version"]:
            parser.error("The release version or date changed during the build. Run -Release again.")
    deliver(candidate, args.sha256.lower(), plan)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError) as error:
        raise SystemExit(str(error)) from error
