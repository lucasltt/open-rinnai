"""Export only reviewed project sources and public documentation for GitHub.

This command never initializes Git, contacts a device, or reads local secrets.
Use --check for a read-only audit. Output folders and ZIPs must be new names
under dist/; existing exports are preserved rather than recursively replaced.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
import ipaddress
import json
from pathlib import Path
import re
import sys
import zipfile
import zlib


ROOT = Path(__file__).resolve().parent.parent
ROOT_FILES = ("README.md", ".gitignore", "requirements.txt", "LICENSE")
OPTIONAL_ROOT_FILES = ("NOTICE", "CHANGELOG.md")
ANALYSIS_FILES = (
    "build_pwa_assets.py", "embed_pwa_assets.py", "embed_api_assets.py",
    "create_secrets.py", "export_github.py", "check_api_v1_http.py",
    "protocol_inspector.py", "test_protocol_inspector.py", "run_api_backend_test.py", "check_sessions_http.py",
)
FORBIDDEN_SUFFIXES = {
    ".apk", ".xapk", ".bin", ".elf", ".exe", ".dll", ".pdb", ".pyc",
    ".zip", ".7z", ".db", ".sqlite", ".sqlite3", ".log",
}
IMAGE_SUFFIXES = {".png", ".jpg", ".jpeg", ".webp", ".gif", ".ico"}
EXAMPLE_IPS = {
    ipaddress.ip_address(value)
    for value in ("192.168.1.50", "192.168.4.1", "255.255.255.0", "255.255.255.255")
}
DOCUMENTATION_NETS = tuple(ipaddress.ip_network(value) for value in
                           ("192.0.2.0/24", "198.51.100.0/24", "203.0.113.0/24"))
IPV4 = re.compile(r"(?<![\d.])(?:\d{1,3}\.){3}\d{1,3}(?![\d.])")
MAC = re.compile(r"(?i)(?<![a-f0-9])(?:[a-f0-9]{2}[:-]){5}[a-f0-9]{2}(?![a-f0-9])")
PERSONAL_WINDOWS_PATH = re.compile(r"(?i)\b[a-z]:[\\/]Users[\\/]")
BYTE_ARRAY = re.compile(r"(?:constexpr\s+)?uint8_t\s+\w+\s*\[\s*\]\s*=\s*\{(.*?)\};", re.DOTALL)
SENSITIVE_YAML = re.compile(
    r"(?im)^[ \t]*(ssid|wifi_ssid|psk|wifi_password|sniffer_ap_password|"
    r"api_encryption_key|ota_password|web_password|password|key)[ \t]*:[ \t]*([^\n]*)$")


@dataclass(frozen=True)
class PublicFile:
    name: str
    data: bytes


def relative_name(path: Path) -> str:
    return path.relative_to(ROOT).as_posix()


def is_private_name(path: Path) -> bool:
    name = path.name.lower()
    return (name == "rinnai-local.yaml" or
            name.startswith("secrets") and name != "secrets.example.yaml" or
            name in (".env", ".ds_store") or name.startswith(".env.") or
            path.suffix.lower() in FORBIDDEN_SUFFIXES or
            any(part in ("__pycache__", "node_modules", ".git", ".esphome",
                         "private", "backups", "diagnostics", "tools", "firmware")
                for part in path.parts))


def select_sources() -> tuple[list[PublicFile], list[dict]]:
    selected: set[Path] = set()
    errors: list[dict] = []
    selected.update(ROOT / name for name in OPTIONAL_ROOT_FILES if (ROOT / name).is_file())
    for name in ROOT_FILES:
        path = ROOT / name
        if not path.is_file():
            errors.append({"path": name, "reason": "required_file_missing"})
        else:
            selected.add(path)
    for name in ANALYSIS_FILES:
        path = ROOT / "analysis" / name
        if not path.is_file():
            errors.append({"path": f"analysis/{name}", "reason": "required_file_missing"})
        else:
            selected.add(path)
    selected.update((ROOT / "analysis").glob("test_*.cpp"))
    selected.update((ROOT / "analysis/stubs").rglob("*.h"))
    for pattern in ("*.h", "*.yaml"):
        selected.update((ROOT / "esphome").glob(pattern))
    api_doc = ROOT / "esphome/API_LOCAL.md"
    if api_doc.is_file():
        selected.add(api_doc)
    else:
        errors.append({"path": "esphome/API_LOCAL.md", "reason": "required_file_missing"})
    for folder in ("docs", "esphome/packages", "esphome/dashboard", "esphome/api", "home_assistant"):
        selected.update(path for path in (ROOT / folder).rglob("*") if path.is_file())

    files = []
    for path in sorted(selected, key=lambda item: relative_name(item)):
        if is_private_name(path.relative_to(ROOT)):
            continue
        name = relative_name(path)
        # Never follow a link into another workspace or a private local folder.
        if path.is_symlink() or any(parent.is_symlink() for parent in path.parents if parent != ROOT):
            errors.append({"path": name, "reason": "symlink_not_exported"})
            continue
        try:
            path.resolve().relative_to(ROOT.resolve())
        except ValueError:
            errors.append({"path": name, "reason": "source_outside_workspace"})
            continue
        files.append(PublicFile(name, path.read_bytes()))
    return files, errors


def glob_regex(pattern: str) -> re.Pattern:
    """Match the deliberately simple patterns used by this public .gitignore."""
    parts = []
    i = 0
    while i < len(pattern):
        char = pattern[i]
        if pattern[i:i + 3] == "**/":
            parts.append("(?:.*/)?")
            i += 3
        elif pattern[i:i + 2] == "**":
            parts.append(".*")
            i += 2
        elif char == "*":
            parts.append("[^/]*")
            i += 1
        elif char == "?":
            parts.append("[^/]")
            i += 1
        else:
            parts.append(re.escape(char))
            i += 1
    return re.compile("^" + "".join(parts) + "$")


def ignored_by_root_gitignore(name: str, text: str) -> bool:
    rules = []
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if "\\" in line or "[" in line:
            raise ValueError("Unsupported .gitignore syntax; audit the new pattern before exporting")
        include = line.startswith("!")
        line = line[1:] if include else line
        anchored = line.startswith("/")
        directory_only = line.endswith("/")
        line = line.strip("/")
        rules.append((include, directory_only, anchored or "/" in line, glob_regex(line)))
    parts = name.split("/")
    # Git cannot include a file while an ancestor directory remains excluded.
    for length in range(1, len(parts) + 1):
        target = "/".join(parts[:length])
        directory = length < len(parts)
        ignored = False
        for include, directory_only, anchored, regex in rules:
            if directory_only and not directory:
                continue
            value = target if anchored else parts[length - 1]
            if regex.fullmatch(value):
                ignored = not include
        if ignored:
            return True
    return False


def text_findings(name: str, text: str, *, embedded: str | None = None) -> list[dict]:
    findings = []

    def add(reason: str, offset: int) -> None:
        item = {"path": name, "reason": reason, "line": text.count("\n", 0, offset) + 1}
        if embedded:
            item["embedded"] = embedded
        findings.append(item)

    for match in IPV4.finditer(text):
        try:
            ip = ipaddress.ip_address(match.group())
        except ValueError:
            continue
        if not (ip in EXAMPLE_IPS or ip.is_loopback or ip.is_unspecified or
                any(ip in network for network in DOCUMENTATION_NETS)):
            add("non_example_ipv4_reference", match.start())
    for pattern, reason in ((MAC, "mac_address_reference"),
                            (PERSONAL_WINDOWS_PATH, "personal_windows_path_reference")):
        for match in pattern.finditer(text):
            add(reason, match.start())
    for match in SENSITIVE_YAML.finditer(text):
        key, raw = match.groups()
        if key.lower() == "key" and Path(name).suffix.lower() not in (".yaml", ".yml"):
            continue
        value = raw.strip().strip("\"'")
        if (not value or value.startswith(("!secret ", "!remove", "${", "TROQUE-", "GERE-", "EXEMPLO-",
                              "EXAMPLE-", "CHANGE_ME", "<")) or
                key.lower() == "ssid" and value == "Rinnai Sniffer"):
            continue
        add("literal_wifi_or_credential_value", match.start())
    return findings


def audit(files: list[PublicFile], initial_errors: list[dict]) -> dict:
    findings = list(initial_errors)
    gitignore = next((item.data.decode("utf-8-sig") for item in files if item.name == ".gitignore"), "")
    for file in files:
        if ignored_by_root_gitignore(file.name, gitignore):
            findings.append({"path": file.name, "reason": "gitignore_excludes_public_source"})
        if Path(file.name).suffix.lower() in IMAGE_SUFFIXES:
            continue
        try:
            text = file.data.decode("utf-8-sig")
        except UnicodeDecodeError:
            findings.append({"path": file.name, "reason": "unreviewed_binary_source"})
            continue
        findings.extend(text_findings(file.name, text))
        # The generated API/PWA headers contain gzip bytes. Inspect the decoded
        # HTML/JSON/JS too; plain-text searching a header would miss old captures.
        if file.name.endswith(".h"):
            for index, match in enumerate(BYTE_ARRAY.finditer(text)):
                numbers = re.findall(r"0[xX]([0-9a-fA-F]{2})\b", match.group(1))
                data = bytes(int(value, 16) for value in numbers)
                if not data.startswith(b"\x1f\x8b"):
                    continue
                try:
                    decoder = zlib.decompressobj(16 + zlib.MAX_WBITS)
                    decoded = decoder.decompress(data, 1024 * 1024)
                    if decoder.unconsumed_tail or not decoder.eof:
                        raise ValueError("Embedded data exceeds the audit size limit")
                    findings.extend(text_findings(file.name, decoded.decode("utf-8"), embedded=f"gzip-array-{index}"))
                except (ValueError, UnicodeDecodeError, zlib.error):
                    findings.append({"path": file.name, "reason": "embedded_gzip_audit_failed"})
    entries = [{"path": item.name, "bytes": len(item.data),
                "sha256": hashlib.sha256(item.data).hexdigest()} for item in files]
    return {"schema_version": 1, "passed": not findings, "source_files": len(files),
            "source_bytes": sum(len(item.data) for item in files), "files": entries,
            "findings": findings,
            "audit": {"allowlist_only": True, "local_secrets_read": False,
                      "embedded_gzip_inspected": True, "gitignore_paths_checked": True,
                      "literal_wifi_and_credentials_checked": True,
                      "device_access": False, "git_commands_run": False}}


def export(files: list[PublicFile], report: dict, output: Path) -> tuple[Path, Path]:
    destination = output.resolve()
    dist = (ROOT / "dist").resolve()
    try:
        destination.relative_to(dist)
    except ValueError:
        raise ValueError("Use a new export folder under this project's dist/ directory")
    if destination == dist:
        raise ValueError("Choose a named subfolder of dist/")
    archive = destination.with_suffix(destination.suffix + ".zip")
    if destination.exists() or archive.exists():
        raise FileExistsError("Export folder or ZIP already exists; choose a new --output name. Nothing was removed.")
    if any(parent.is_symlink() for parent in (destination.parent, *destination.parent.parents)):
        raise ValueError("Output parent directories cannot be symlinks")
    # Every byte below comes from the already audited snapshot, preventing a
    # source edited during export from slipping into the published ZIP.
    report_data = (json.dumps(report, ensure_ascii=False, indent=2) + "\n").encode("utf-8")
    payload = [*files, PublicFile("PUBLICATION_REPORT.json", report_data)]
    destination.mkdir(parents=True, exist_ok=False)
    for item in payload:
        path = destination / item.name
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open("xb") as handle:
            handle.write(item.data)
    # Fixed timestamps/permissions and stable order make the ZIP reproducible
    # for the same sources and output-folder name on Windows or Linux.
    with zipfile.ZipFile(archive, "x", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as zipped:
        for item in payload:
            entry = zipfile.ZipInfo(f"{destination.name}/{item.name}", date_time=(1980, 1, 1, 0, 0, 0))
            entry.compress_type = zipfile.ZIP_DEFLATED
            entry.create_system = 3
            entry.external_attr = 0o100644 << 16
            zipped.writestr(entry, item.data, compress_type=zipfile.ZIP_DEFLATED, compresslevel=9)
    with zipfile.ZipFile(archive) as zipped:
        for item in payload:
            assert zipped.read(f"{destination.name}/{item.name}") == item.data
    return destination, archive


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="Audit the allowlisted files without creating an export")
    parser.add_argument("--output", type=Path, default=ROOT / "dist/rinnai-local-github",
                        help="New directory under dist/ (the ZIP uses the same name plus .zip)")
    args = parser.parse_args()
    try:
        files, errors = select_sources()
        report = audit(files, errors)
        summary = {key: report[key] for key in ("passed", "source_files", "source_bytes", "findings", "audit")}
        if report["passed"] and not args.check:
            directory, archive = export(files, report, args.output)
            summary.update(output=relative_name(directory), archive=relative_name(archive),
                           archive_sha256=hashlib.sha256(archive.read_bytes()).hexdigest())
        print(json.dumps(summary, ensure_ascii=False, indent=2))
        return 0 if report["passed"] else 1
    except (OSError, ValueError) as error:
        print(json.dumps({"passed": False, "error": str(error)}, ensure_ascii=False), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
