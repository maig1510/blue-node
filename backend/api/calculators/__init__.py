"""Per-shape tank volume calculators.

The rest of the API talks to this module, never to the shape files directly:

    calc = calculators.get("cylinder", height_cm=200, dimensions={"diameter_cm": 150})
    calc.level(distance_mm=307, sensor_offset_mm=100)

Adding a shape means writing one module beside these, importing it here, and
adding it to CALCULATORS and Shape. The only other place a shape name appears
is the tanks_shape_dimensions CHECK in infra/timescale/init.sql.
"""

from typing import Literal

from pydantic import BaseModel

from .base import ConstantCrossSectionTank, TankCalculator
from .cylinder import CylinderTank
from .rectangular import RectangularTank
from .triangular import TriangularTank

__all__ = [
    "CALCULATORS",
    "ConstantCrossSectionTank",
    "Shape",
    "TankCalculator",
    "get",
    "validate_dimensions",
]

CALCULATORS: dict[str, type[TankCalculator]] = {
    calculator.shape: calculator
    for calculator in (CylinderTank, RectangularTank, TriangularTank)
}

# Spelled out rather than derived from CALCULATORS so that type checkers and
# the generated OpenAPI schema see the actual values.
Shape = Literal["cylinder", "rectangular", "triangular"]


def get(shape: str, height_cm: float, dimensions: dict) -> TankCalculator:
    """Build the calculator for one tank.

    Raises pydantic.ValidationError if the dimensions do not match the shape.
    """
    return CALCULATORS[shape](height_cm, dimensions)


def validate_dimensions(shape: str, dimensions: dict) -> BaseModel:
    """Parse dimensions with the model belonging to `shape`.

    Raises pydantic.ValidationError, which FastAPI renders as a 422 when this
    runs inside a request.
    """
    return CALCULATORS[shape].dimensions_model.model_validate(dimensions)
