"""Shared device metadata and offline archive availability."""
from homeassistant.helpers.entity import DeviceInfo
from homeassistant.helpers.update_coordinator import CoordinatorEntity

from .const import DOMAIN


class ArchiveEntity(CoordinatorEntity):
    _attr_has_entity_name = True

    @property
    def available(self):
        # These are historical records, independent of live source availability.
        return self.coordinator.ledger is not None

    @property
    def device_info(self):
        return DeviceInfo(identifiers={(DOMAIN, self.coordinator.device_id)},
                          name="Histórico Rinnai", manufacturer="Rinnai Local",
                          model="Arquivo de usos concluídos", configuration_url=self.coordinator.base_url)
