"""Turning a distance reading into a water level.

The sensor looks down at the water from above, so what it measures is the empty
space, not the water:

        ---  sensor face
         |   sensor_offset_mm     (how far the sensor sits above the rim)
        ===  tank rim  ---
         |                |
         |   distance_mm  |  height_cm * 10
        ~~~  water        |
         |                |
        ___  tank floor  ---

    water_height_mm = height_mm - (distance_mm - sensor_offset_mm)

This module is the ONLY place that knows which shapes exist, how their volume
is computed, and - just as importantly - that tanks are measured in centimetres
while telemetry arrives in millimetres. Everything below works in millimetres
and converts at the edges; nothing outside this file may multiply by ten.

Adding a shape means adding one entry to SHAPES and nothing else, as long as
the shape has a constant cross-section. A shape whose cross-section varies with
depth (a horizontal cylinder, a cone) does not fit `area x height` and needs
compute() to gain a per-shape volume function instead.
"""

import math
from typing import Literal

from pydantic import BaseModel, ConfigDict, Field


class CylinderDimensions(BaseModel):
    """An upright cylinder, measured across the top."""

    model_config = ConfigDict(extra="forbid")

    diameter_cm: float = Field(gt=0)


class BoxDimensions(BaseModel):
    """A rectangular tank, measured on the floor."""

    model_config = ConfigDict(extra="forbid")

    width_cm: float = Field(gt=0)
    depth_cm: float = Field(gt=0)


def _cylinder_area_cm2(dims: CylinderDimensions) -> float:
    radius = dims.diameter_cm / 2
    return math.pi * radius * radius


def _box_area_cm2(dims: BoxDimensions) -> float:
    return dims.width_cm * dims.depth_cm


# shape name -> (dimensions model, cross-section function)
SHAPES = {
    "cylinder": (CylinderDimensions, _cylinder_area_cm2),
    "box": (BoxDimensions, _box_area_cm2),
}

# Spelled out rather than derived from SHAPES so that type checkers and the
# generated OpenAPI schema see the actual values. A new shape has to be named
# in three places: SHAPES above, here, and the tanks_shape_dimensions CHECK in
# infra/timescale/init.sql.
Shape = Literal["cylinder", "box"]


def validate_dimensions(shape: str, dimensions: dict) -> BaseModel:
    """Parse dimensions with the model belonging to `shape`.

    Raises pydantic.ValidationError, which FastAPI renders as a 422 when this
    runs inside a request.
    """
    model, _ = SHAPES[shape]
    return model.model_validate(dimensions)


def cross_section_cm2(shape: str, dimensions: dict) -> float:
    """Horizontal area of the tank, in cm^2."""
    model, area = SHAPES[shape]
    return area(model.model_validate(dimensions))


def compute(
    height_cm: float,
    shape: str,
    dimensions: dict,
    distance_mm: int,
    sensor_offset_mm: int,
) -> dict:
    """Derive water height, fill percentage and volume from one reading.

    The water height is clamped to [0, tank height]. The raw formula routinely
    falls outside that range in normal use: an empty tank reads further than
    its own height, and the A01NYUB has a ~28 cm dead zone in which a nearly
    full tank reads nonsense. A negative level or 112 % is a worse answer than
    a clamped one, and callers still get distance_mm back to judge for
    themselves.
    """
    height_mm = height_cm * 10
    water_height_mm = height_mm - (distance_mm - sensor_offset_mm)
    water_height_mm = max(0.0, min(float(height_mm), water_height_mm))

    # 1 litre is 1000 cm^3, and area is already cm^2, so the height has to come
    # back to centimetres before multiplying.
    area_cm2 = cross_section_cm2(shape, dimensions)
    volume_l = area_cm2 * (water_height_mm / 10) / 1000
    capacity_l = area_cm2 * height_cm / 1000

    return {
        "water_height_mm": round(water_height_mm, 1),
        "percent": round(water_height_mm / height_mm * 100, 1),
        "volume_l": round(volume_l, 1),
        "capacity_l": round(capacity_l, 1),
    }
