#include "effects/CodeApe.h"

#include "effects/ApeMaskData.h"

CodeApe::CodeApe()
    : RainReveal("ape", "Code Ape", CharacterMask{kApeMask, nullptr, kApeMaskW, kApeMaskH}) {}

RainReveal::RevealKnobs CodeApe::knobsFrom(const AppSettings& settings) const {
    const ApeSettings& s = settings.ape;
    RevealKnobs k;
    k.color = s.color;
    k.bgDim = s.bgDim;
    k.brightness = s.apeBrightness;
    k.size = s.size;
    k.posX = s.posX;
    k.speed = s.speed;
    k.glyphSize = s.glyphSize;
    k.eyeGlow = s.eyeGlow;
    k.breathing = s.breathing;
    k.colorize = s.fromImage;
    k.reveal = s.reveal;
    k.detail = s.detail;
    k.breatheAmt = s.breatheAmt;
    k.imagePath = s.imagePath;
    return k;
}
