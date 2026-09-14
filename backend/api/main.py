"""HTTP API over the blue-node database.

Telemetry is read-only here - the ingest side owns those writes. What this API
owns is everything no machine can know by itself: a device's name, which tank
it watches, how high above the rim it is mounted, and the tanks themselves.

Error conventions differ on purpose:

  - GET /latest keeps its original behaviour of answering 200 with null fields
    even when the database is down, because the Flutter app renders the same
    "no data" state either way and has no error path of its own.
  - Every other endpoint uses real status codes: 404 for an unknown device or
    tank, 422 for measurements that do not match the shape they claim, 503 when
    Postgres is unreachable. Reporting a failed write as a success would be
    actively misleading.

Known gap: there is no authentication anywhere in this stack, so the writes are
as open as the reads. Acceptable for a LAN development setup, not beyond it.
"""

import logging

import psycopg
from fastapi import FastAPI, HTTPException, Response
from fastapi.responses import JSONResponse
from psycopg.types.json import Jsonb
from pydantic import ValidationError

import db
import tank_level
from models import (
    Device,
    DeviceUpdate,
    LatestReading,
    Reading,
    Tank,
    TankCreate,
    TankLevel,
    TankUpdate,
)

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
log = logging.getLogger("api")

DEVICE_COLUMNS = """
    id, node_id, mac_address::text AS mac_address, name, tank_id,
    sensor_offset_mm, firmware, is_online, created_at, last_seen_at
"""

TANK_COLUMNS = "id, name, shape, height_cm, dimensions, created_at, updated_at"

# Newest reading across every device assigned to the tank. Usually there is
# exactly one; taking the newest means a second sensor on the same tank needs
# no special case.
TANK_LEVEL_SQL = """
    SELECT d.id AS device_id, d.sensor_offset_mm, t.distance_mm, t.time
    FROM devices d
    JOIN telemetry t ON t.device_id = d.id
    WHERE d.tank_id = %s
    ORDER BY t.time DESC
    LIMIT 1
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


def fetch_tank(cur, tank_id: int) -> dict:
    """Read one tank or raise 404."""
    cur.execute(f"SELECT {TANK_COLUMNS} FROM tanks WHERE id = %s", (tank_id,))
    row = cur.fetchone()
    if row is None:
        raise HTTPException(status_code=404, detail=f"no tank with id {tank_id}")
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


@app.post("/tanks", response_model=Tank, status_code=201)
def create_tank(tank: TankCreate):
    """Register a tank. TankCreate has already checked dimensions against shape."""
    with db.connect() as conn:
        with conn.cursor() as cur:
            cur.execute(
                f"""INSERT INTO tanks (name, shape, height_cm, dimensions)
                    VALUES (%s, %s, %s, %s) RETURNING {TANK_COLUMNS}""",
                # Jsonb(): psycopg has no default dumper for a bare dict.
                (tank.name, tank.shape, tank.height_cm, Jsonb(tank.dimensions)),
            )
            return cur.fetchone()


@app.get("/tanks", response_model=list[Tank])
def list_tanks():
    """Every tank, oldest first."""
    with db.connect() as conn:
        with conn.cursor() as cur:
            cur.execute(f"SELECT {TANK_COLUMNS} FROM tanks ORDER BY id")
            return cur.fetchall()


@app.get("/tanks/{tank_id}", response_model=Tank)
def get_tank(tank_id: int):
    with db.connect() as conn:
        with conn.cursor() as cur:
            return fetch_tank(cur, tank_id)


@app.patch("/tanks/{tank_id}", response_model=Tank)
def update_tank(tank_id: int, update: TankUpdate):
    """Change a tank, validating shape and dimensions as a pair.

    A request may carry shape alone, dimensions alone, or both, so the pair is
    only complete once merged with the stored row - which is why TankUpdate
    itself does not validate it. Without this, switching a cylinder to "box"
    while leaving its diameter_cm behind would pass the model and be caught
    only by the CHECK constraint, as a 500.

    exclude_none rather than the exclude_unset used for devices: every tank
    column here is NOT NULL, so there is no "clear this field" to express and
    an explicit null is best read as "leave it alone".
    """
    fields = update.model_dump(exclude_none=True)

    with db.connect() as conn:
        with conn.cursor() as cur:
            # 404 before the UPDATE so an unknown id is reported as such rather
            # than silently affecting zero rows.
            tank = fetch_tank(cur, tank_id)
            if not fields:
                return tank

            shape = fields.get("shape", tank["shape"])
            dimensions = fields.get("dimensions", tank["dimensions"])
            try:
                tank_level.validate_dimensions(shape, dimensions)
            except ValidationError as exc:
                raise HTTPException(status_code=422, detail=exc.errors()) from exc

            if "dimensions" in fields:
                fields["dimensions"] = Jsonb(fields["dimensions"])

            # Keys come from TankUpdate's own field names, never from the
            # request body, so interpolating them into the SQL is safe; the
            # values stay parameterised.
            assignments = ", ".join(f"{name} = %s" for name in fields)
            cur.execute(
                f"UPDATE tanks SET {assignments} WHERE id = %s RETURNING {TANK_COLUMNS}",
                (*fields.values(), tank_id),
            )
            return cur.fetchone()


@app.delete("/tanks/{tank_id}", status_code=204)
def delete_tank(tank_id: int):
    """Delete a tank. Devices assigned to it survive, unassigned."""
    with db.connect() as conn:
        with conn.cursor() as cur:
            fetch_tank(cur, tank_id)
            cur.execute("DELETE FROM tanks WHERE id = %s", (tank_id,))
    return Response(status_code=204)


@app.get("/tanks/{tank_id}/level", response_model=TankLevel)
def tank_level_now(tank_id: int):
    """How full the tank is, derived from its newest reading."""
    with db.connect() as conn:
        with conn.cursor() as cur:
            tank = fetch_tank(cur, tank_id)

            cur.execute(TANK_LEVEL_SQL, (tank_id,))
            row = cur.fetchone()
            if row is None:
                # Two different situations, worth telling apart: nothing is
                # watching this tank, or something is but has not reported yet.
                cur.execute("SELECT 1 FROM devices WHERE tank_id = %s LIMIT 1", (tank_id,))
                detail = (
                    f"tank {tank_id} has no readings yet"
                    if cur.fetchone()
                    else f"tank {tank_id} has no device assigned"
                )
                raise HTTPException(status_code=404, detail=detail)

            if row["distance_mm"] is None:
                raise HTTPException(
                    status_code=404, detail=f"tank {tank_id} has no distance reading yet"
                )

            level = tank_level.compute(
                height_cm=float(tank["height_cm"]),
                shape=tank["shape"],
                dimensions=tank["dimensions"],
                distance_mm=row["distance_mm"],
                sensor_offset_mm=row["sensor_offset_mm"],
            )
            return TankLevel(
                tank_id=tank_id,
                device_id=row["device_id"],
                distance_mm=row["distance_mm"],
                time=row["time"],
                **level,
            )
