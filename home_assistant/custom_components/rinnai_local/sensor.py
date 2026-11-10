"""Summaries of the durable archive and its synchronization health."""
from datetime import datetime, timezone

from homeassistant.components.sensor import SensorDeviceClass, SensorEntity
from homeassistant.const import EntityCategory

from .const import DOMAIN
from .entity import ArchiveEntity


# key, display name, unit, device class, icon
DESCRIPTIONS = (
    ("archive_count", "Usos arquivados", None, None, "mdi:counter"),
    ("missing", "Usos perdidos após sincronização", None, None, "mdi:alert-circle-outline"),
    ("prior", "Usos anteriores indisponíveis", None, None, "mdi:history"),
    ("undated", "Usos sem horário", None, None, "mdi:clock-alert-outline"),
    ("duration", "Último uso duração", "s", SensorDeviceClass.DURATION, None),
    ("water", "Último uso água", "L", SensorDeviceClass.WATER, None),
    ("energy", "Último uso energia estimada", "kWh", SensorDeviceClass.ENERGY, None),
    ("gas", "Último uso gás estimado", None, None, "mdi:fire"),
    ("water_cost", "Último uso custo água", "BRL", SensorDeviceClass.MONETARY, None),
    ("gas_cost", "Último uso custo gás", "BRL", SensorDeviceClass.MONETARY, None),
    ("cost", "Último uso custo total", "BRL", SensorDeviceClass.MONETARY, None),
    ("ended", "Último uso término", None, SensorDeviceClass.TIMESTAMP, None),
    ("sync", "Estado da sincronização", None, None, "mdi:sync"),
)


async def async_setup_entry(hass, entry, async_add_entities):
    coordinator = hass.data[DOMAIN][entry.entry_id]
    async_add_entities([RinnaiArchiveSensor(coordinator, row) for row in DESCRIPTIONS])


class RinnaiArchiveSensor(ArchiveEntity, SensorEntity):
    def __init__(self, coordinator, description):
        super().__init__(coordinator)
        self.key, self._attr_name, self._unit, self._attr_device_class, self._attr_icon = description
        self._attr_unique_id = f"{coordinator.device_id}_archive_{self.key}"
        if self.key in ("missing", "prior", "undated", "sync"):
            self._attr_entity_category = EntityCategory.DIAGNOSTIC

    @property
    def native_unit_of_measurement(self):
        if self.key == "gas":
            row = self.coordinator.ledger.latest
            return ("m³" if row["gas_unit"] == "m3" else "kg") if row else None
        return self._unit

    @property
    def native_value(self):
        ledger = self.coordinator.ledger
        if self.key == "archive_count":
            return len(ledger.records)
        if self.key == "missing":
            return ledger.missing_count("missed_after_sync")
        if self.key == "prior":
            return ledger.missing_count("before_first_sync")
        if self.key == "undated":
            return ledger.undated_count
        if self.key == "sync":
            if not self.coordinator.last_update_success:
                return self.coordinator.last_error_code or "indisponível"
            if ledger.missing_count("missed_after_sync"):
                return "lacunas_detectadas"
            stream = ledger.data["streams"].get(ledger.data["current_stream_id"], {})
            if stream.get("sequence_exhausted"):
                return "sequência_esgotada"
            if stream.get("source_regressed"):
                return "fonte_regrediu"
            return "sincronizado"
        row = ledger.latest
        if row is None:
            return None
        if self.key == "duration":
            return row["elapsed_s"] if row["elapsed_s"] is not None else row["duration_s"]
        if self.key == "ended":
            epoch = row["ended_at_epoch_s"]
            return datetime.fromtimestamp(epoch, timezone.utc) if epoch is not None else None
        return row[{"water": "water_l", "energy": "energy_kwh", "gas": "gas"}.get(self.key, self.key)]

    @property
    def extra_state_attributes(self):
        coordinator = self.coordinator
        if self.key == "sync":
            return {"last_success": coordinator.last_seen.isoformat() if coordinator.last_seen else None,
                    "last_error": coordinator.last_error_code, "archive_limit": coordinator.max_records,
                    "revision": coordinator.ledger.data["revision"], "source_capacity": 24}
        row = coordinator.ledger.latest
        if row and self.key in ("duration", "water", "energy", "gas", "water_cost", "gas_cost", "cost", "ended"):
            return {"session_id": row["id"], "sequence": row["sequence"], "complete": row["complete"],
                    "started_at_epoch_s": row["started_at_epoch_s"], "ended_at_epoch_s": row["ended_at_epoch_s"],
                    "observed_duration_s": row["duration_s"], "pricing_basis": row["pricing_basis"]}
        return None
