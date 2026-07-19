#include "TitanBridgeModule.h"

#include "TitanCAPI.h"

DEFINE_LOG_CATEGORY_STATIC(LogTitanBridge, Log, All);

void FTitanBridgeModule::StartupModule()
{
    UE_LOG(LogTitanBridge, Log, TEXT("TitanBridge loaded — %hs (C API v%d)"),
           titan_version(), titan_api_version());
}

void FTitanBridgeModule::ShutdownModule()
{
}

IMPLEMENT_MODULE(FTitanBridgeModule, TitanBridge)
