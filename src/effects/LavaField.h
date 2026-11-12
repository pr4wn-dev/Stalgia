#pragma once

#include <string>

// CPU twin of the lava height-field normal. Fails if the surface is a cone,
// if the analytic gradient disagrees with the field, or if the shade steps.
bool lavaSurfaceContinuity(std::string& why);
