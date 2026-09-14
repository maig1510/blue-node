"""The `devices` table as the ingest side sees it.

Auto-provisioning: a node that publishes for the first time gets a row here on
the spot, with nothing but its node_id filled in. Naming it and assigning it to
a tank happens later through the API - dropping its telemetry until someone
registers it would lose data for no benefit on a private broker.

Every function takes the connection as an argument rather than holding one:
main.py owns the connection and replaces it when Postgres goes away.
"""

import logging
import os
import time

log = logging.getLogger("ingest.devices")

# How often last_seen_at may be written per device. Telemetry arrives once a
# second, and an UPDATE per message would be almost entirely write amplification
# for a field nobody reads at that resolution.
LAST_SEEN_INTERVAL_S = float(os.getenv("LAST_SEEN_INTERVAL_S", "30"))

# ON CONFLICT DO UPDATE rather than DO NOTHING: DO NOTHING suppresses the
# RETURNING row on a conflict, and the id is the whole point of the call. The
# assignment is a no-op that exists only to make the row eligible for RETURNING.
RESOLVE_SQL = """
    INSERT INTO devices (node_id) VALUES (%s)
    ON CONFLICT (node_id) DO UPDATE SET node_id = EXCLUDED.node_id
    RETURNING id
"""

TOUCH_SQL = "UPDATE devices SET last_seen_at = now() WHERE id = %s"

# COALESCE so a payload without mac/fw (the broker-published offline LWT, or an
# older firmware) never blanks out what an earlier message already established.
STATE_SQL = """
    INSERT INTO devices (node_id, mac_address, firmware, is_online, last_seen_at)
    VALUES (%s, %s::macaddr, %s, %s, now())
    ON CONFLICT (node_id) DO UPDATE SET
        mac_address  = COALESCE(EXCLUDED.mac_address, devices.mac_address),
        firmware     = COALESCE(EXCLUDED.firmware, devices.firmware),
        is_online    = EXCLUDED.is_online,
        last_seen_at = EXCLUDED.last_seen_at
    RETURNING id
"""

# node_id -> devices.id. Ids never change for a given node_id, so this needs no
# expiry - only clear_cache() on reconnect, in case the database was replaced
# underneath us (a `down -v` re-runs init.sql and restarts the identity column).
_ids: dict[str, int] = {}

# devices.id -> monotonic timestamp of the last last_seen_at write.
_touched: dict[int, float] = {}


def clear_cache():
    """Forget every cached id. Call after reconnecting to Postgres."""
    _ids.clear()
    _touched.clear()


def resolve(conn, node_id: str) -> int:
    """Return devices.id for this node, creating the device if it is new."""
    cached = _ids.get(node_id)
    if cached is not None:
        return cached

    with conn.cursor() as cur:
        cur.execute(RESOLVE_SQL, (node_id,))
        (device_id,) = cur.fetchone()

    _ids[node_id] = device_id
    log.info("Device %s resolved to id %s", node_id, device_id)
    return device_id


def touch_last_seen(conn, device_id: int):
    """Bump last_seen_at, at most once per LAST_SEEN_INTERVAL_S per device."""
    now = time.monotonic()
    last = _touched.get(device_id)
    if last is not None and now - last < LAST_SEEN_INTERVAL_S:
        return

    with conn.cursor() as cur:
        cur.execute(TOUCH_SQL, (device_id,))
    _touched[device_id] = now


def apply_state(conn, node_id: str, status: str, mac: str | None, firmware: str | None):
    """Record a state message, creating the device if it is new.

    `status` is the payload's "status" field; anything other than "online"
    counts as offline, which covers the broker-published Last Will.
    """
    is_online = status == "online"

    with conn.cursor() as cur:
        cur.execute(STATE_SQL, (node_id, mac, firmware, is_online))
        (device_id,) = cur.fetchone()

    _ids[node_id] = device_id
    # The row was just written, so telemetry need not write it again right away.
    _touched[device_id] = time.monotonic()
    log.info("Device %s (id %s) is %s", node_id, device_id, status)
