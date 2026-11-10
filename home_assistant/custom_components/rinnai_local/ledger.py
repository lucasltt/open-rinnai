"""Validated, transactional completed-use archive; independent of HA and I/O.

The device's 24-record ring is a recovery buffer, not the archive. An immutable
session ID is the identity; timestamps and tariffs are never used as its key.
The caller must durably save a proposed Ledger before making it visible.
"""
from __future__ import annotations

from copy import deepcopy
from bisect import bisect_left, bisect_right
from dataclasses import dataclass
from datetime import datetime, timezone
import math
import re
from typing import Any

DEVICE_ID = re.compile(r"rinnai-[0-9a-f]{12}\Z")
STREAM_ID = re.compile(r"[0-9a-f]{32}\Z")
MAX_SEQUENCE = 0xFFFFFFFF
MAX_RECORDS = 50000
MAX_STREAMS = 1024
MAX_GAPS = 10000
MIN_EPOCH = 1577836800
MAX_EPOCH = 7258118400  # 2200-01-01, prevents platform datetime overflows.


class ArchiveError(ValueError):
    """Base error with a stable code for HA diagnostics."""
    code = "invalid_snapshot"


class ArchiveConflict(ArchiveError):
    """An immutable physical quantity conflicts with an already saved ID."""
    code = "record_conflict"


class ArchiveFull(ArchiveError):
    """The configured archive is full; no cursor or record may advance."""
    code = "archive_full"


def _integer(value: Any, name: str, minimum: int = 0, maximum: int = MAX_SEQUENCE) -> int:
    if type(value) is not int or not minimum <= value <= maximum:
        raise ArchiveError(f"Invalid integer: {name}")
    return value


def _number(value: Any, name: str, *, nullable: bool = False) -> float | None:
    if value is None and nullable:
        return None
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ArchiveError(f"Invalid number: {name}")
    try:
        result = float(value)
    except OverflowError as error:
        raise ArchiveError(f"Number exceeds supported range: {name}") from error
    if not math.isfinite(result) or not 0 <= result <= 1e18:
        raise ArchiveError(f"Invalid finite amount: {name}")
    return result


def _epoch(value: Any, name: str) -> int | None:
    if value is None:
        return None
    return _integer(value, name, MIN_EPOCH, MAX_EPOCH)


def _boolean(value: Any, name: str) -> bool:
    if type(value) is not bool:
        raise ArchiveError(f"Invalid boolean: {name}")
    return value


def _gas_unit(value: Any) -> str:
    if value == "m³":
        return "m3"
    if value not in ("m3", "kg"):
        raise ArchiveError("Invalid gas unit")
    return value


def _settings(value: Any) -> dict[str, Any]:
    if not isinstance(value, dict) or value.get("gas_type") not in ("GN", "GLP"):
        raise ArchiveError("Invalid pricing settings")
    result = {"gas_type": value["gas_type"], "gas_unit": _gas_unit(value.get("gas_unit"))}
    if result["gas_unit"] != ("m3" if result["gas_type"] == "GN" else "kg"):
        raise ArchiveError("Gas type/unit mismatch")
    for key in ("gas_tariff", "water_tariff"):
        result[key] = _number(value.get(key), key)
        if result[key] > 100000:
            raise ArchiveError("Tariff exceeds firmware range")
    return result


def validate_record(value: Any, stream: str, settings: dict[str, Any]) -> dict[str, Any]:
    """Validate one immutable identity and its current display metadata."""
    if not isinstance(value, dict):
        raise ArchiveError("Invalid session object")
    sequence = _integer(value.get("sequence"), "sequence", 1)
    if value.get("id") != f"{stream}:{sequence}":
        raise ArchiveError("Session ID does not match stream and sequence")
    result = {"id": value["id"], "sequence": sequence, "stream_id": stream}
    for key in ("started_at_epoch_s", "ended_at_epoch_s"):
        result[key] = _epoch(value.get(key), key)
    for key in ("duration_s", "heating_s", "water_l", "energy_kwh", "gas"):
        result[key] = _number(value.get(key), key)
    for key in ("elapsed_s", "gas_cost", "water_cost", "cost"):
        result[key] = _number(value.get(key), key, nullable=True)
    result["gas_unit"] = _gas_unit(value.get("gas_unit"))
    if result["gas_unit"] != settings["gas_unit"]:
        raise ArchiveError("Record/settings gas unit mismatch")
    factor = 9400 if settings["gas_type"] == "GN" else 11500
    expected_gas = result["energy_kwh"] * 3600 / 4.1868 / factor
    if not math.isclose(result["gas"], expected_gas, abs_tol=0.000006, rel_tol=2e-5):
        raise ArchiveError("Gas estimate differs from the reported energy")
    for key in ("complete", "cost_complete"):
        result[key] = _boolean(value.get(key), key)
    reason = value.get("end_reason")
    if not isinstance(reason, str) or not 1 <= len(reason) <= 64 or not re.fullmatch(r"[a-z_]+", reason):
        raise ArchiveError("Invalid end reason")
    result["end_reason"] = reason
    start, end, elapsed = (result[key] for key in ("started_at_epoch_s", "ended_at_epoch_s", "elapsed_s"))
    if start is not None and end is not None:
        if end < start or elapsed is None or abs(elapsed - (end - start)) > 0.001:
            raise ArchiveError("Invalid session timestamps/elapsed time")
    elif elapsed is not None:
        raise ArchiveError("Elapsed time provided without both timestamps")
    gas_known, water_known = settings["gas_tariff"] > 0, settings["water_tariff"] > 0
    if (result["gas_cost"] is not None) != gas_known or (result["water_cost"] is not None) != water_known:
        raise ArchiveError("Cost availability does not match tariffs")
    if result["cost_complete"] != (gas_known and water_known):
        raise ArchiveError("Invalid cost_complete flag")
    if (result["cost"] is not None) != (gas_known or water_known):
        raise ArchiveError("Invalid combined cost availability")
    if result["cost"] is not None:
        expected = (result["gas_cost"] or 0) + (result["water_cost"] or 0)
        if not math.isclose(result["cost"], expected, abs_tol=0.003, rel_tol=1e-7):
            raise ArchiveError("Combined cost differs from its components")
    if gas_known and not math.isclose(result["gas_cost"], result["gas"] * settings["gas_tariff"],
                                      abs_tol=0.002 + settings["gas_tariff"] * 0.0000051, rel_tol=2e-5):
        raise ArchiveError("Gas cost differs from tariff")
    if water_known and not math.isclose(result["water_cost"], result["water_l"] / 1000 * settings["water_tariff"],
                                        abs_tol=0.002 + settings["water_tariff"] * 0.00000051, rel_tol=2e-5):
        raise ArchiveError("Water cost differs from tariff")
    return result


def validate_snapshot(value: Any, expected_device: str | None = None) -> dict[str, Any]:
    """Reject incomplete, cross-device and contradictory snapshots atomically."""
    if not isinstance(value, dict) or value.get("api_version") != "1" or type(value.get("schema_version")) is not int or value["schema_version"] != 1:
        raise ArchiveError("Unsupported completed-use API schema")
    device, stream = value.get("device_id"), value.get("stream_id")
    if not isinstance(device, str) or DEVICE_ID.fullmatch(device) is None:
        raise ArchiveError("Invalid device identity")
    if expected_device is not None and device != expected_device:
        raise ArchiveConflict("A different device answered at this address")
    if not isinstance(stream, str) or STREAM_ID.fullmatch(stream) is None:
        raise ArchiveError("Invalid history stream identity")
    if value.get("ready") is not True or value.get("storage_ok") is not True:
        raise ArchiveError("Device history is not durably ready")
    if value.get("pricing_basis") != "current_settings":
        raise ArchiveError("Unsupported pricing basis")
    capacity = _integer(value.get("capacity"), "capacity", 1, 24)
    total = _integer(value.get("total_sessions"), "total_sessions")
    exhausted = _boolean(value.get("sequence_exhausted"), "sequence_exhausted")
    firmware = value.get("firmware_version")
    if not isinstance(firmware, str) or not 1 <= len(firmware) <= 64:
        raise ArchiveError("Invalid firmware version")
    settings = _settings(value.get("settings"))
    rows = value.get("sessions")
    if not isinstance(rows, list) or len(rows) > capacity:
        raise ArchiveError("Invalid session ring length")
    records = [validate_record(row, stream, settings) for row in rows]
    sequences = [row["sequence"] for row in records]
    if sequences != sorted(set(sequences)):
        raise ArchiveError("Sessions are duplicated or not oldest-first")
    oldest = value.get("oldest_sequence")
    newest = value.get("newest_sequence")
    if records:
        _integer(oldest, "oldest_sequence", 1)
        _integer(newest, "newest_sequence", 1)
        if oldest != sequences[0] or newest != sequences[-1] or total < newest:
            raise ArchiveError("Snapshot cursor does not match records")
    elif oldest is not None or newest is not None:
        raise ArchiveError("Empty ring has nonempty cursors")
    return {"device_id": device, "stream_id": stream, "capacity": capacity,
            "total_sessions": total, "sequence_exhausted": exhausted,
            "firmware_version": firmware, "settings": settings, "sessions": records}


def _add_gap(gaps: list[dict], start: int, end: int, kind: str) -> None:
    if end >= start:
        gaps.append({"start": start, "end": end, "kind": kind})


def _recover(gaps: list[dict], sequence: int) -> list[dict]:
    result = []
    for gap in gaps:
        if not gap["start"] <= sequence <= gap["end"]:
            result.append(gap)
        else:
            _add_gap(result, gap["start"], sequence - 1, gap["kind"])
            _add_gap(result, sequence + 1, gap["end"], gap["kind"])
    return result


def _union_ranges(ranges: list[tuple[int, int]]) -> list[tuple[int, int]]:
    result: list[tuple[int, int]] = []
    for start, end in sorted(ranges):
        if result and start <= result[-1][1] + 1:
            result[-1] = (result[-1][0], max(result[-1][1], end))
        else:
            result.append((start, end))
    return result


def _intersections(left: list[tuple[int, int]], right: list[tuple[int, int]]) -> list[tuple[int, int]]:
    result = []
    i = j = 0
    while i < len(left) and j < len(right):
        start, end = max(left[i][0], right[j][0]), min(left[i][1], right[j][1])
        if end >= start:
            result.append((start, end))
        if left[i][1] < right[j][1]:
            i += 1
        else:
            j += 1
    return result


@dataclass(frozen=True)
class MergeResult:
    ledger: "Ledger"
    added_ids: tuple[str, ...]
    updated_ids: tuple[str, ...]
    changed: bool


class Ledger:
    """Copy-on-write ledger; published only after its durable commit succeeds."""

    def __init__(self, device_id: str, data: dict | None = None):
        if not isinstance(device_id, str) or DEVICE_ID.fullmatch(device_id) is None:
            raise ArchiveError("Invalid device identity")
        self.device_id = device_id
        self.data = deepcopy(data) if data is not None else {
            "schema_version": 1, "device_id": device_id, "revision": 0,
            "current_stream_id": None, "streams": {}, "records": [], "aliases": {},
        }
        if data is not None:
            self._validate_saved()

    @property
    def records(self) -> list[dict]:
        return self.data["records"]

    @property
    def latest(self) -> dict | None:
        stream = self.data["current_stream_id"]
        state = self.data["streams"].get(stream, {})
        highest = state.get("highest_sequence", 0)
        if highest:
            wanted = f"{stream}:{highest}"
            wanted = self.data.get("aliases", {}).get(wanted, wanted)
            for row in reversed(self.records):
                if row["id"] == wanted:
                    return row
        # An empty new stream has no new completed use. Retain the latest known
        # historical use, rather than treating a backfilled import as newest.
        dated = (row for row in self.records if row["ended_at_epoch_s"] is not None)
        latest_dated = max(dated, key=lambda row: row["ended_at_epoch_s"], default=None)
        return latest_dated or (self.records[-1] if self.records else None)

    @property
    def undated_count(self) -> int:
        return sum(row["started_at_epoch_s"] is None or row["ended_at_epoch_s"] is None for row in self.records)

    def missing_count(self, kind: str) -> int:
        # Aliases prove that two stream IDs include the same physical history.
        # Union their intervals rather than reporting the same older gap twice.
        parents = {stream: stream for stream in self.data["streams"]}

        def root(stream: str) -> str:
            while parents[stream] != stream:
                parents[stream] = parents[parents[stream]]
                stream = parents[stream]
            return stream

        for alias, canonical in self.data.get("aliases", {}).items():
            parents[root(alias.split(":", 1)[0])] = root(canonical.split(":", 1)[0])
        groups: dict[str, dict] = {}
        for stream, state in self.data["streams"].items():
            group = groups.setdefault(root(stream), {"before_first_sync": [], "missed_after_sync": [], "known": set()})
            for gap in state["gaps"]:
                group[gap["kind"]].append((gap["start"], gap["end"]))
        for row in self.records:
            groups[root(row["stream_id"])]["known"].add(row["sequence"])
        total = 0
        for group in groups.values():
            known = sorted(group["known"])

            def count(ranges: list[tuple[int, int]]) -> int:
                return sum(end - start + 1 - (bisect_right(known, end) - bisect_left(known, start))
                           for start, end in ranges)

            ranges = _union_ranges(group.get(kind, []))
            total += count(ranges)
            if kind == "before_first_sync":
                observed = _union_ranges(group["missed_after_sync"])
                total -= count(_intersections(ranges, observed))
        return total

    def to_data(self) -> dict:
        return deepcopy(self.data)

    def _validate_saved(self) -> None:
        data = self.data
        if not isinstance(data, dict) or type(data.get("schema_version")) is not int or data.get("schema_version") != 1 or data.get("device_id") != self.device_id:
            raise ArchiveError("Invalid stored archive schema/device")
        _integer(data.get("revision"), "archive revision", 0, 2**63 - 1)
        if not isinstance(data.get("streams"), dict) or len(data["streams"]) > MAX_STREAMS:
            raise ArchiveError("Invalid stored stream ledger")
        current = data.get("current_stream_id")
        if current is not None and current not in data["streams"]:
            raise ArchiveError("Missing current stream")
        for stream, state in data["streams"].items():
            if not isinstance(stream, str) or STREAM_ID.fullmatch(stream) is None or not isinstance(state, dict):
                raise ArchiveError("Invalid stored stream")
            _integer(state.get("highest_sequence"), "stored cursor")
            _boolean(state.get("source_regressed"), "stored regression flag")
            _boolean(state.get("sequence_exhausted"), "stored exhaustion flag")
            if not isinstance(state.get("gaps"), list) or len(state["gaps"]) > MAX_GAPS:
                raise ArchiveError("Invalid stored gaps")
            previous_end = 0
            for gap in state["gaps"]:
                if not isinstance(gap, dict):
                    raise ArchiveError("Invalid stored gap object")
                if gap.get("kind") not in ("before_first_sync", "missed_after_sync"):
                    raise ArchiveError("Invalid gap kind")
                start = _integer(gap.get("start"), "gap start", 1)
                until = _integer(gap.get("end"), "gap end", start)
                if start <= previous_end or until > state["highest_sequence"]:
                    raise ArchiveError("Stored gaps overlap or exceed the cursor")
                previous_end = until
        if not isinstance(data.get("records"), list) or len(data["records"]) > MAX_RECORDS:
            raise ArchiveError("Invalid stored record count")
        seen = set()
        for row in data["records"]:
            if not isinstance(row, dict) or row.get("stream_id") not in data["streams"]:
                raise ArchiveError("Stored record has unknown stream")
            settings = _settings(row.get("tariffs_at_import"))
            validated = validate_record(row, row["stream_id"], settings)
            if validated["sequence"] > data["streams"][row["stream_id"]]["highest_sequence"]:
                raise ArchiveError("Stored record exceeds stream cursor")
            if validated["id"] in seen:
                raise ArchiveError("Duplicate stored record")
            seen.add(validated["id"])
            if row.get("pricing_basis") != "tariffs_at_import" or _epoch(row.get("imported_at_epoch_s"), "imported timestamp") is None:
                raise ArchiveError("Invalid stored import metadata")
        aliases = data.setdefault("aliases", {})
        if not isinstance(aliases, dict) or len(aliases) > MAX_RECORDS * 2:
            raise ArchiveError("Invalid source alias map")
        record_index = {row["id"]: row for row in self.records}
        for source_id, canonical_id in aliases.items():
            if not isinstance(source_id, str) or not isinstance(canonical_id, str) or canonical_id not in seen or source_id in seen:
                raise ArchiveError("Invalid source alias")
            parts = source_id.split(":")
            if len(parts) != 2 or parts[0] not in data["streams"] or not parts[1].isdigit() or not 1 <= int(parts[1]) <= MAX_SEQUENCE:
                raise ArchiveError("Invalid alias identity")
            if str(int(parts[1])) != parts[1] or int(parts[1]) > data["streams"][parts[0]]["highest_sequence"]:
                raise ArchiveError("Alias sequence exceeds its stream cursor")
            target = record_index[canonical_id]
            if int(parts[1]) != target["sequence"]:
                raise ArchiveError("Alias sequence differs from canonical record")

    def propose(self, snapshot: dict, *, now_epoch: int | None = None, max_records: int = 10000) -> MergeResult:
        """Return a new candidate without changing this ledger on any failure."""
        snap = validate_snapshot(snapshot, self.device_id)
        _integer(max_records, "archive limit", 1, MAX_RECORDS)
        now = now_epoch if now_epoch is not None else int(datetime.now(timezone.utc).timestamp())
        _epoch(now, "import time")
        candidate = Ledger(self.device_id)
        candidate.data = self.to_data()
        data = candidate.data
        stream = snap["stream_id"]
        new_stream = stream not in data["streams"]
        if new_stream:
            if len(data["streams"]) >= MAX_STREAMS:
                raise ArchiveFull("Too many source streams; archive maintenance required")
            data["streams"][stream] = {"highest_sequence": 0, "gaps": [], "source_regressed": False,
                                       "sequence_exhausted": False}
        state = data["streams"][stream]
        highest = state["highest_sequence"]
        data["current_stream_id"] = stream
        state["sequence_exhausted"] = snap["sequence_exhausted"]
        records = snap["sessions"]
        state["source_regressed"] = bool(highest and (not records or records[-1]["sequence"] < highest))
        if records:
            cursor = highest
            if new_stream:
                _add_gap(state["gaps"], 1, records[0]["sequence"] - 1, "before_first_sync")
                cursor = records[0]["sequence"] - 1
            for row in records:
                if row["sequence"] > cursor:
                    _add_gap(state["gaps"], cursor + 1, row["sequence"] - 1, "missed_after_sync")
                    cursor = row["sequence"]
            state["highest_sequence"] = max(highest, records[-1]["sequence"])
        existing = {row["id"]: row for row in data["records"]}
        aliases = data["aliases"]
        added, updated = [], []
        physical = {"water_l": 0.002, "energy_kwh": 0.00002, "duration_s": 0.002, "heating_s": 0.002}
        by_sequence: dict[int, list[dict]] = {}
        for saved in existing.values():
            by_sequence.setdefault(saved["sequence"], []).append(saved)
        for row in records:
            state["gaps"] = _recover(state["gaps"], row["sequence"])
            canonical = aliases.get(row["id"], row["id"])
            if canonical not in existing:
                # A corrupt-primary fallback can create a new stream around
                # surviving old records. Reconcile only with reliable equal
                # boundaries, sequence and physical quantities; otherwise
                # ambiguity must be visible rather than double-counted.
                matches = []
                for saved in by_sequence.get(row["sequence"], []):
                    if saved["stream_id"] == stream or saved["sequence"] != row["sequence"]:
                        continue
                    all_dated = all(item[key] is not None for item in (saved, row)
                                    for key in ("started_at_epoch_s", "ended_at_epoch_s"))
                    dates_equal = all(saved[key] == row[key] for key in ("started_at_epoch_s", "ended_at_epoch_s"))
                    quantities_equal = all(math.isclose(saved[key], row[key], rel_tol=1e-9, abs_tol=tol)
                                           for key, tol in physical.items())
                    any_distinct_boundary = any(saved[key] is not None and row[key] is not None and saved[key] != row[key]
                                                for key in ("started_at_epoch_s", "ended_at_epoch_s"))
                    if all_dated and dates_equal:
                        if not quantities_equal:
                            raise ArchiveConflict("Conflicting physical data across source streams")
                        matches.append(saved)
                    elif quantities_equal and not any_distinct_boundary:
                        raise ArchiveConflict("Ambiguous undated use after source stream reset")
                if len(matches) > 1:
                    raise ArchiveConflict("Multiple possible archive matches after source reset")
                if matches:
                    canonical = matches[0]["id"]
                    aliases[row["id"]] = canonical
            if canonical in existing:
                saved = existing[canonical]
                for key, tolerance in physical.items():
                    if not math.isclose(saved[key], row[key], rel_tol=1e-9, abs_tol=tolerance):
                        raise ArchiveConflict(f"Conflicting {key} for saved session {row['id']}")
                update_fields = ["complete", "end_reason"]
                boundaries = ("started_at_epoch_s", "ended_at_epoch_s")
                if all(saved[key] is None or row[key] is not None for key in boundaries):
                    update_fields.extend(("started_at_epoch_s", "ended_at_epoch_s", "elapsed_s"))
                if any(saved[key] != row[key] for key in update_fields):
                    for key in update_fields:
                        saved[key] = row[key]
                    if canonical not in updated:
                        updated.append(canonical)
                continue  # Prices and physical quantities remain as first imported.
            if len(data["records"]) >= max_records:
                raise ArchiveFull("Archive limit reached; increase the limit before importing further uses")
            imported = deepcopy(row)
            imported.update(tariffs_at_import=deepcopy(snap["settings"]),
                            pricing_basis="tariffs_at_import", imported_at_epoch_s=now)
            data["records"].append(imported)
            existing[row["id"]] = imported
            by_sequence.setdefault(row["sequence"], []).append(imported)
            added.append(row["id"])
        if any(len(state["gaps"]) > MAX_GAPS for state in data["streams"].values()):
            raise ArchiveFull("Too many source gap intervals")
        changed = data != self.data
        if changed:
            data["revision"] += 1
        return MergeResult(candidate if changed else self, tuple(added), tuple(updated), changed)

    def query(self, *, offset: int = 0, limit: int = 100, start: int | None = None,
              end: int | None = None, include_undated: bool = True) -> dict:
        """Newest-imported-first pagination; unknown dates remain explicitly null."""
        _integer(offset, "offset", 0, MAX_RECORDS)
        _integer(limit, "limit", 1, 500)
        _boolean(include_undated, "include_undated")
        if start is not None:
            _epoch(start, "query start")
        if end is not None:
            _epoch(end, "query end")
        if start is not None and end is not None and start >= end:
            raise ArchiveError("The query end must be after its start")
        rows = []
        for row in reversed(self.records):
            since, until = row["started_at_epoch_s"], row["ended_at_epoch_s"]
            if since is None or until is None:
                if not include_undated:
                    continue
            elif (start is not None and until <= start) or (end is not None and since >= end):
                continue
            rows.append(row)
        return {"device_id": self.device_id, "revision": self.data["revision"],
                "total": len(rows), "offset": offset, "limit": limit,
                "next_offset": offset + limit if offset + limit < len(rows) else None,
                "records": deepcopy(rows[offset:offset + limit]),
                "missed_after_sync": self.missing_count("missed_after_sync"),
                "unavailable_before_first_sync": self.missing_count("before_first_sync"),
                "undated_count": self.undated_count}
