#pragma once

namespace dobby {

void installPersonaOwnershipHook();
bool personaOwnershipHookInstalled();
bool personaOwnershipFeatureCaptured();
bool setPersonaOwnershipBypass(bool enabled);

} // namespace dobby
