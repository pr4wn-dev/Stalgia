#pragma once

#include "effects/RainReveal.h"

class CodeApe final : public RainReveal {
public:
    CodeApe();

protected:
    RevealKnobs knobsFrom(const AppSettings& settings) const override;
};
