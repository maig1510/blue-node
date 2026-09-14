"""An upright cylinder - the common round water tank."""

import math

from pydantic import BaseModel, ConfigDict, Field

from .base import ConstantCrossSectionTank


class CylinderDimensions(BaseModel):
    """Measured across the top, inside wall to inside wall."""

    model_config = ConfigDict(extra="forbid")

    diameter_cm: float = Field(gt=0)


class CylinderTank(ConstantCrossSectionTank):
    shape = "cylinder"
    dimensions_model = CylinderDimensions

    def cross_section_cm2(self) -> float:
        radius = self.dimensions.diameter_cm / 2
        return math.pi * radius * radius
