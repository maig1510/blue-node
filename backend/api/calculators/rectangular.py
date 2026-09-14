"""A rectangular tank - a box standing upright."""

from pydantic import BaseModel, ConfigDict, Field

from .base import ConstantCrossSectionTank


class RectangularDimensions(BaseModel):
    """The footprint, measured on the floor."""

    model_config = ConfigDict(extra="forbid")

    width_cm: float = Field(gt=0)
    depth_cm: float = Field(gt=0)


class RectangularTank(ConstantCrossSectionTank):
    shape = "rectangular"
    dimensions_model = RectangularDimensions

    def cross_section_cm2(self) -> float:
        return self.dimensions.width_cm * self.dimensions.depth_cm
