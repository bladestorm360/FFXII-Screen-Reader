#pragma once

namespace TestRuntime {
void PauseNextLookup();
bool LookupStarted();
void ReleaseLookup();
int LookupCount();
}
