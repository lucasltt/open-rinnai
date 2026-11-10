"""Read-only calendar over the durable archive; no synthetic event times."""
from datetime import datetime, timezone

from homeassistant.components.calendar import CalendarEntity, CalendarEvent

from .const import DOMAIN
from .entity import ArchiveEntity


def calendar_events(records, start_date, end_date):
    events = []
    lower, upper = start_date.timestamp(), end_date.timestamp()
    for row in records:
        start, end = row["started_at_epoch_s"], row["ended_at_epoch_s"]
        if start is None or end is None or end <= start or end <= lower or start >= upper:
            continue
        elapsed = row["elapsed_s"]
        length = elapsed if elapsed is not None else row["duration_s"]
        minutes, seconds = divmod(int(length), 60)
        summary = f"Água quente · {minutes}min {seconds:02d}s · {row['water_l']:.1f} L"
        def cost(value):
            return "sem tarifa" if value is None else f"R$ {value:.2f}"
        description = (
            f"Uso #{row['sequence']}\n"
            f"Tempo decorrido: {length:.0f} s\nTempo acompanhado: {row['duration_s']:.1f} s\n"
            f"Tempo aquecendo: {row['heating_s']:.0f} s\nÁgua: {row['water_l']:.3f} L\n"
            f"Energia estimada: {row['energy_kwh']:.5f} kWh\nGás estimado: {row['gas']:.5f} {row['gas_unit']}\n"
            f"Água estimada: {cost(row['water_cost'])}\nGás estimado: {cost(row['gas_cost'])}\n"
            f"Total estimado: {cost(row['cost'])}\nTarifas da primeira importação.\n"
            f"Cobertura: {'completa' if row['complete'] else 'incompleta'} ({row['end_reason']})\n"
            f"ID: {row['id']}"
        )
        events.append(CalendarEvent(start=datetime.fromtimestamp(start, timezone.utc),
            end=datetime.fromtimestamp(end, timezone.utc), summary=summary,
            description=description, uid=row["id"]))
    return sorted(events, key=lambda event: event.start)


async def async_setup_entry(hass, entry, async_add_entities):
    async_add_entities([RinnaiCalendar(hass.data[DOMAIN][entry.entry_id])])


class RinnaiCalendar(ArchiveEntity, CalendarEntity):
    _attr_name = "Usos de água quente"
    _attr_icon = "mdi:shower-head"
    _attr_supported_features = 0

    def __init__(self, coordinator):
        super().__init__(coordinator)
        self._attr_unique_id = coordinator.device_id + "_completed_uses_calendar"

    @property
    def event(self):
        # This calendar is an archive, not an appointment scheduler.
        return None

    async def async_get_events(self, hass, start_date, end_date):
        records = self.coordinator.ledger.records
        return await hass.async_add_executor_job(calendar_events, records, start_date, end_date)
