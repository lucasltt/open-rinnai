"""User setup and host/retention options for the local completed-use archive."""
try:
    import probatio as vol
except ImportError:
    import voluptuous as vol

import aiohttp
from homeassistant import config_entries
from homeassistant.core import callback
from homeassistant.helpers.aiohttp_client import async_get_clientsession

from .client import async_fetch_snapshot, normalize_url
from .const import CONF_BASE_URL, CONF_DEVICE_ID, CONF_MAX_RECORDS, DEFAULT_MAX_RECORDS, DOMAIN, MAX_RECORDS
from .ledger import ArchiveError


def schema(base="http://", maximum=DEFAULT_MAX_RECORDS):
    return vol.Schema({vol.Required(CONF_BASE_URL, default=base): str,
                       vol.Required(CONF_MAX_RECORDS, default=maximum): vol.All(vol.Coerce(int), vol.Range(min=24, max=MAX_RECORDS))})


class RinnaiConfigFlow(config_entries.ConfigFlow, domain=DOMAIN):
    VERSION = 1

    async def async_step_user(self, user_input=None):
        errors = {}
        if user_input is not None:
            try:
                base = normalize_url(user_input[CONF_BASE_URL])
                result = await async_fetch_snapshot(async_get_clientsession(self.hass), base)
                await self.async_set_unique_id(result["device_id"])
                self._abort_if_unique_id_configured()
                return self.async_create_entry(title="Histórico Rinnai", data={
                    CONF_BASE_URL: base, CONF_DEVICE_ID: result["device_id"],
                    CONF_MAX_RECORDS: user_input[CONF_MAX_RECORDS]})
            except (ValueError, ArchiveError):
                errors["base"] = "invalid_device"
            except (aiohttp.ClientError, TimeoutError, OSError):
                errors["base"] = "cannot_connect"
        return self.async_show_form(step_id="user", data_schema=schema(
            (user_input or {}).get(CONF_BASE_URL, "http://"),
            (user_input or {}).get(CONF_MAX_RECORDS, DEFAULT_MAX_RECORDS)), errors=errors)

    @staticmethod
    @callback
    def async_get_options_flow(config_entry):
        return RinnaiOptionsFlow(config_entry)


class RinnaiOptionsFlow(config_entries.OptionsFlow):
    def __init__(self, entry):
        self._entry = entry

    async def async_step_init(self, user_input=None):
        errors = {}
        previous = self._entry.options.get(CONF_BASE_URL, self._entry.data[CONF_BASE_URL])
        if user_input is not None:
            try:
                base = normalize_url(user_input[CONF_BASE_URL])
                if base != previous:
                    await async_fetch_snapshot(async_get_clientsession(self.hass), base, self._entry.data[CONF_DEVICE_ID])
                return self.async_create_entry(title="", data={CONF_BASE_URL: base, CONF_MAX_RECORDS: user_input[CONF_MAX_RECORDS]})
            except (ValueError, ArchiveError):
                errors["base"] = "invalid_device"
            except (aiohttp.ClientError, TimeoutError, OSError):
                errors["base"] = "cannot_connect"
        return self.async_show_form(step_id="init", data_schema=schema(previous,
            self._entry.options.get(CONF_MAX_RECORDS, self._entry.data.get(CONF_MAX_RECORDS, DEFAULT_MAX_RECORDS))), errors=errors)
