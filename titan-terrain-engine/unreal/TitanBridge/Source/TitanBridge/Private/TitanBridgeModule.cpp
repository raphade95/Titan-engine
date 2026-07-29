#include "TitanBridgeModule.h"

#include "TitanCAPI.h"

DEFINE_LOG_CATEGORY_STATIC(LogTitanBridge, Log, All);

bool FTitanBridgeModule::bEngineUsable = false;

void FTitanBridgeModule::StartupModule()
{
    // Verify the linked library matches the headers this module was compiled
    // against. The plugin ships a prebuilt libTitanCore plus a *copy* of the
    // engine headers, and those two had already drifted apart once: the
    // shipped binary exported 35 of the ~60 API functions — missing everything
    // added in v0.4 and v0.5 — while the headers declared all of them.
    // titan_api_version() exists precisely to catch that, so actually check it
    // rather than only logging it.
    const int LinkedVersion = titan_api_version();
    if (LinkedVersion != TITAN_C_API_VERSION)
    {
        bEngineUsable = false;
        UE_LOG(LogTitanBridge, Error,
               TEXT("TitanBridge ABI mismatch: the linked libTitanCore reports C API "
                    "v%d but this module was built against v%d. The prebuilt library "
                    "in ThirdParty/TitanCore/lib is stale — rebuild it from cpp/ with "
                    "CMake and repackage. Terrain generation is disabled."),
               LinkedVersion, TITAN_C_API_VERSION);
        return;
    }

    bEngineUsable = true;
    UE_LOG(LogTitanBridge, Log, TEXT("TitanBridge loaded — %hs (C API v%d)"),
           titan_version(), LinkedVersion);
}

void FTitanBridgeModule::ShutdownModule()
{
    bEngineUsable = false;
}

IMPLEMENT_MODULE(FTitanBridgeModule, TitanBridge)
