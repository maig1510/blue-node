"""MQTT -> TimescaleDB bridge for blue-node telemetry.

Subscribes to two topics of every node:

  - blue-node/+/telemetry, one row per message into the `telemetry` hypertable
  - blue-node/+/state, which keeps the `devices` row up to date (online flag,
    firmware version)

Only registered nodes are recorded. A device row is created by onboarding
(POST /devices on the api), never here; messages from any other node_id are
dropped - see devices.py.
"""

import json
import logging
import os
import time

import paho.mqtt.client as mqtt
import psycopg

import devices

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
log = logging.getLogger("ingest")

MQTT_HOST = os.getenv("MQTT_HOST", "mosquitto")
MQTT_PORT = int(os.getenv("MQTT_PORT", "1883"))
MQTT_TELEMETRY_TOPIC = os.getenv("MQTT_TELEMETRY_TOPIC", "blue-node/+/telemetry")
MQTT_STATE_TOPIC = os.getenv("MQTT_STATE_TOPIC", "blue-node/+/state")
PG_DSN = os.getenv("PG_DSN", "postgresql://blue:blue@timescaledb:5432/bluenode")

# Resolves the device and snapshots the tank it is assigned to in the same
# statement: tank_id is always the assignment at the moment the reading
# arrived (see the telemetry comment in init.sql), with no cache that a PATCH
# in the app could leave stale. An unregistered node_id selects nothing, so
# nothing is inserted and RETURNING comes back empty.
INSERT_SQL = """
    INSERT INTO telemetry (device_id, tank_id, distance_mm)
    SELECT id, tank_id, %s FROM devices WHERE node_id = %s
    RETURNING device_id
"""


def connect_pg():
    """Connect to Postgres, retrying until it answers.

    depends_on/healthcheck already orders startup, but the database can also go
    away later (a restart, a `down` of just that service); retrying here means
    the container never wedges in a crash loop waiting for it.
    """
    while True:
        try:
            conn = psycopg.connect(PG_DSN, autocommit=True)
            log.info("Connected to TimescaleDB")
            return conn
        except psycopg.OperationalError as exc:
            log.warning("Postgres not ready (%s), retrying in 3s", exc)
            time.sleep(3)


def on_connect(client, userdata, flags, reason_code, properties):
    if reason_code.is_failure:
        log.error("Broker refused connection: %s", reason_code)
        return
    # Subscribing here rather than after connect() so the subscriptions come
    # back on their own after a broker restart. The QoS matches what the
    # firmware publishes at (see the policy table in node_protocol.h): telemetry
    # is fire-and-forget, state is worth a redelivery.
    client.subscribe(MQTT_TELEMETRY_TOPIC, qos=0)
    client.subscribe(MQTT_STATE_TOPIC, qos=1)
    log.info("Connected to broker, subscribed to %s and %s",
             MQTT_TELEMETRY_TOPIC, MQTT_STATE_TOPIC)


def on_disconnect(client, userdata, flags, reason_code, properties):
    log.warning("Disconnected from broker: %s", reason_code)


def handle_telemetry(conn, node_id, payload):
    distance_mm = payload.get("distance_mm")

    with conn.cursor() as cur:
        cur.execute(INSERT_SQL, (distance_mm, node_id))
        row = cur.fetchone()

    if row is None:
        devices.note_unknown(node_id, "telemetry")
        return

    (device_id,) = row
    devices.touch_last_seen(conn, device_id)
    log.debug("%s distance_mm=%s", node_id, distance_mm)


def handle_state(conn, node_id, payload):
    # State messages are retained, so the broker replays the last one of every
    # node when this service connects - which is how is_online rebuilds itself
    # after an ingest restart. A node that died while ingest was down comes back
    # as a retained "online"; last_seen_at, not is_online, is the honest
    # freshness signal.
    devices.apply_state(
        conn,
        node_id,
        payload.get("status", ""),
        payload.get("fw"),
    )


def on_message(client, userdata, msg):
    # A bad payload must not take down the network thread, so nothing in here
    # is allowed to raise.
    try:
        # blue-node/<NODE_ID>/<category>
        _, node_id, category = msg.topic.split("/")
        payload = json.loads(msg.payload)

        if category == "telemetry":
            handle_telemetry(userdata["conn"], node_id, payload)
        elif category == "state":
            handle_state(userdata["conn"], node_id, payload)
        else:
            log.debug("Ignoring message on %s", msg.topic)
    except psycopg.Error as exc:
        log.error("Query failed (%s), reconnecting to Postgres", exc)
        # The ids behind the throttling state were handed out by the database
        # we just lost; a replacement database would not agree with them.
        devices.clear_cache()
        userdata["conn"] = connect_pg()
    except Exception as exc:
        log.error("Dropping message on %s: %s", msg.topic, exc)


def main():
    userdata = {"conn": connect_pg()}

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, userdata=userdata)
    client.on_connect = on_connect
    client.on_disconnect = on_disconnect
    client.on_message = on_message

    client.connect(MQTT_HOST, MQTT_PORT, keepalive=60)
    # loop_forever() handles reconnects itself, so there is no retry loop here.
    client.loop_forever()


if __name__ == "__main__":
    main()
