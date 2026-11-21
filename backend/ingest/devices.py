"""The `devices` table as the ingest side sees it.

No auto-provisioning: a device row is created only by onboarding (the app
registers the node's MAC through POST /devices before the node learns its
broker address). Anything published under a node_id without a row - a node
that skipped onboarding, a stale simulator, a retained message left over from
an older schema - is dropped here, and logged once per UNKNOWN_LOG_INTERVAL_S
so a node publishing every second cannot flood the log.

There is no node_id -> devices.id cache: telemetry resolves the device inside
its own INSERT (see main.py), so a lookup costs nothing extra, and there is
nothing to go stale when a device is reassigned to another tank or, later,
removed.

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

# How often a message from the same unregistered node may be logged.
UNKNOWN_LOG_INTERVAL_S = float(os.getenv("UNKNOWN_LOG_INTERVAL_S", "60"))

TOUCH_SQL = "UPDATE devices SET last_seen_at = now() WHERE id = %s"

# UPDATE, never INSERT: a state message must not bring an unregistered node
# into existence. COALESCE so a payload without fw (the broker-published
# offline LWT, or an older firmware) never blanks out what an earlier message
# already established. The MAC is not written here at all - registration put
# it there, and node_id is derived from it.
STATE_SQL = """
    UPDATE devices SET
        firmware     = COALESCE(%s, firmware),
        is_online    = %s,
        last_seen_at = now()
    WHERE node_id = %s
    RETURNING id
"""

# devices.id -> monotonic timestamp of the last last_seen_at write.
_touched: dict[int, float] = {}

# node_id -> monotonic timestamp of the last "unknown node" log line.
_unknown_logged: dict[str, float] = {}


def clear_cache():
    """Forget the throttling state. Call after reconnecting to Postgres.

    A replacement database (a `down -v` re-runs init.sql and restarts the
    identity column) would hand out the same ids for different devices.
    """
    _touched.clear()
    _unknown_logged.clear()


def note_unknown(node_id: str, category: str):
    """Log that a message from an unregistered node was dropped, rate-limited."""
    now = time.monotonic()
    last = _unknown_logged.get(node_id)
    if last is not None and now - last < UNKNOWN_LOG_INTERVAL_S:
        return
    _unknown_logged[node_id] = now
    log.warning("Dropping %s from unregistered node %s (not onboarded)", category, node_id)


def touch_last_seen(conn, device_id: int):
    """Bump last_seen_at, at most once per LAST_SEEN_INTERVAL_S per device."""
    now = time.monotonic()
    last = _touched.get(device_id)
    if last is not None and now - last < LAST_SEEN_INTERVAL_S:
        return

    with conn.cursor() as cur:
        cur.execute(TOUCH_SQL, (device_id,))
    _touched[device_id] = now


def apply_state(conn, node_id: str, status: str, firmware: str | None) -> int | None:
    """Record a state message of a registered node.

    `status` is the payload's "status" field; anything other than "online"
    counts as offline, which covers the broker-published Last Will.

    Returns devices.id, or None when the node is not registered (the message
    is then dropped).
    """
    is_online = status == "online"

    with conn.cursor() as cur:
        cur.execute(STATE_SQL, (firmware, is_online, node_id))
        row = cur.fetchone()

    if row is None:
        note_unknown(node_id, "state")
        return None

    (device_id,) = row
    # The row was just written, so telemetry need not write it again right away.
    _touched[device_id] = time.monotonic()
    log.info("Device %s (id %s) is %s", node_id, device_id, status)
    return device_id
