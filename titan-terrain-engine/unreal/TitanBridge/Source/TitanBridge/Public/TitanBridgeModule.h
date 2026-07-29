#pragma once

#include "Modules/ModuleManager.h"

class FTitanBridgeModule : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;

    // False when the linked libTitanCore's C API version does not match the
    // headers this module was built against — i.e. the prebuilt library in
    // ThirdParty is stale. Generation is refused in that state rather than
    // producing terrain from a mismatched ABI.
    static bool IsEngineUsable() { return bEngineUsable; }

private:
    static bool bEngineUsable;
};
