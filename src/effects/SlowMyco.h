#pragma once

#include "effects/RainReveal.h"

class SlowMyco final : public RainReveal {
public:
    SlowMyco();

protected:
    RevealKnobs knobsFrom(const AppSettings& settings) const override;
};
