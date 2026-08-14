#pragma once

namespace dobby {

void registerPersonaCapeRepositoryPreinit();
void installPersonaCapeRepositoryHook();
bool personaCapeRepositoryHookInstalled();
bool personaCapeRepositoryAccepted();
bool setPersonaCapeRepositoryEnabled(bool enabled);

} // namespace dobby
