"""Read-only HTTP API over the blue-node telemetry hypertable.

The ingest side is write-only; this is the one way data comes back out
(pgAdmin aside). Currently a single endpoint, GET /latest, used by the
Flutter test app to show the newest reading.
"""

import logging
import os

import psycopg
from fastapi import FastAPI

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
log = logging.getLogger("api")

PG_DSN = os.getenv("PG_DSN", "postgresql://blue:blue@timescaledb:5432/bluenode")

# ORDER BY time DESC walks the (device_id, time DESC) index backwards; LIMIT 1
# stops after the first row, so this stays cheap as the hypertable grows.
LATEST_SQL = """
    SELECT device_id, distance_mm, time
    FROM telemetry
    ORDER BY time DESC
    LIMIT 1
"""

app = FastAPI(title="blue-node api")


@app.get("/latest")
def latest():
    """Newest reading from any node.

    Connects per request rather than holding a pooled connection: the traffic
    is one poll every couple of seconds, and this way a TimescaleDB restart
    can't leave the API wedged on a dead socket.
    """
    try:
        with psycopg.connect(PG_DSN, connect_timeout=5) as conn:
            with conn.cursor() as cur:
                cur.execute(LATEST_SQL)
                row = cur.fetchone()
    except psycopg.OperationalError as exc:
        log.warning("Postgres unavailable: %s", exc)
        return {"device_id": None, "distance_mm": None, "time": None, "error": str(exc)}

    # An empty table is not an error - the app renders the same "no data" state
    # either way, so it gets nulls instead of a 404 to special-case.
    if row is None:
        return {"device_id": None, "distance_mm": None, "time": None}

    device_id, distance_mm, time = row
    return {
        "device_id": device_id,
        "distance_mm": distance_mm,
        "time": time.isoformat(),
    }
