"""What every tank shape has to be able to work out.

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

This package is the ONLY place that knows how a shape's volume is computed and
- just as importantly - that tanks are measured in centimetres while telemetry
arrives in millimetres. Everything here works in millimetres and converts at the
edges; nothing outside this package may multiply by ten.

The split into two base classes is not decoration. Every shape supported today
has a constant cross-section, so its volume is area x water height and
ConstantCrossSectionTank writes that once. A shape whose cross-section varies
with depth - a horizontal cylinder, a V-shaped trough - does not fit that
formula at all: its surface widens as it fills. Such a shape subclasses
TankCalculator directly and brings its own current_volume_l(), without any of
the shapes below having to change.
"""

from abc import ABC, abstractmethod
from typing import ClassVar

from pydantic import BaseModel


class TankCalculator(ABC):
    """Base for the per-shape maths.

    Subclasses declare `shape` (the value stored in tanks.shape) and
    `dimensions_model` (the Pydantic model describing tanks.dimensions for that
    shape), and implement the two volume methods.
    """

    shape: ClassVar[str]
    dimensions_model: ClassVar[type[BaseModel]]

    def __init__(self, height_cm: float, dimensions: dict):
        self.height_cm = float(height_cm)
        self.height_mm = self.height_cm * 10
        # Parsed, not just validated: subclasses read the measurements off a
        # typed object rather than digging through a dict.
        self.dimensions = self.dimensions_model.model_validate(dimensions)

    @abstractmethod
    def total_volume_l(self) -> float:
        """Capacity of the tank when full, in litres."""

    @abstractmethod
    def current_volume_l(self, water_height_mm: float) -> float:
        """Volume of water standing `water_height_mm` deep, in litres."""

    def water_height_mm(self, distance_mm: int, sensor_offset_mm: int) -> float:
        """Depth of the water, clamped to [0, tank height].

        The raw formula routinely falls outside that range in normal use: an
        empty tank reads further than its own height, and the A01NYUB has a
        ~28 cm dead zone in which a nearly full tank reads nonsense. A negative
        level or 112 % is a worse answer than a clamped one, and callers still
        get distance_mm back to judge for themselves.
        """
        water = self.height_mm - (distance_mm - sensor_offset_mm)
        return max(0.0, min(self.height_mm, water))

    def fill_percentage(self, water_height_mm: float) -> float:
        """How full the tank is, 0-100."""
        return water_height_mm / self.height_mm * 100

    def level(self, distance_mm: int, sensor_offset_mm: int) -> dict:
        """One reading turned into the fill state the API reports."""
        water_height_mm = self.water_height_mm(distance_mm, sensor_offset_mm)
        return {
            "water_height_mm": round(water_height_mm, 1),
            "percent": round(self.fill_percentage(water_height_mm), 1),
            "volume_l": round(self.current_volume_l(water_height_mm), 1),
            "capacity_l": round(self.total_volume_l(), 1),
        }


class ConstantCrossSectionTank(TankCalculator):
    """A tank whose horizontal area is the same at every depth.

    Volume is then simply area x depth, so a subclass only has to say how big
    that area is.
    """

    @abstractmethod
    def cross_section_cm2(self) -> float:
        """Horizontal area of the tank, in cm^2."""

    def total_volume_l(self) -> float:
        return self._volume_l(self.height_mm)

    def current_volume_l(self, water_height_mm: float) -> float:
        return self._volume_l(water_height_mm)

    def _volume_l(self, depth_mm: float) -> float:
        # A litre is 1000 cm^3 and the area is already cm^2, so the depth has
        # to come back to centimetres before multiplying.
        return self.cross_section_cm2() * (depth_mm / 10) / 1000
