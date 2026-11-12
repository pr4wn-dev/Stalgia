#include "effects/SlowMyco.h"

#include "effects/MycoMaskData.h"

SlowMyco::SlowMyco()
    : RainReveal("myco", "Slow Myco", CharacterMask{kMycoMask, kMycoRgb, kMycoMaskW, kMycoMaskH}) {}

RainReveal::RevealKnobs SlowMyco::knobsFrom(const AppSettings& settings) const {
    const MycoSettings& s = settings.myco;
    RevealKnobs k;
    k.color = s.color;
    k.bgDim = s.bgDim;
    k.brightness = s.brightness;
    k.size = s.size;
    k.posX = s.posX;
    k.speed = s.speed;
    k.glyphSize = s.glyphSize;
    k.eyeGlow = s.eyeGlow;
    k.breathing = s.breathing;
    k.colorize = s.colorize;
    k.reveal = s.reveal;
    k.detail = s.detail;
    k.breatheAmt = s.breatheAmt;
    k.imagePath = s.imagePath;
    return k;
}
