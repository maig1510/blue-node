"""Request and response schemas."""

from datetime import datetime

from pydantic import BaseModel, ConfigDict


class Device(BaseModel):
    """A registered node. Rows are created by the ingest service."""

    id: int
    node_id: str
    mac_address: str | None = None
    name: str | None = None
    tank_id: int | None = None
    firmware: str | None = None
    is_online: bool
    created_at: datetime
    last_seen_at: datetime | None = None


class DeviceUpdate(BaseModel):
    """The fields a human owns. Everything else is written by ingest.

    Both fields are nullable *and* optional, and the two mean different things:
    omitting a field leaves it alone, sending null clears it. The handler tells
    them apart with model_dump(exclude_unset=True), so the defaults here are
    never actually used as values.
    """

    model_config = ConfigDict(extra="forbid")

    name: str | None = None
    tank_id: int | None = None


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
