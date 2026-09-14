"""HTTP API over the blue-node database.

Mostly read-only - the ingest side owns the writes. The one exception is
PATCH /devices/{id}, which sets the two fields no machine can know: the
device's name and the tank it belongs to.

Error conventions differ on purpose:

  - GET /latest keeps its original behaviour of answering 200 with null fields
    even when the database is down, because the Flutter app renders the same
    "no data" state either way and has no error path of its own.
  - Every other endpoint uses real status codes: 404 for an unknown device,
    503 when Postgres is unreachable. Reporting a failed write as a success
    would be actively misleading.

Known gap: there is no authentication anywhere in this stack, so the PATCH is
as open as the reads. Acceptable for a LAN development setup, not beyond it.
"""

import logging

import psycopg
from fastapi import FastAPI, HTTPException
from fastapi.responses import JSONResponse

import db
from models import Device, DeviceUpdate, LatestReading, Reading

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
log = logging.getLogger("api")

DEVICE_COLUMNS = """
    id, node_id, mac_address::text AS mac_address, name, tank_id,
    firmware, is_online, created_at, last_seen_at
"""

# ORDER BY time DESC walks the (device_id, time DESC) index backwards; LIMIT 1
# stops after the first row, so this stays cheap as the hypertable grows.
LATEST_SQL = """
    SELECT d.id, d.node_id, d.name, t.distance_mm, t.time
    FROM telemetry t
    JOIN devices d ON d.id = t.device_id
    ORDER BY t.time DESC
    LIMIT 1
"""

DEVICE_LATEST_SQL = """
    SELECT device_id, distance_mm, time
    FROM telemetry
    WHERE device_id = %s
    ORDER BY time DESC
    LIMIT 1
"""

app = FastAPI(title="blue-node api")


@app.exception_handler(psycopg.OperationalError)
def on_db_unavailable(request, exc):
    """Turn a dead database into a 503 instead of a 500 traceback."""
    log.warning("Postgres unavailable: %s", exc)
    return JSONResponse(status_code=503, content={"detail": "database unavailable"})


def fetch_device(cur, device_id: int) -> dict:
    """Read one device or raise 404."""
    cur.execute(f"SELECT {DEVICE_COLUMNS} FROM devices WHERE id = %s", (device_id,))
    row = cur.fetchone()
    if row is None:
        raise HTTPException(status_code=404, detail=f"no device with id {device_id}")
    return row


@app.get("/latest", response_model=LatestReading)
def latest():
    """Newest reading from any node, with the identity of the node it came from."""
    try:
        with db.connect() as conn:
            with conn.cursor() as cur:
                cur.execute(LATEST_SQL)
                row = cur.fetchone()
    except psycopg.OperationalError as exc:
        # Deliberately not the 503 the other endpoints give - see module docstring.
        log.warning("Postgres unavailable: %s", exc)
        return LatestReading(error=str(exc))

    # An empty table is not an error - the app renders the same "no data" state
    # either way, so it gets nulls instead of a 404 to special-case.
    if row is None:
        return LatestReading()

    return LatestReading(
        id=row["id"],
        device_id=row["node_id"],
        name=row["name"],
        distance_mm=row["distance_mm"],
        time=row["time"].isoformat(),
    )


@app.get("/devices", response_model=list[Device])
def list_devices():
    """Every known device, oldest first."""
    with db.connect() as conn:
        with conn.cursor() as cur:
            cur.execute(f"SELECT {DEVICE_COLUMNS} FROM devices ORDER BY id")
            return cur.fetchall()


@app.get("/devices/{device_id}", response_model=Device)
def get_device(device_id: int):
    with db.connect() as conn:
        with conn.cursor() as cur:
            return fetch_device(cur, device_id)


@app.patch("/devices/{device_id}", response_model=Device)
def update_device(device_id: int, update: DeviceUpdate):
    """Set the device's name and/or the tank it belongs to.

    Only the fields actually present in the request body are written, so
    clearing a name (null) is distinguishable from leaving it untouched.
    """
    fields = update.model_dump(exclude_unset=True)

    with db.connect() as conn:
        with conn.cursor() as cur:
            # 404 before the UPDATE so an unknown id is reported as such rather
            # than silently affecting zero rows.
            device = fetch_device(cur, device_id)
            if not fields:
                return device

            # Keys come from DeviceUpdate's own field names, never from the
            # request body, so interpolating them into the SQL is safe; the
            # values stay parameterised.
            assignments = ", ".join(f"{name} = %s" for name in fields)
            cur.execute(
                f"UPDATE devices SET {assignments} WHERE id = %s RETURNING {DEVICE_COLUMNS}",
                (*fields.values(), device_id),
            )
            return cur.fetchone()


@app.get("/devices/{device_id}/latest", response_model=Reading)
def device_latest(device_id: int):
    """Newest reading of one device."""
    with db.connect() as conn:
        with conn.cursor() as cur:
            fetch_device(cur, device_id)

            cur.execute(DEVICE_LATEST_SQL, (device_id,))
            row = cur.fetchone()
            if row is None:
                raise HTTPException(
                    status_code=404, detail=f"device {device_id} has no readings yet"
                )
            return row
