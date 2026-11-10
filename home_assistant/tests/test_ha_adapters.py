"""Run in the target HA Python runtime; uses no running HA or real device."""
import asyncio
from copy import deepcopy
from datetime import datetime, timezone
import json
from pathlib import Path
from tempfile import TemporaryDirectory
from types import SimpleNamespace
import unittest
from unittest.mock import AsyncMock, patch

from custom_components.rinnai_local.calendar import RinnaiCalendar, calendar_events
from custom_components.rinnai_local.client import normalize_url
from custom_components.rinnai_local.coordinator import RinnaiCoordinator
from custom_components.rinnai_local.ledger import Ledger
from custom_components.rinnai_local.sensor import DESCRIPTIONS, RinnaiArchiveSensor
from custom_components.rinnai_local.storage import DurableArchive, PersistenceError
from homeassistant.helpers.update_coordinator import UpdateFailed
from test_ledger import DEVICE, NOW, snapshot


class FakeHass:
    async def async_add_executor_job(self, function, *args):
        return await asyncio.to_thread(function, *args)


class FakeStore:
    def __init__(self, path):
        self.path = str(path)
        self.suppress = False
        self.corrupt = False
        self.writes = 0

    async def async_save(self, data):
        if self.suppress:
            return  # Same observable behavior as a swallowed HA write error.
        self.writes += 1
        document = {"version": 1, "data": deepcopy(data)}
        if self.corrupt:
            document["data"]["commit_token"] = "old-token"
        target = Path(self.path)
        temporary = target.with_suffix(".tmp")
        temporary.write_text(json.dumps(document), encoding="utf-8")
        temporary.replace(target)

    async def async_load(self):
        target = Path(self.path)
        return json.loads(target.read_text(encoding="utf-8"))["data"] if target.exists() else None


def archive_at(path):
    archive = DurableArchive.__new__(DurableArchive)
    archive.hass = FakeHass()
    archive.device_id = DEVICE
    archive.store = FakeStore(path)
    return archive


class PersistenceTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.folder = TemporaryDirectory(prefix="rinnai-store-test-")
        self.addCleanup(self.folder.cleanup)
        self.archive = archive_at(Path(self.folder.name)/"archive.json")
        self.first = Ledger(DEVICE).propose(snapshot([1]), now_epoch=NOW).ledger

    async def test_confirmed_commit_and_restart_restore(self):
        await self.archive.async_commit(self.first)
        restored = await self.archive.async_load()
        self.assertEqual(restored.to_data(), self.first.to_data())

    async def test_suppressed_write_preserves_previous_archive(self):
        await self.archive.async_commit(self.first)
        old = Path(self.archive.store.path).read_bytes()
        candidate = self.first.propose(snapshot([1, 2]), now_epoch=NOW).ledger
        self.archive.store.suppress = True
        with self.assertRaises(PersistenceError):
            await self.archive.async_commit(candidate)
        self.assertEqual(Path(self.archive.store.path).read_bytes(), old)
        self.assertEqual((await self.archive.async_load()).to_data(), self.first.to_data())

    async def test_wrong_commit_token_is_not_acknowledged(self):
        self.archive.store.corrupt = True
        with self.assertRaises(PersistenceError):
            await self.archive.async_commit(self.first)

    async def test_existing_unreadable_archive_never_becomes_empty_archive(self):
        Path(self.archive.store.path).write_text("not-json", encoding="utf-8")
        self.archive.store.async_load = AsyncMock(return_value=None)
        with self.assertRaises(PersistenceError):
            await self.archive.async_load()
        self.assertEqual(Path(self.archive.store.path).read_text(), "not-json")

    async def test_integrity_mismatch_rejects_restore(self):
        await self.archive.async_commit(self.first)
        path = Path(self.archive.store.path)
        raw = json.loads(path.read_text())
        raw["data"]["ledger"]["records"][0]["water_l"] += 1
        path.write_text(json.dumps(raw))
        with self.assertRaises(PersistenceError):
            await self.archive.async_load()

    def coordinator(self):
        result = RinnaiCoordinator.__new__(RinnaiCoordinator)
        result.hass = FakeHass()
        result.device_id, result.base_url, result.max_records = DEVICE, "http://192.168.1.50", 10000
        result.ledger, result.archive = self.first, self.archive
        result.last_error_code, result.last_seen = None, None
        result._commit_lock, result._stopping = asyncio.Lock(), False
        return result

    async def test_coordinator_does_not_advance_on_failed_save_and_retries(self):
        coordinator = self.coordinator()
        self.archive.store.suppress = True
        with patch("custom_components.rinnai_local.coordinator.async_get_clientsession", return_value=object()), \
             patch("custom_components.rinnai_local.coordinator.async_fetch_snapshot", AsyncMock(return_value=snapshot([1, 2]))):
            with self.assertRaises(UpdateFailed):
                await coordinator._async_update_data()
            self.assertIs(coordinator.ledger, self.first)
            self.assertEqual(coordinator.last_error_code, "storage_error")
            self.archive.store.suppress = False
            await coordinator._async_update_data()
            self.assertEqual(len(coordinator.ledger.records), 2)
            self.assertIsNone(coordinator.last_error_code)

    async def test_duplicate_snapshot_does_not_rewrite_store(self):
        coordinator = self.coordinator()
        with patch("custom_components.rinnai_local.coordinator.async_get_clientsession", return_value=object()), \
             patch("custom_components.rinnai_local.coordinator.async_fetch_snapshot", AsyncMock(return_value=snapshot([1]))):
            await coordinator._async_update_data()
            await coordinator._async_update_data()
        self.assertEqual(self.archive.store.writes, 0)

    async def test_stop_drains_old_writer_before_reload(self):
        coordinator = self.coordinator()
        started, release = asyncio.Event(), asyncio.Event()
        async def slow_fetch(*args):
            started.set()
            await release.wait()
            return snapshot([1, 2])
        with patch("custom_components.rinnai_local.coordinator.async_get_clientsession", return_value=object()), \
             patch("custom_components.rinnai_local.coordinator.async_fetch_snapshot", side_effect=slow_fetch) as network:
            update = asyncio.create_task(coordinator._async_update_data())
            await started.wait()
            stop = asyncio.create_task(coordinator.async_stop())
            await asyncio.sleep(0)
            self.assertFalse(stop.done())
            release.set()
            await update
            await stop
            await coordinator._async_update_data()
            self.assertEqual(network.call_count, 1)
            self.assertEqual(len((await self.archive.async_load()).records), 2)


class PresentationTests(unittest.TestCase):
    def test_calendar_preserves_real_times_and_skips_unknown_times(self):
        known = Ledger(DEVICE).propose(snapshot([1, 2]), now_epoch=NOW).ledger
        rows = deepcopy(known.records)
        rows[1].update(started_at_epoch_s=None, ended_at_epoch_s=None, elapsed_s=None)
        events = calendar_events(rows, datetime(2020, 1, 1, tzinfo=timezone.utc), datetime(2030, 1, 1, tzinfo=timezone.utc))
        self.assertEqual(len(events), 1)
        self.assertEqual(events[0].uid, rows[0]["id"])
        self.assertEqual(events[0].start.timestamp(), rows[0]["started_at_epoch_s"])
        self.assertIn("Tarifas da primeira importação", events[0].description)

    def test_archive_entities_remain_available_with_source_offline(self):
        ledger = Ledger(DEVICE).propose(snapshot([1]), now_epoch=NOW).ledger
        coordinator = SimpleNamespace(ledger=ledger, device_id=DEVICE, last_update_success=False,
                                      last_error_code="cannot_connect", base_url="http://192.168.1.50")
        calendar = RinnaiCalendar(coordinator)
        self.assertTrue(calendar.available)
        water = RinnaiArchiveSensor(coordinator, next(row for row in DESCRIPTIONS if row[0] == "water"))
        self.assertTrue(water.available)
        self.assertEqual(water.native_value, ledger.latest["water_l"])
        state = RinnaiArchiveSensor(coordinator, next(row for row in DESCRIPTIONS if row[0] == "sync"))
        self.assertEqual(state.native_value, "cannot_connect")

    def test_base_url_validation(self):
        self.assertEqual(normalize_url("http://192.168.1.50/"), "http://192.168.1.50")
        for invalid in ("file:///tmp/test", "http://user:secret@example.invalid", "http://192.168.1.50/api/v1", "http://192.168.1.50?token=example"):
            with self.subTest(invalid=invalid), self.assertRaises(ValueError):
                normalize_url(invalid)


if __name__ == "__main__":
    unittest.main()
