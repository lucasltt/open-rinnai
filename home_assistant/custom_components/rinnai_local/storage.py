"""Save an archive atomically and verify it before advancing its cursor."""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import secrets

from homeassistant.core import HomeAssistant
from homeassistant.helpers.storage import Store

from .const import DOMAIN, STORAGE_VERSION
from .ledger import ArchiveError, Ledger


class PersistenceError(ArchiveError):
    code = "storage_error"


def digest(data: dict) -> str:
    raw = json.dumps(data, sort_keys=True, separators=(",", ":"), ensure_ascii=False, allow_nan=False).encode()
    return hashlib.sha256(raw).hexdigest()


class DurableArchive:
    def __init__(self, hass: HomeAssistant, device_id: str):
        self.hass = hass
        self.device_id = device_id
        # Device identity rather than config-entry ID preserves the archive on
        # removal/re-addition. Normal unload never deletes this store.
        self.store = Store(hass, STORAGE_VERSION, f"{DOMAIN}.{device_id}", private=True,
                           atomic_writes=True, serialize_in_event_loop=False)

    async def async_load(self) -> Ledger:
        existed = await self.hass.async_add_executor_job(Path(self.store.path).exists)
        saved = await self.store.async_load()
        if saved is None:
            if existed:
                raise PersistenceError("Existing archive could not be loaded; it was preserved")
            return Ledger(self.device_id)
        try:
            ledger = saved["ledger"]
            actual = await self.hass.async_add_executor_job(digest, ledger)
            if actual != saved["sha256"] or not isinstance(saved["commit_token"], str):
                raise ValueError("Archive integrity mismatch")
            return await self.hass.async_add_executor_job(Ledger, self.device_id, ledger)
        except (KeyError, TypeError, ValueError) as error:
            raise PersistenceError("Stored archive failed validation; no replacement was written") from error

    def _confirm(self, expected: dict) -> bool:
        try:
            saved = json.loads(Path(self.store.path).read_text(encoding="utf-8"))
            actual = saved["data"]
            return (actual["commit_token"] == expected["commit_token"] and
                    actual["sha256"] == expected["sha256"] and
                    digest(actual["ledger"]) == expected["sha256"])
        except (OSError, ValueError, KeyError, TypeError):
            return False

    async def async_commit(self, ledger: Ledger) -> None:
        data = ledger.to_data()
        expected = {"ledger": data, "sha256": await self.hass.async_add_executor_job(digest, data),
                    "commit_token": secrets.token_hex(16)}
        # Some HA Store versions log write errors instead of raising them.
        # Awaiting async_save alone therefore is not a durable acknowledgement.
        await self.store.async_save(expected)
        if not await self.hass.async_add_executor_job(self._confirm, expected):
            raise PersistenceError("Archive write was not confirmed; the previous cursor remains active")
