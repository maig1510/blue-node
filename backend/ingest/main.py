"""MQTT -> TimescaleDB bridge for blue-node telemetry.

Subscribes to the telemetry topic of every node (blue-node/+/telemetry) and
writes one row per message into the `telemetry` hypertable.
"""

import json
import logging
import os
import time

import paho.mqtt.client as mqtt
import psycopg

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
log = logging.getLogger("ingest")

MQTT_HOST = os.getenv("MQTT_HOST", "mosquitto")
MQTT_PORT = int(os.getenv("MQTT_PORT", "1883"))
MQTT_TOPIC = os.getenv("MQTT_TOPIC", "blue-node/+/telemetry")
PG_DSN = os.getenv("PG_DSN", "postgresql://blue:blue@timescaledb:5432/bluenode")

INSERT_SQL = "INSERT INTO telemetry (device_id, distance_mm) VALUES (%s, %s)"


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
    # Subscribing here rather than after connect() so the subscription comes
    # back on its own after a broker restart.
    client.subscribe(MQTT_TOPIC, qos=0)
    log.info("Connected to broker, subscribed to %s", MQTT_TOPIC)


def on_disconnect(client, userdata, flags, reason_code, properties):
    log.warning("Disconnected from broker: %s", reason_code)


def on_message(client, userdata, msg):
    # A bad payload must not take down the network thread, so nothing in here
    # is allowed to raise.
    try:
        # blue-node/<DEVICE_ID>/telemetry
        device_id = msg.topic.split("/")[1]
        payload = json.loads(msg.payload)
        distance_mm = payload.get("distance_mm")

        with userdata["conn"].cursor() as cur:
            cur.execute(INSERT_SQL, (device_id, distance_mm))
        log.debug("%s distance_mm=%s", device_id, distance_mm)
    except psycopg.Error as exc:
        log.error("Insert failed (%s), reconnecting to Postgres", exc)
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
