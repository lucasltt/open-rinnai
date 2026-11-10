"""Compile the literal current Backend class against a host-only HTTP adapter."""
from pathlib import Path
import json
import hashlib
import os
import re
import subprocess

ROOT = Path(__file__).resolve().parent.parent
analysis = ROOT / "analysis"
(analysis / "diagnostics").mkdir(parents=True, exist_ok=True)
source = (ROOT / "esphome/dashboard_backend.h").read_text(encoding="utf-8")
struct_start = source.index("struct Live {")
struct_end = source.index("template<typename Target>", struct_start)
class_start = source.index("class Backend :")
class_end = source.index("inline Backend &backend()", class_start)
backend_source = source[class_start:class_end]
(analysis / "generated_api_structs.inc").write_text(source[struct_start:struct_end], encoding="utf-8")
helper_start = source.index("// The SDK builder")
helper_end = source.index("inline std::string state_json", helper_start)
(analysis / "generated_api_snapshot.inc").write_text(source[helper_start:helper_end], encoding="utf-8")
(analysis / "generated_api_backend.inc").write_text(backend_source, encoding="utf-8")
runtime = analysis / "tools/zig_runtime"
zig = runtime / "zig-windows-x86_64-0.12.1/zig.exe"
arduino = ROOT / "esphome/.esphome/build/rinnai-sniffer/managed_components/bblanchon__arduinojson/src"
assert arduino.joinpath("ArduinoJson.h").is_file()
env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(runtime / "cache"))
results = []
for name in ("api_command", "api_backend"):
    executable = runtime / f"test_{name}.exe"
    command = [str(zig), "c++", "-target", "x86_64-windows-gnu", "-std=c++17", "-O1", "-Wall", "-Wextra", "-Werror",
               "-Wno-unused-function", "-I", str(analysis / "stubs"), "-I", str(arduino),
               str(analysis / f"test_{name}.cpp"), "-o", str(executable)]
    subprocess.run(command, cwd=ROOT, env=env, check=True)
    run = subprocess.run([str(executable)], cwd=ROOT, capture_output=True, text=True)
    print(run.stdout.strip())
    if run.stderr:
        print(run.stderr.strip())
    run.check_returncode()
    match = re.search(r"(\d+) checks passed", run.stdout)
    results.append({"suite": name, "checks": int(match.group(1)), "passed": True})
(analysis / "diagnostics/api_backend_host_check.json").write_text(
    json.dumps({"suites": results, "passed": True,
                "source": "Actual extracted Backend class; HTTP I/O and response fixtures stubbed",
                "backend_class_sha256": hashlib.sha256(backend_source.encode()).hexdigest(),
                "parser_sha256": hashlib.sha256((ROOT / "esphome/api_requests.h").read_bytes()).hexdigest(),
                "command_tracker_sha256": hashlib.sha256((ROOT / "esphome/api_command.h").read_bytes()).hexdigest()}, indent=2) + "\n",
    encoding="utf-8")
