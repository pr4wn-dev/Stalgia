#pragma once

class EffectRegistry;

// Empty in the public build. A directory passed as STALGIA_PRIVATE_EFFECTS_DIR
// supplies its own definition and is compiled instead of the stub.
void registerPrivateEffects(EffectRegistry& registry);
