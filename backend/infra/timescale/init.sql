-- Runs once, on the first boot of an empty timescale_data volume.
-- Wipe the volume (docker compose down -v) to re-run it after edits.

CREATE EXTENSION IF NOT EXISTS timescaledb;

-- One column per telemetry field. The firmware sends only distance_mm
-- (see node_protocol.h for the topic layout); add a column here and to the
-- INSERT in ingest/main.py as the payload grows.
--
-- "time" is broker-arrival time: the node sends no timestamp of its own.
CREATE TABLE telemetry (
    time        TIMESTAMPTZ NOT NULL DEFAULT now(),
    device_id   TEXT        NOT NULL,
    distance_mm INTEGER
);


SELECT create_hypertable('telemetry', by_range('time'));

CREATE INDEX ON telemetry (device_id, time DESC);
