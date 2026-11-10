"""Read-only validation of the durable completed-use synchronization API."""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import json
import math
from pathlib import Path
import re
import time

import requests

from check_api_v1_http import json_schema, validate

ROOT = Path(__file__).resolve().parent.parent


def verify(snapshot, document):
    schema = document["paths"]["/sessions"]["get"]["responses"]["200"]["content"]["application/json"]["schema"]
    validate(snapshot, schema, document)
    assert snapshot["schema_version"] == 1 and snapshot["api_version"] == "1"
    assert re.fullmatch(r"rinnai-[0-9a-f]{12}", snapshot["device_id"])
    assert re.fullmatch(r"[0-9a-f]{32}", snapshot["stream_id"])
    assert snapshot["ready"] and snapshot["storage_ok"]
    assert snapshot["capacity"] == 24 and len(snapshot["sessions"]) <= 24
    assert snapshot["pricing_basis"] == "current_settings" and "samples" not in snapshot
    records = snapshot["sessions"]
    sequences = [row["sequence"] for row in records]
    assert sequences == sorted(set(sequences))
    assert snapshot["oldest_sequence"] == (sequences[0] if sequences else None)
    assert snapshot["newest_sequence"] == (sequences[-1] if sequences else None)
    assert not sequences or snapshot["total_sessions"] >= sequences[-1]
    for row in records:
        assert row["id"] == f"{snapshot['stream_id']}:{row['sequence']}"
        for name in ("water_l", "energy_kwh", "duration_s", "heating_s"):
            assert math.isfinite(row[name]) and row[name] >= 0
        start, end = row["started_at_epoch_s"], row["ended_at_epoch_s"]
        if start is not None and end is not None:
            assert end >= start and row["elapsed_s"] == end-start
        else:
            assert row["elapsed_s"] is None
        assert row["gas_unit"] == snapshot["settings"]["gas_unit"]
    return len(records)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", default="http://192.168.1.50")
    parser.add_argument("--expected-version", default="1.2.0")
    parser.add_argument("--validate-local", action="store_true")
    args = parser.parse_args()
    contract = json.loads((ROOT / "esphome/api/openapi.json").read_text(encoding="utf-8"))
    document = json_schema(contract)
    if args.validate_local:
        media = contract["paths"]["/sessions"]["get"]["responses"]["200"]["content"]["application/json"]
        example = media.get("example")
        if example is None:
            examples = media.get("examples", {})
            example = next(iter(examples.values()))["value"]
        count = verify(example, document)
        print(json.dumps({"passed": True, "device_access": False, "example_records": count}))
        return
    report = {"recorded_utc": datetime.now(timezone.utc).isoformat(), "passed": False,
              "base": args.base, "write_commands_sent": False}
    try:
        with requests.Session() as client:
            response = client.get(args.base.rstrip("/")+"/api/v1/sessions", timeout=15)
            response.raise_for_status()
            initial = response.json()
            count = verify(initial, document)
            assert initial["firmware_version"] == args.expected_version
            assert len(response.content) <= 32768
            time.sleep(1.1)
            later = client.get(args.base.rstrip("/")+"/api/v1/sessions", timeout=15)
            later.raise_for_status()
            current = later.json()
            verify(current, document)
            assert current["device_id"] == initial["device_id"]
            assert current["stream_id"] == initial["stream_id"]
            previous_by_id = {row["id"]: row for row in initial["sessions"]}
            matching = 0
            for row in current["sessions"]:
                if row["id"] in previous_by_id:
                    previous = previous_by_id[row["id"]]
                    for name in ("sequence", "water_l", "energy_kwh", "duration_s", "heating_s"):
                        assert row[name] == previous[name]
                    matching += 1
            report.update(passed=True, firmware_version=initial["firmware_version"],
                          source_records=count, stable_matching_records=matching,
                          payload_bytes=len(response.content), device_id=initial["device_id"],
                          stream_id=initial["stream_id"], oldest_sequence=initial["oldest_sequence"],
                          newest_sequence=initial["newest_sequence"], total_sessions=initial["total_sessions"])
    finally:
        destination = ROOT / "analysis/diagnostics/sessions_http_check.json"
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_text(json.dumps(report, ensure_ascii=False, indent=2)+"\n", encoding="utf-8")
        print(json.dumps({key: report[key] for key in ("passed", "source_records", "payload_bytes") if key in report}))


if __name__ == "__main__":
    main()
