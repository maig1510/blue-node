"""A prism with a triangular footprint, standing upright.

Note this is NOT a V-shaped trough lying on its side. The triangle is the
tank's floor plan, so it is the same at every depth and the tank fills
linearly, exactly like the cylinder and the box. A trough, where the triangle
stands in the vertical plane and the water surface widens as the tank fills,
is a different shape and would subclass TankCalculator directly.
"""

from pydantic import BaseModel, ConfigDict, Field

from .base import ConstantCrossSectionTank


class TriangularDimensions(BaseModel):
    """The triangular footprint.

    triangle_height_cm is the altitude of the triangle IN THE FLOOR PLAN -
    the perpendicular distance from base_cm to the opposite corner. It is not
    how tall the tank is; that is tanks.height_cm and it is a different number
    entirely. Two things called "height" on one tank is a trap, hence the name.
    """

    model_config = ConfigDict(extra="forbid")

    base_cm: float = Field(gt=0)
    triangle_height_cm: float = Field(gt=0)


class TriangularTank(ConstantCrossSectionTank):
    shape = "triangular"
    dimensions_model = TriangularDimensions

    def cross_section_cm2(self) -> float:
        return self.dimensions.base_cm * self.dimensions.triangle_height_cm / 2
