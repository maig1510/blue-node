"""Postgres access for the API.

One connection per request rather than a pool: the traffic is one poll every
couple of seconds, and this way a TimescaleDB restart cannot leave the API
wedged on a dead socket.
"""

import os

import psycopg
from psycopg.rows import dict_row

PG_DSN = os.getenv("PG_DSN", "postgresql://blue:blue@timescaledb:5432/bluenode")


def connect():
    """Open a connection as a context manager. Raises psycopg.OperationalError."""
    return psycopg.connect(PG_DSN, connect_timeout=5, row_factory=dict_row)
