#include "TitanTerrainActor.h"

#include "Async/Async.h"
#include "ProceduralMeshComponent.h"
#include "TitanBridgeModule.h"
#include "TitanCAPI.h"

namespace
{
// Seed-string hash, computed by libTitanCore.
//
// Deliberately delegates to the engine rather than reimplementing FNV-1a.
// This plugin used to hash TCHARs truncated to their low byte, TitanLab
// hashed UTF-8 bytes, and the web lab hashed UTF-16 code units — three
// "identical" hashes that agreed only on ASCII, so a seed containing an
// accent or an emoji produced three different terrains across the three
// products. titan_hash_seed is the single definition; feed it UTF-8.
uint32 HashSeed(const FString& Seed)
{
    const FTCHARToUTF8 Utf8(*Seed);
    return titan_hash_seed(Utf8.Get());
}
} // namespace

ATitanTerrainActor::ATitanTerrainActor()
{
    PrimaryActorTick.bCanEverTick = false;

    ProceduralMesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("TitanMesh"));
    RootComponent = ProceduralMesh;
    ProceduralMesh->bUseAsyncCooking = true;
}

void ATitanTerrainActor::BeginPlay()
{
    Super::BeginPlay();
    if (ProceduralMesh->GetNumSections() == 0)
    {
        GenerateTerrain();
    }
}

void ATitanTerrainActor::GenerateTerrain()
{
    RunGeneration(bGenerateCollision);
}

void ATitanTerrainActor::RandomizeSeed()
{
    const TCHAR Charset[] = TEXT("abcdefghijklmnopqrstuvwxyz0123456789");
    FString Fresh;
    for (int32 i = 0; i < 7; ++i)
    {
        Fresh.AppendChar(Charset[FMath::RandRange(0, 35)]);
    }
    Seed = Fresh;
    GenerateTerrain();
}

void ATitanTerrainActor::FinalizeCollision()
{
    RunGeneration(true);
}

void ATitanTerrainActor::RunGeneration(bool bWithCollision)
{
    // Refuse to generate against a mismatched engine binary rather than emit
    // terrain from an ABI we cannot trust. StartupModule logs the detail.
    if (!FTitanBridgeModule::IsEngineUsable())
    {
        return;
    }

    const int32 MyGeneration = ++GenerationCounter;

    // Snapshot all settings; the worker touches no UObject state.
    struct FSettings
    {
        int32 Resolution;
        float Scale, Height, Exponent, Warp, UnitsPerCell;
        int32 Octaves, NoiseType;
        uint32 SeedHash;
        bool bRivers; int32 RiverPasses; float RiverStrength;
        bool bDroplets; int32 DropletCount; int32 SpawnMode;
        bool bThermal; int32 ThermalPasses; float Talus;
    };
    FSettings S;
    S.Resolution = FMath::Clamp(Resolution, 64, 1024);
    S.Scale = Scale;
    S.Height = HeightMultiplier;
    S.Exponent = Exponent;
    S.Warp = WarpStrength;
    S.UnitsPerCell = UnitsPerCell;
    S.Octaves = Octaves;
    S.NoiseType = static_cast<int32>(NoiseType);
    S.SeedHash = HashSeed(Seed);
    S.bRivers = bRiverNetworks;
    S.RiverPasses = RiverPasses;
    S.RiverStrength = RiverStrength;
    S.bDroplets = bHydraulicErosion;
    S.DropletCount = DropletRounds * 16384;
    S.SpawnMode = static_cast<int32>(Rainfall);
    S.bThermal = bThermalWeathering;
    S.ThermalPasses = ThermalPasses;
    S.Talus = TalusAngle;

    TWeakObjectPtr<ATitanTerrainActor> WeakThis(this);

    Async(EAsyncExecution::Thread, [WeakThis, S, MyGeneration, bWithCollision]()
    {
        TitanHandle* Engine = titan_create();
        if (!Engine)
        {
            return;
        }

        titan_configure(Engine, S.Resolution, 1.0f, S.Scale, S.Height, S.SeedHash,
                        S.Octaves, 0.5f, 2.0f, S.Exponent, S.NoiseType, S.Warp,
                        1.0f, 2.0f, 0.0f, 0.0f);
        titan_generate(Engine);
        if (S.bRivers) { titan_erode_fluvial(Engine, S.RiverPasses, S.RiverStrength); }
        if (S.bDroplets) { titan_erode_hydraulic(Engine, S.DropletCount, S.SpawnMode); }
        if (S.bThermal) { titan_erode_thermal(Engine, S.ThermalPasses, S.Talus, 0.5f); }
        titan_build_mesh(Engine);

        const int32 VertexCount = titan_mesh_vertex_count(Engine);
        const int32 IndexCount = titan_mesh_index_count(Engine);
        const float* Positions = titan_mesh_positions_ptr(Engine);
        const float* Normals = titan_mesh_normals_ptr(Engine);
        const float* Colors = titan_mesh_colors_ptr(Engine);
        const float* UVs = titan_mesh_uvs_ptr(Engine);
        const uint32* Indices = titan_mesh_indices_ptr(Engine);

        TSharedPtr<FTitanMeshData> Data = MakeShared<FTitanMeshData>();
        Data->Vertices.Reserve(VertexCount);
        Data->Normals.Reserve(VertexCount);
        Data->UVs.Reserve(VertexCount);
        Data->VertexColors.Reserve(VertexCount);
        Data->Triangles.Reserve(IndexCount);

        // Core is Y-up; Unreal is Z-up in centimetres. Swap Y/Z and scale.
        const float U = S.UnitsPerCell;
        for (int32 V = 0; V < VertexCount; ++V)
        {
            Data->Vertices.Add(FVector(Positions[V * 3 + 0] * U,
                                       Positions[V * 3 + 2] * U,
                                       Positions[V * 3 + 1] * U));
            Data->Normals.Add(FVector(Normals[V * 3 + 0],
                                      Normals[V * 3 + 2],
                                      Normals[V * 3 + 1]));
            Data->UVs.Add(FVector2D(UVs[V * 2 + 0], UVs[V * 2 + 1]));
            Data->VertexColors.Add(FColor(
                static_cast<uint8>(FMath::Clamp(Colors[V * 4 + 0], 0.0f, 1.0f) * 255.0f),
                static_cast<uint8>(FMath::Clamp(Colors[V * 4 + 1], 0.0f, 1.0f) * 255.0f),
                static_cast<uint8>(FMath::Clamp(Colors[V * 4 + 2], 0.0f, 1.0f) * 255.0f),
                static_cast<uint8>(FMath::Clamp(Colors[V * 4 + 3], 0.0f, 1.0f) * 255.0f)));
        }

        // Axis swap mirrors handedness — reverse each triangle's winding.
        for (int32 T = 0; T + 2 < IndexCount; T += 3)
        {
            Data->Triangles.Add(static_cast<int32>(Indices[T + 0]));
            Data->Triangles.Add(static_cast<int32>(Indices[T + 2]));
            Data->Triangles.Add(static_cast<int32>(Indices[T + 1]));
        }

        titan_destroy(Engine);

        AsyncTask(ENamedThreads::GameThread, [WeakThis, Data, MyGeneration, bWithCollision]()
        {
            ATitanTerrainActor* Self = WeakThis.Get();
            if (!Self || Self->GenerationCounter.load() != MyGeneration)
            {
                return; // superseded or actor gone
            }
            Self->ApplyMesh(Data, bWithCollision);
        });
    });
}

void ATitanTerrainActor::ApplyMesh(TSharedPtr<FTitanMeshData> Data, bool bWithCollision)
{
    if (!Data.IsValid() || !ProceduralMesh)
    {
        return;
    }

    ProceduralMesh->CreateMeshSection(
        0,
        Data->Vertices,
        Data->Triangles,
        Data->Normals,
        Data->UVs,
        Data->VertexColors,
        TArray<FProcMeshTangent>(),
        bWithCollision);

    ProceduralMesh->SetCastShadow(true);
    ProceduralMesh->MarkRenderStateDirty();
}
