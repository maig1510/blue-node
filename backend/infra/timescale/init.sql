-- Runs once, on the first boot of an empty timescale_data volume.
-- Wipe the volume (docker compose down -v) to re-run it after edits.

CREATE EXTENSION IF NOT EXISTS timescaledb;

-- Bumps updated_at on every UPDATE, so the column holds even for a write that
-- bypasses the API (pgAdmin, psql).
CREATE FUNCTION touch_updated_at() RETURNS TRIGGER AS $$
BEGIN
    NEW.updated_at = now();
    RETURN NEW;
END;
$$ LANGUAGE plpgsql;

-- A physical tank. Its shape and dimensions are what turn a distance reading
-- into a volume; dimensions holds the per-shape measurements as JSON so that
-- supporting another shape does not mean another column.
--
-- Units: everything here is centimetres, while telemetry is millimetres. The
-- conversion happens in exactly one place, api/tank_level.py - hence the unit
-- suffix on every name.
--
-- Created before devices, which references it.
CREATE TABLE tanks (
    id         BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    name       TEXT         NOT NULL,
    shape      TEXT         NOT NULL,
    height_cm  NUMERIC(8,1) NOT NULL CHECK (height_cm > 0),
    dimensions JSONB        NOT NULL,
    created_at TIMESTAMPTZ  NOT NULL DEFAULT now(),
    updated_at TIMESTAMPTZ  NOT NULL DEFAULT now(),

    -- The API validates dimensions against the shape with Pydantic, but
    -- pgAdmin goes straight to the table. This is the backstop that keeps a
    -- box from carrying a cylinder's measurements.
    CONSTRAINT tanks_shape_dimensions CHECK (
        (shape = 'cylinder' AND dimensions ? 'diameter_cm') OR
        (shape = 'box'      AND dimensions ?& array['width_cm', 'depth_cm'])
    )
);

CREATE TRIGGER tanks_touch_updated_at
    BEFORE UPDATE ON tanks
    FOR EACH ROW EXECUTE FUNCTION touch_updated_at();

-- One physical node. Rows are created by the ingest service the first time a
-- node publishes (auto-provisioning), so a new device only has to be powered
-- on; name, tank_id and sensor_offset_mm are filled in afterwards through the
-- API.
--
-- node_id is the technical identifier the firmware puts in the topic: the low
-- three bytes of the WiFi MAC as uppercase hex (see node_protocol.c). The full
-- MAC only arrives on the state topic, so mac_address stays NULL until the
-- node announces itself once. It is UNIQUE because it is the key a QR-code
-- onboarding flow will look a device up by.
CREATE TABLE devices (
    id           BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    node_id      TEXT        NOT NULL UNIQUE,
    mac_address  MACADDR     UNIQUE,
    name         TEXT,

    -- ON DELETE SET NULL: deleting a tank should not be blocked by a node
    -- being assigned to it. The node outlives the tank and simply becomes
    -- unassigned, and its telemetry hangs off the device rather than the tank,
    -- so no measurement is lost either way.
    tank_id      BIGINT REFERENCES tanks(id) ON DELETE SET NULL,

    -- How far the sensor face sits above the top rim of the tank. 0 means
    -- flush with the rim. Without it a distance reading cannot be turned into
    -- a water height - see api/tank_level.py.
    sensor_offset_mm INTEGER NOT NULL DEFAULT 0,

    firmware     TEXT,
    is_online    BOOLEAN     NOT NULL DEFAULT FALSE,
    created_at   TIMESTAMPTZ NOT NULL DEFAULT now(),
    last_seen_at TIMESTAMPTZ
);

-- One column per telemetry field. The firmware sends only distance_mm
-- (see node_protocol.h for the topic layout); add a column here and to the
-- INSERT in ingest/main.py as the payload grows.
--
-- "time" is broker-arrival time: the node sends no timestamp of its own.
--
-- device_id is the internal devices.id, not the technical node_id: the text
-- identifier lives in exactly one place. A foreign key FROM a hypertable TO a
-- plain table is supported by TimescaleDB (the reverse direction is not, which
-- is why devices must never reference telemetry). It also means a device
-- cannot be deleted while its measurements exist - which is what we want.
CREATE TABLE telemetry (
    time        TIMESTAMPTZ NOT NULL DEFAULT now(),
    device_id   BIGINT      NOT NULL REFERENCES devices(id),
    distance_mm INTEGER
);


SELECT create_hypertable('telemetry', by_range('time'));

CREATE INDEX ON telemetry (device_id, time DESC);
