"""Fetch, validate, durably commit, then publish the completed-use archive."""
from datetime import timedelta
from functools import partial
import asyncio
import logging

import aiohttp
from homeassistant.helpers.aiohttp_client import async_get_clientsession
from homeassistant.helpers.update_coordinator import DataUpdateCoordinator, UpdateFailed
from homeassistant.util import dt as dt_util

from .client import async_fetch_snapshot
from .const import CONF_BASE_URL, CONF_DEVICE_ID, CONF_MAX_RECORDS, DEFAULT_MAX_RECORDS, DOMAIN, POLL_SECONDS
from .ledger import ArchiveError
from .storage import DurableArchive

LOGGER = logging.getLogger(__name__)


class RinnaiCoordinator(DataUpdateCoordinator):
    def __init__(self, hass, entry):
        super().__init__(hass, LOGGER, name=DOMAIN, config_entry=entry,
                         update_interval=timedelta(seconds=POLL_SECONDS))
        self.entry = entry
        self.device_id = entry.data[CONF_DEVICE_ID]
        self.base_url = entry.options.get(CONF_BASE_URL, entry.data[CONF_BASE_URL])
        self.max_records = entry.options.get(CONF_MAX_RECORDS, entry.data.get(CONF_MAX_RECORDS, DEFAULT_MAX_RECORDS))
        self.archive = DurableArchive(hass, self.device_id)
        self.ledger = None
        self.last_error_code = None
        self.last_seen = None
        self._commit_lock = asyncio.Lock()
        self._stopping = False

    async def async_load_archive(self):
        self.ledger = await self.archive.async_load()
        self.data = self.ledger

    async def _async_update_data(self):
        async with self._commit_lock:
            if self._stopping:
                return self.ledger
            return await self._async_fetch_commit()

    async def async_stop(self):
        """Drain an old writer before reload opens another Store for this key."""
        self._stopping = True
        async with self._commit_lock:
            pass

    async def _async_fetch_commit(self):
        try:
            snapshot = await async_fetch_snapshot(async_get_clientsession(self.hass), self.base_url, self.device_id)
            proposal = await self.hass.async_add_executor_job(
                partial(self.ledger.propose, snapshot, max_records=self.max_records))
            if proposal.changed:
                await self.archive.async_commit(proposal.ledger)
                self.ledger = proposal.ledger
            self.last_seen = dt_util.utcnow()
            self.last_error_code = None
            return self.ledger
        except (ArchiveError, aiohttp.ClientError, TimeoutError, OSError) as error:
            self.last_error_code = getattr(error, "code", "cannot_connect")
            raise UpdateFailed(f"Completed-use archive: {self.last_error_code}") from error
