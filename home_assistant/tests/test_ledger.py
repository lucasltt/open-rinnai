"""Pure archive regression tests: no HA runtime, network or heater required."""
from copy import deepcopy
import importlib.util
import json
from pathlib import Path
import sys
import unittest

COMPONENT = Path(__file__).resolve().parents[1] / "custom_components/rinnai_local"
SPEC = importlib.util.spec_from_file_location("rinnai_local_ledger_test", COMPONENT / "ledger.py")
assert SPEC and SPEC.loader
ledger = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = ledger
SPEC.loader.exec_module(ledger)

Ledger = ledger.Ledger
ArchiveError = ledger.ArchiveError
ArchiveConflict = ledger.ArchiveConflict
ArchiveFull = ledger.ArchiveFull
DEVICE = "rinnai-aabbccddeeff"
A = "a" * 32
B = "b" * 32
C = "c" * 32
NOW = 1800000000


def session(sequence, stream=A, *, start=None, unknown=False, settings=None):
    cfg = settings or {"gas_type": "GN", "gas_unit": "m3", "gas_tariff": 8.5, "water_tariff": 12.0}
    begin = 1700000000 + sequence * 600 if start is None else start
    water = round(12 + sequence / 10, 3)
    energy = round(sequence / 100, 5)
    factor = 9400 if cfg["gas_type"] == "GN" else 11500
    exact_gas = energy * 3600 / 4.1868 / factor
    gas_cost = round(exact_gas * cfg["gas_tariff"], 3) if cfg["gas_tariff"] else None
    water_cost = round(water / 1000 * cfg["water_tariff"], 3) if cfg["water_tariff"] else None
    return {"id": f"{stream}:{sequence}", "sequence": sequence,
            "started_at_epoch_s": None if unknown else begin,
            "ended_at_epoch_s": None if unknown else begin + 120,
            "elapsed_s": None if unknown else 120,
            "duration_s": 119.5, "heating_s": 100, "water_l": water,
            "energy_kwh": energy, "gas": round(exact_gas, 5),
            "gas_unit": cfg["gas_unit"], "gas_cost": gas_cost, "water_cost": water_cost,
            "cost": round((gas_cost or 0) + (water_cost or 0), 3) if gas_cost is not None or water_cost is not None else None,
            "cost_complete": bool(cfg["gas_tariff"] and cfg["water_tariff"]),
            "complete": True, "end_reason": "flow_stop"}


def snapshot(sequences=(), stream=A, *, settings=None, rows=None, total=None):
    cfg = settings or {"gas_type": "GN", "gas_unit": "m3", "gas_tariff": 8.5, "water_tariff": 12.0}
    values = [session(seq, stream, settings=cfg) for seq in sequences] if rows is None else rows
    return {"api_version": "1", "schema_version": 1, "device_id": DEVICE,
            "stream_id": stream, "firmware_version": "1.2.0", "ready": True,
            "storage_ok": True, "capacity": 24, "total_sessions": total if total is not None else values[-1]["sequence"] if values else 0,
            "oldest_sequence": values[0]["sequence"] if values else None,
            "newest_sequence": values[-1]["sequence"] if values else None,
            "sequence_exhausted": False, "pricing_basis": "current_settings",
            "settings": deepcopy(cfg), "sessions": deepcopy(values)}


def imported(values=range(1, 4), **kwargs):
    return Ledger(DEVICE).propose(snapshot(values, **kwargs), now_epoch=NOW).ledger


class LedgerTests(unittest.TestCase):
    def test_gap_backfill_does_not_regress_latest_use(self):
        first = imported([1, 3])
        recovered = first.propose(snapshot([1, 2, 3]), now_epoch=NOW).ledger
        self.assertEqual([row["sequence"] for row in recovered.records], [1, 3, 2])
        self.assertEqual(recovered.latest["sequence"], 3)
        self.assertEqual(recovered.missing_count("missed_after_sync"), 0)

    def test_latest_resolves_alias_and_empty_new_stream(self):
        first = imported([1, 3])
        recovered = first.propose(snapshot([1, 2, 3]), now_epoch=NOW).ledger
        aliased = recovered.propose(snapshot([1, 2, 3], stream=B), now_epoch=NOW).ledger
        self.assertEqual(aliased.latest["id"], f"{A}:3")
        empty_reset = aliased.propose(snapshot([], stream=C), now_epoch=NOW).ledger
        self.assertEqual(empty_reset.latest["id"], f"{A}:3")

    def test_import_existing_ring_and_no_mutation_before_commit(self):
        original = Ledger(DEVICE)
        proposal = original.propose(snapshot(range(44, 68)), now_epoch=NOW)
        self.assertEqual(original.records, [])
        self.assertEqual(len(proposal.ledger.records), 24)
        self.assertEqual(proposal.ledger.missing_count("before_first_sync"), 43)
        self.assertEqual(proposal.ledger.missing_count("missed_after_sync"), 0)
        self.assertEqual(proposal.ledger.latest["sequence"], 67)
        self.assertEqual(len(proposal.added_ids), 24)
        self.assertTrue(proposal.changed)

    def test_duplicate_snapshot_is_identity_no_write(self):
        saved = imported()
        proposal = saved.propose(snapshot(range(1, 4)), now_epoch=NOW + 500)
        self.assertFalse(proposal.changed)
        self.assertIs(proposal.ledger, saved)
        self.assertEqual(proposal.added_ids, ())

    def test_new_uses_do_not_repeat_previous_rows(self):
        saved = imported()
        proposal = saved.propose(snapshot(range(1, 6)), now_epoch=NOW)
        self.assertEqual(len(proposal.ledger.records), 5)
        self.assertEqual(proposal.added_ids, (f"{A}:4", f"{A}:5"))
        self.assertEqual(len(saved.records), 3)

    def test_json_round_trip_preserves_archive_and_cursor(self):
        saved = imported(range(50, 68))
        restored = Ledger(DEVICE, json.loads(json.dumps(saved.to_data())))
        self.assertFalse(restored.propose(snapshot(range(50, 68))).changed)
        self.assertEqual(restored.to_data(), saved.to_data())

    def test_offline_ring_overrun_gap_only_once(self):
        saved = imported(range(1, 4))
        proposal = saved.propose(snapshot(range(20, 44)), now_epoch=NOW)
        self.assertEqual(proposal.ledger.missing_count("missed_after_sync"), 16)
        self.assertEqual(proposal.ledger.missing_count("before_first_sync"), 0)
        self.assertFalse(proposal.ledger.propose(snapshot(range(20, 44))).changed)

    def test_missing_internal_sequences_and_later_recovery(self):
        saved = imported([1, 3, 6])
        self.assertEqual(saved.missing_count("missed_after_sync"), 3)
        recovered = saved.propose(snapshot([2, 3, 4, 5, 6])).ledger
        self.assertEqual(recovered.missing_count("missed_after_sync"), 0)
        self.assertEqual(len(recovered.records), 6)

    def test_empty_stream_followed_by_large_window_is_observed_gap(self):
        saved = imported([])
        recovered = saved.propose(snapshot(range(9, 33))).ledger
        self.assertEqual(recovered.missing_count("missed_after_sync"), 8)
        self.assertEqual(recovered.missing_count("before_first_sync"), 0)

    def test_late_clock_does_not_duplicate_or_erase_quantities(self):
        unknown = [session(1, unknown=True)]
        saved = Ledger(DEVICE).propose(snapshot(rows=unknown), now_epoch=NOW).ledger
        self.assertEqual(saved.undated_count, 1)
        proposal = saved.propose(snapshot([1]), now_epoch=NOW + 30)
        self.assertEqual(len(proposal.ledger.records), 1)
        self.assertEqual(proposal.ledger.undated_count, 0)
        self.assertEqual(proposal.updated_ids, (f"{A}:1",))
        self.assertEqual(proposal.ledger.latest["imported_at_epoch_s"], NOW)

    def test_clock_correction_updates_same_identity(self):
        saved = imported([1])
        rows = [session(1, start=1700007200)]
        proposal = saved.propose(snapshot(rows=rows))
        self.assertEqual(proposal.ledger.latest["started_at_epoch_s"], 1700007200)
        self.assertEqual(len(proposal.ledger.records), 1)

    def test_missing_dates_do_not_remove_saved_clock(self):
        saved = imported([1])
        proposal = saved.propose(snapshot(rows=[session(1, unknown=True)]))
        self.assertFalse(proposal.changed)
        self.assertEqual(proposal.ledger.undated_count, 0)

    def test_prices_frozen_at_first_import_even_if_tariffs_change(self):
        saved = imported([1])
        cfg = {"gas_type": "GN", "gas_unit": "m3", "gas_tariff": 14.0, "water_tariff": 21.0}
        proposal = saved.propose(snapshot([1], settings=cfg))
        self.assertFalse(proposal.changed)
        self.assertEqual(proposal.ledger.latest["tariffs_at_import"]["gas_tariff"], 8.5)

    def test_unknown_prices_stay_unknown_after_later_configuration(self):
        cfg = {"gas_type": "GN", "gas_unit": "m3", "gas_tariff": 0, "water_tariff": 0}
        saved = imported([1], settings=cfg)
        self.assertIsNone(saved.latest["cost"])
        proposal = saved.propose(snapshot([1]))
        self.assertFalse(proposal.changed)
        self.assertIsNone(proposal.ledger.latest["cost"])

    def test_gas_type_change_does_not_reprice_old_use(self):
        saved = imported([1])
        cfg = {"gas_type": "GLP", "gas_unit": "kg", "gas_tariff": 12.0, "water_tariff": 12.0}
        proposal = saved.propose(snapshot([1, 2], settings=cfg))
        self.assertEqual(proposal.ledger.records[0]["gas_unit"], "m3")
        self.assertEqual(proposal.ledger.records[1]["gas_unit"], "kg")

    def test_physical_conflict_is_atomic(self):
        saved = imported()
        before = saved.to_data()
        value = snapshot(range(1, 5))
        value["sessions"][1]["duration_s"] += 60
        with self.assertRaises(ArchiveConflict):
            saved.propose(value)
        self.assertEqual(saved.to_data(), before)

    def test_minor_quantity_rounding_does_not_mutate_saved_measurements(self):
        saved = imported([1])
        value = snapshot([1])
        value["sessions"][0]["duration_s"] += .001
        self.assertFalse(saved.propose(value).changed)

    def test_limit_never_prunes_or_advances_cursor(self):
        saved = imported([1, 2])
        before = saved.to_data()
        with self.assertRaises(ArchiveFull):
            saved.propose(snapshot([1, 2, 3]), max_records=2)
        self.assertEqual(saved.to_data(), before)
        self.assertEqual(len(saved.propose(snapshot([1, 2, 3]), max_records=3).ledger.records), 3)

    def test_source_reset_new_dated_use_is_independent(self):
        saved = imported([1, 2])
        value = snapshot(rows=[session(1, B, start=1701000000)], stream=B)
        proposal = saved.propose(value)
        self.assertEqual(len(proposal.ledger.records), 3)
        self.assertEqual(len(proposal.ledger.data["streams"]), 2)
        self.assertEqual(proposal.ledger.latest["id"], f"{B}:1")

    def test_source_fallback_dated_duplicates_get_alias_not_new_records(self):
        saved = imported([1, 2])
        value = snapshot([1, 2], stream=B)
        proposal = saved.propose(value)
        self.assertEqual(len(proposal.ledger.records), 2)
        self.assertEqual(proposal.added_ids, ())
        self.assertEqual(proposal.ledger.data["aliases"], {f"{B}:1": f"{A}:1", f"{B}:2": f"{A}:2"})
        restored = Ledger(DEVICE, proposal.ledger.to_data())
        self.assertFalse(restored.propose(value).changed)

    def test_rollback_revisits_known_stream_without_double_counting(self):
        saved = imported([1, 2])
        saved = saved.propose(snapshot([1, 2], stream=B)).ledger
        proposal = saved.propose(snapshot([1, 2], stream=A))
        self.assertEqual(len(proposal.ledger.records), 2)
        self.assertEqual(proposal.added_ids, ())

    def test_fallback_alias_does_not_double_count_old_unavailable_history(self):
        saved = imported(range(44, 68))
        after = saved.propose(snapshot(range(44, 68), stream=B)).ledger
        self.assertEqual(after.missing_count("before_first_sync"), 43)
        self.assertEqual(after.missing_count("missed_after_sync"), 0)
        self.assertEqual(len(after.records), 24)

    def test_cross_stream_recovery_reduces_original_gap(self):
        saved = imported([1, 2])
        saved = saved.propose(snapshot(range(20, 44))).ledger
        self.assertEqual(saved.missing_count("missed_after_sync"), 17)
        # Strong dated aliases at20+ prove this is the same surviving history.
        recovered = saved.propose(snapshot([5, 20, 21], stream=B)).ledger
        self.assertEqual(recovered.missing_count("missed_after_sync"), 16)
        self.assertEqual(recovered.missing_count("before_first_sync"), 0)

    def test_ambiguous_undated_fallback_fails_closed(self):
        saved = imported(rows=[session(1, unknown=True)])
        before = saved.to_data()
        with self.assertRaises(ArchiveConflict):
            saved.propose(snapshot(rows=[session(1, B, unknown=True)], stream=B))
        self.assertEqual(saved.to_data(), before)

    def test_conflicting_physical_values_same_dates_after_reset_fails(self):
        saved = imported([1])
        value = snapshot([1], stream=B)
        value["sessions"][0]["duration_s"] = 30
        with self.assertRaises(ArchiveConflict):
            saved.propose(value)

    def test_regressed_snapshot_does_not_decrease_cursor(self):
        saved = imported(range(1, 8))
        proposal = saved.propose(snapshot(range(1, 4)))
        self.assertEqual(len(proposal.ledger.records), 7)
        self.assertEqual(proposal.ledger.data["streams"][A]["highest_sequence"], 7)
        self.assertTrue(proposal.ledger.data["streams"][A]["source_regressed"])
        self.assertEqual(proposal.added_ids, ())

    def test_sequence_exhausted_is_recorded_but_not_invented_use(self):
        value = snapshot([])
        value["sequence_exhausted"] = True
        result = Ledger(DEVICE).propose(value).ledger
        self.assertTrue(result.data["streams"][A]["sequence_exhausted"])
        self.assertEqual(result.records, [])

    def test_query_pagination_unknown_dates_and_overlap(self):
        rows = [session(1), session(2, unknown=True), session(3)]
        saved = imported(rows=rows)
        page = saved.query(limit=2)
        self.assertEqual(page["total"], 3)
        self.assertEqual(page["next_offset"], 2)
        self.assertEqual([r["sequence"] for r in page["records"]], [3, 2])
        self.assertEqual([r["sequence"] for r in saved.query(offset=2)["records"]], [1])
        self.assertEqual(saved.query(include_undated=False)["total"], 2)
        self.assertEqual(saved.query(start=1700000600, end=1700000720, include_undated=False)["total"], 1)
        self.assertEqual(saved.query(start=1700000720, end=1700000800, include_undated=False)["total"], 0)

    def test_query_results_cannot_mutate_archive(self):
        saved = imported([1])
        result = saved.query()
        result["records"][0]["water_l"] = 9000
        self.assertNotEqual(saved.latest["water_l"], 9000)
        copy = saved.to_data()
        copy["records"].clear()
        self.assertEqual(len(saved.records), 1)

    def test_wrong_device_never_merges(self):
        value = snapshot([1])
        value["device_id"] = "rinnai-112233445566"
        with self.assertRaises(ArchiveConflict):
            Ledger(DEVICE).propose(value)

    def test_malformed_snapshot_and_records_are_rejected(self):
        mutations = [
            lambda x: x.update(schema_version=True),
            lambda x: x.update(ready=False),
            lambda x: x.update(storage_ok=False),
            lambda x: x.update(stream_id="boot-1"),
            lambda x: x.update(capacity=100),
            lambda x: x.update(total_sessions=True),
            lambda x: x.update(oldest_sequence=True),
            lambda x: x.update(newest_sequence=None),
            lambda x: x.update(sequence_exhausted="false"),
            lambda x: x.update(pricing_basis="at_use"),
            lambda x: x["sessions"][0].update(id="same-on-every-boot"),
            lambda x: x["sessions"][0].update(water_l=True),
            lambda x: x["sessions"][0].update(duration_s=-1),
            lambda x: x["sessions"][0].update(energy_kwh=float("nan")),
            lambda x: x["sessions"][0].update(gas=float("inf")),
            lambda x: x["sessions"][0].update(cost_complete="yes"),
            lambda x: x["sessions"][0].update(elapsed_s=500),
            lambda x: x["sessions"][0].update(ended_at_epoch_s=None),
            lambda x: x["sessions"][0].update(end_reason="<script>"),
            lambda x: x["sessions"][0].update(gas_cost=None),
            lambda x: x["sessions"][0].update(cost=999),
            lambda x: x["sessions"].append(deepcopy(x["sessions"][0])),
        ]
        for index, mutate in enumerate(mutations):
            with self.subTest(index=index):
                value = snapshot([1]);mutate(value)
                with self.assertRaises(ArchiveError):
                    Ledger(DEVICE).propose(value)

    def test_malformed_saved_archive_fails_without_overwriting(self):
        mutations = [
            lambda x: x.update(schema_version=True),
            lambda x: x.update(device_id="different"),
            lambda x: x.update(revision=-1),
            lambda x: x.update(records={}),
            lambda x: x.update(current_stream_id=B),
            lambda x: x["records"].append(deepcopy(x["records"][0])),
            lambda x: x["streams"][A].update(gaps=[None]),
            lambda x: x["streams"][A].update(highest_sequence=0),
            lambda x: x.update(aliases={f"{B}:1": f"{A}:1"}),
        ]
        data = imported([1]).to_data()
        for index, mutate in enumerate(mutations):
            with self.subTest(index=index):
                value = deepcopy(data);mutate(value)
                with self.assertRaises(ArchiveError):
                    Ledger(DEVICE, value)

    def test_frozen_pricing_survives_restart(self):
        saved = imported([1])
        restored = Ledger(DEVICE, saved.to_data())
        self.assertEqual(restored.latest["pricing_basis"], "tariffs_at_import")
        self.assertEqual(restored.latest["cost"], saved.latest["cost"])

    def test_extremely_large_json_integer_is_a_typed_archive_error(self):
        value = snapshot([1])
        value["sessions"][0]["water_l"] = 10**1000
        with self.assertRaises(ArchiveError):
            Ledger(DEVICE).propose(value)

    def test_query_rejects_wrong_option_types(self):
        saved = imported([1])
        for kwargs in ({"include_undated": "false"}, {"limit": True}, {"offset": -1}, {"start": False}):
            with self.subTest(kwargs=kwargs):
                with self.assertRaises(ArchiveError):
                    saved.query(**kwargs)

    def test_saved_diagnostics_flags_are_strict_booleans(self):
        for key in ("source_regressed", "sequence_exhausted"):
            value = imported([1]).to_data()
            value["streams"][A][key] = "false"
            with self.assertRaises(ArchiveError):
                Ledger(DEVICE, value)


if __name__ == "__main__":
    unittest.main()
