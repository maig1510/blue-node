-- Runs once, on the first boot of an empty timescale_data volume.
-- Wipe the volume (docker compose down -v) to re-run it after edits.

CREATE EXTENSION IF NOT EXISTS timescaledb;

-- One physical node. Rows are created by the ingest service the first time a
-- node publishes (auto-provisioning), so a new device only has to be powered
-- on; name and tank_id are filled in afterwards through the API.
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
    tank_id      BIGINT,
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
