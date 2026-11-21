"""Request and response schemas."""

import re
from datetime import datetime

from pydantic import BaseModel, ConfigDict, Field, field_validator, model_validator

import calculators
from calculators import Shape


# Separators are optional and may be ":" or "-", so the MAC can be pasted in
# any of the usual spellings; what is left has to be exactly six hex octets.
_MAC_SEPARATORS = re.compile(r"[:-]")
_MAC_HEX = re.compile(r"[0-9A-Fa-f]{12}")


class Device(BaseModel):
    """A registered node. Rows are created by POST /devices during onboarding."""

    id: int
    node_id: str
    mac_address: str
    name: str | None = None
    tank_id: int | None = None
    sensor_offset_mm: int
    firmware: str | None = None
    is_online: bool
    created_at: datetime
    last_seen_at: datetime | None = None


class DeviceCreate(BaseModel):
    """Body of POST /devices: the MAC the app read from the node over BLE.

    The MAC is the device's identity - node_id is derived from it in the
    database - so it is normalised here to one canonical spelling
    (AA:BB:CC:DD:EE:FF) before it gets anywhere near SQL.
    """

    model_config = ConfigDict(extra="forbid")

    mac_address: str

    @field_validator("mac_address")
    @classmethod
    def normalise_mac(cls, value: str) -> str:
        digits = _MAC_SEPARATORS.sub("", value.strip())
        if not _MAC_HEX.fullmatch(digits):
            raise ValueError("expected a MAC address of six hex octets, e.g. A4:C1:F8:2B:70:C8")
        digits = digits.upper()
        return ":".join(digits[i:i + 2] for i in range(0, 12, 2))


class DeviceRegistration(Device):
    """What POST /devices answers: the device row plus what the node needs next.

    broker_uri is handed to the node over BLE as the last onboarding step, so
    the firmware carries no broker address of its own.
    """

    broker_uri: str


class DeviceUpdate(BaseModel):
    """The fields a human owns. Everything else is written by registration or ingest.

    name and tank_id are nullable *and* optional, and the two mean different
    things: omitting a field leaves it alone, sending null clears it. The
    handler tells them apart with model_dump(exclude_unset=True), so the
    defaults here are never actually used as values.

    sensor_offset_mm is the exception - its column is NOT NULL, so it can be
    changed but not cleared, and an explicit null is rejected rather than
    passed to the database to fail there as a 500.
    """

    model_config = ConfigDict(extra="forbid")

    name: str | None = None
    tank_id: int | None = None
    sensor_offset_mm: int | None = Field(default=None, ge=0)

    @model_validator(mode="after")
    def reject_null_offset(self):
        if "sensor_offset_mm" in self.model_fields_set and self.sensor_offset_mm is None:
            raise ValueError("sensor_offset_mm cannot be null")
        return self


class Tank(BaseModel):
    """A physical tank."""

    id: int
    name: str
    shape: Shape
    height_cm: float
    dimensions: dict
    created_at: datetime
    updated_at: datetime


class TankCreate(BaseModel):
    model_config = ConfigDict(extra="forbid")

    name: str = Field(min_length=1)
    shape: Shape
    height_cm: float = Field(gt=0)
    dimensions: dict

    @model_validator(mode="after")
    def check_dimensions(self):
        """dimensions has to carry the measurements this shape needs."""
        calculators.validate_dimensions(self.shape, self.dimensions)
        return self


class TankUpdate(BaseModel):
    """A partial update.

    Deliberately does NOT validate dimensions against shape: a PATCH may carry
    either field alone, so the pair is only complete once it is merged with the
    row already in the database. Validating here would accept switching a
    cylinder to shape "box" while leaving its diameter_cm behind. The handler
    validates the merged result instead.

    Unlike DeviceUpdate, every column behind these fields is NOT NULL, so there
    is nothing to clear: the handler reads this with model_dump(exclude_none=True),
    which makes an explicit null mean the same as omitting the field.
    """

    model_config = ConfigDict(extra="forbid")

    name: str | None = Field(default=None, min_length=1)
    shape: Shape | None = None
    height_cm: float | None = Field(default=None, gt=0)
    dimensions: dict | None = None


class TankLevel(BaseModel):
    """A tank's fill state, derived from its newest reading."""

    tank_id: int
    device_id: int
    distance_mm: int
    time: datetime
    water_height_mm: float
    percent: float
    volume_l: float
    capacity_l: float


class Reading(BaseModel):
    """One measurement of one device."""

    device_id: int
    distance_mm: int | None = None
    time: datetime


class LatestReading(BaseModel):
    """The legacy /latest shape, widened with the device's identity.

    device_id stays the *technical* node id string rather than devices.id: the
    Flutter app reads it as a String and predates the devices table. The
    internal id is exposed alongside it as `id`.
    """

    id: int | None = None
    device_id: str | None = None
    name: str | None = None
    distance_mm: int | None = None
    time: str | None = None
    error: str | None = None
