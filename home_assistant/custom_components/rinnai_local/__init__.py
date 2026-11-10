"""Persistent completed-use history, complementary to native ESPHome control."""
from datetime import datetime

try:
    import probatio as vol
except ImportError:
    import voluptuous as vol

from homeassistant.const import Platform
from homeassistant.core import SupportsResponse
from homeassistant.exceptions import ConfigEntryNotReady, ServiceValidationError
from homeassistant.helpers import config_validation as cv
from homeassistant.util import dt as dt_util

from .const import DOMAIN, SERVICE_GET_HISTORY
from .coordinator import RinnaiCoordinator
from .ledger import ArchiveError

PLATFORMS = [Platform.SENSOR, Platform.CALENDAR]


def parse_time(value):
    if value is None:
        return None
    parsed = dt_util.parse_datetime(value) if isinstance(value, str) else value
    if not isinstance(parsed, datetime) or parsed.tzinfo is None:
        raise ServiceValidationError("Use an ISO datetime with timezone")
    return int(parsed.timestamp())


async def async_setup_entry(hass, entry):
    coordinator = RinnaiCoordinator(hass, entry)
    try:
        await coordinator.async_load_archive()
    except ArchiveError as error:
        raise ConfigEntryNotReady("Stored archive needs attention; it was preserved") from error
    if coordinator.ledger.records:
        # An existing archive remains usable after an HA restart while the ESP
        # is offline. Only a first installation requires a successful fetch.
        await coordinator.async_refresh()
    else:
        await coordinator.async_config_entry_first_refresh()
    hass.data.setdefault(DOMAIN, {})[entry.entry_id] = coordinator
    entry.async_on_unload(entry.add_update_listener(async_reload_entry))
    if not hass.services.has_service(DOMAIN, SERVICE_GET_HISTORY):
        async def get_history(call):
            coordinators = hass.data.get(DOMAIN, {})
            entry_id = call.data.get("entry_id")
            if entry_id is None:
                if len(coordinators) != 1:
                    raise ServiceValidationError("Select entry_id when more than one archive is configured")
                selected = next(iter(coordinators.values()))
            else:
                selected = coordinators.get(entry_id)
            if selected is None:
                raise ServiceValidationError("Unknown archive entry_id")
            try:
                result = selected.ledger.query(offset=call.data.get("offset", 0), limit=call.data.get("limit", 100),
                    start=parse_time(call.data.get("start")), end=parse_time(call.data.get("end")),
                    include_undated=call.data.get("include_undated", True))
                result["entry_id"] = selected.entry.entry_id
                result["sync_ok"] = selected.last_update_success
                result["last_sync_error"] = selected.last_error_code
                return result
            except ArchiveError as error:
                raise ServiceValidationError(str(error)) from error

        hass.services.async_register(DOMAIN, SERVICE_GET_HISTORY, get_history, schema=vol.Schema({
            vol.Optional("entry_id"): cv.string,
            vol.Optional("offset", default=0): vol.All(vol.Coerce(int), vol.Range(min=0, max=50000)),
            vol.Optional("limit", default=100): vol.All(vol.Coerce(int), vol.Range(min=1, max=500)),
            vol.Optional("start"): cv.string, vol.Optional("end"): cv.string,
            vol.Optional("include_undated", default=True): cv.boolean,
        }), supports_response=SupportsResponse.ONLY)
    await hass.config_entries.async_forward_entry_setups(entry, PLATFORMS)
    return True


async def async_reload_entry(hass, entry):
    await hass.config_entries.async_reload(entry.entry_id)


async def async_unload_entry(hass, entry):
    coordinator = hass.data[DOMAIN][entry.entry_id]
    await coordinator.async_stop()
    if await hass.config_entries.async_unload_platforms(entry, PLATFORMS):
        hass.data[DOMAIN].pop(entry.entry_id, None)
        if not hass.data[DOMAIN]:
            hass.services.async_remove(DOMAIN, SERVICE_GET_HISTORY)
        return True
    coordinator._stopping = False
    return False
