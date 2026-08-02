#include "TitanTerrainActor.h"

#include "Async/Async.h"
#include "ProceduralMeshComponent.h"
#include "Landscape.h"
#include "LandscapeInfo.h"
#include "LandscapeProxy.h"
#include "Engine/World.h"
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

// Landscape geometry is not free-form: a landscape is a whole number of
// components, each a whole number of subsections, each SubsectionSizeQuads
// across, and the vertex count is that product plus one. Ask for 300 and you
// get nothing. 63-quad subsections, two per component, is Unreal's own default
// and gives 126 quads per component.
constexpr int32 kSubsectionSizeQuads = 63;
constexpr int32 kNumSubsections = 2;
constexpr int32 kQuadsPerComponent = kSubsectionSizeQuads * kNumSubsections;

/** The valid landscape vertex count closest to (and at least) a requested one. */
int32 LandscapeVertsFor(int32 Requested)
{
    const int32 Components = FMath::Max(1, FMath::RoundToInt(
        static_cast<float>(Requested - 1) / static_cast<float>(kQuadsPerComponent)));
    return Components * kQuadsPerComponent + 1;
}

// Landscape stores height as uint16 with 32768 as the zero plane, scaled by
// LANDSCAPE_ZSCALE (1/128) and then by the actor's Z scale. Spreading the
// terrain across the full 16-bit range and deriving Z scale from that is what
// keeps the vertical precision — anchoring to a fixed Z scale instead throws
// most of the range away on a terrain that does not happen to fill it.
constexpr float kLandscapeZScale = 1.0f / 128.0f;
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
        int32 WorldSize;
        ETitanOutput Output;
        float Scale, Height, Exponent, Warp, UnitsPerWorldUnit, CellSize;
        int32 Octaves, NoiseType;
        uint32 SeedHash;
        bool bRivers; int32 RiverPasses; float RiverStrength;
        bool bDroplets; int32 DropletCount; int32 SpawnMode;
        bool bThermal; int32 ThermalPasses; float Talus;
    };
    FSettings S;
    S.Resolution = FMath::Clamp(Resolution, 64, 1024);
    // Cell size is world size divided by sample count, exactly as TitanLab
    // computes it. It used to be hardcoded to 1.0, which silently made
    // Resolution a world-size control: the terrain's extent was its pixel
    // count, so raising detail widened the landscape and the same seed
    // produced a different place. It also meant the plugin could not
    // reproduce a project from the app unless the app happened to be at
    // World Size == Resolution, against a manifest promising exactly that.
    S.WorldSize = FMath::Clamp(WorldSize, 64, 8192);
    S.Output = Output;
    S.CellSize = static_cast<float>(S.WorldSize) / static_cast<float>(S.Resolution);
    S.Scale = Scale;
    S.Height = HeightMultiplier;
    S.Exponent = Exponent;
    S.Warp = WarpStrength;
    S.UnitsPerWorldUnit = UnitsPerWorldUnit;
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

        titan_configure(Engine, S.Resolution, S.CellSize, S.Scale, S.Height, S.SeedHash,
                        S.Octaves, 0.5f, 2.0f, S.Exponent, S.NoiseType, S.Warp,
                        1.0f, 2.0f, 0.0f, 0.0f);
        titan_generate(Engine);
        if (S.bRivers) { titan_erode_fluvial(Engine, S.RiverPasses, S.RiverStrength); }
        if (S.bDroplets) { titan_erode_hydraulic(Engine, S.DropletCount, S.SpawnMode); }
        if (S.bThermal) { titan_erode_thermal(Engine, S.ThermalPasses, S.Talus, 0.5f); }

        if (S.Output == ETitanOutput::Landscape)
        {
            // Read the surface out rather than building a mesh: a landscape
            // wants a heightfield, and the mesh would be thrown away.
            const int32 Count = S.Resolution * S.Resolution;
            TArray<float> Field;
            Field.SetNumUninitialized(Count);
            titan_read_height(Engine, Field.GetData(), Count);
            titan_destroy(Engine);

            TSharedPtr<FTitanHeightField> HF = MakeShared<FTitanHeightField>();
            HF->VertsPerSide = LandscapeVertsFor(S.Resolution);
            HF->SubsectionSizeQuads = kSubsectionSizeQuads;
            HF->NumSubsections = kNumSubsections;

            const int32 V = HF->VertsPerSide;
            HF->Heights.SetNumUninitialized(V * V);

            float MinH = TNumericLimits<float>::Max();
            float MaxH = TNumericLimits<float>::Lowest();
            for (int32 i = 0; i < Count; ++i)
            {
                MinH = FMath::Min(MinH, Field[i]);
                MaxH = FMath::Max(MaxH, Field[i]);
            }
            const float RangeH = FMath::Max(MaxH - MinH, KINDA_SMALL_NUMBER);

            // Bilinear resample from the engine's grid onto the landscape's.
            // The two rarely match: landscape sizes are quantised and the
            // engine's resolution is not.
            const float Step = static_cast<float>(S.Resolution - 1)
                             / static_cast<float>(V - 1);
            for (int32 y = 0; y < V; ++y)
            {
                const float sy = FMath::Min(y * Step, static_cast<float>(S.Resolution - 1));
                const int32 y0 = FMath::FloorToInt(sy);
                const int32 y1 = FMath::Min(y0 + 1, S.Resolution - 1);
                const float fy = sy - y0;
                for (int32 x = 0; x < V; ++x)
                {
                    const float sx = FMath::Min(x * Step, static_cast<float>(S.Resolution - 1));
                    const int32 x0 = FMath::FloorToInt(sx);
                    const int32 x1 = FMath::Min(x0 + 1, S.Resolution - 1);
                    const float fx = sx - x0;

                    const float h = FMath::Lerp(
                        FMath::Lerp(Field[y0 * S.Resolution + x0], Field[y0 * S.Resolution + x1], fx),
                        FMath::Lerp(Field[y1 * S.Resolution + x0], Field[y1 * S.Resolution + x1], fx),
                        fy);

                    // Spread across the whole 16-bit range; the actor's Z
                    // scale below turns that back into centimetres.
                    const float Normalized = (h - MinH) / RangeH;
                    HF->Heights[y * V + x] = static_cast<uint16>(
                        FMath::Clamp(Normalized * 65535.0f, 0.0f, 65535.0f));
                }
            }

            // X and Y: the landscape is V-1 quads across and must cover
            // WorldSize world units, same as the procedural mesh path.
            const float SpanCm = static_cast<float>(S.WorldSize) * S.UnitsPerWorldUnit;
            const float ScaleXY = SpanCm / static_cast<float>(V - 1);
            // Z: 65535 quantisation steps must come out as the terrain's real
            // vertical range in centimetres.
            const float ScaleZ = (RangeH * S.UnitsPerWorldUnit) * 128.0f / 65535.0f;
            HF->Scale = FVector(ScaleXY, ScaleXY, FMath::Max(ScaleZ, KINDA_SMALL_NUMBER));

            AsyncTask(ENamedThreads::GameThread, [WeakThis, HF, MyGeneration]()
            {
                ATitanTerrainActor* Self = WeakThis.Get();
                if (!Self || Self->GenerationCounter.load() != MyGeneration)
                {
                    return;
                }
                Self->ApplyLandscape(HF);
            });
            return;
        }

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
        const float U = S.UnitsPerWorldUnit;
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

void ATitanTerrainActor::ApplyLandscape(TSharedPtr<FTitanHeightField> Field)
{
#if WITH_EDITOR
    if (!Field.IsValid() || Field->VertsPerSide <= 0)
    {
        return;
    }

    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }

    // Replace rather than accumulate: regenerating should not leave a stack of
    // landscapes on top of each other.
    if (SpawnedLandscape)
    {
        SpawnedLandscape->Destroy();
        SpawnedLandscape = nullptr;
    }
    // The procedural mesh is this actor's other output; clear it so the two
    // are never both standing in the same place.
    if (ProceduralMesh)
    {
        ProceduralMesh->ClearAllMeshSections();
    }

    const int32 V = Field->VertsPerSide;

    FActorSpawnParameters Params;
    Params.ObjectFlags = RF_Transactional;
    ALandscape* Landscape = World->SpawnActor<ALandscape>(
        GetActorLocation(), GetActorRotation(), Params);
    if (!Landscape)
    {
        UE_LOG(LogTemp, Warning, TEXT("TitanBridge: could not spawn a Landscape actor."));
        return;
    }

    Landscape->SetActorRelativeScale3D(Field->Scale);

    // Automatic lighting LOD, the same rule the New Landscape tool uses.
    Landscape->StaticLightingLOD = FMath::DivideAndRoundUp(
        FMath::CeilLogTwo((V * V) / (2048 * 2048) + 1), static_cast<uint32>(2));

    // Height data is keyed by edit-layer guid; the invalid guid is the
    // "no edit layers" key, which is what a plain landscape wants.
    TMap<FGuid, TArray<uint16>> HeightData;
    HeightData.Add(FGuid(), Field->Heights);
    TMap<FGuid, TArray<FLandscapeImportLayerInfo>> MaterialLayers;
    MaterialLayers.Add(FGuid(), TArray<FLandscapeImportLayerInfo>());

    // A *fresh* guid, not the actor's own. Passing the actor's existing guid
    // makes Import treat the call as a reimport of a landscape it already
    // knows, and it quietly produces no components — the actor exists, with
    // zero bounds and nothing to sculpt.
    Landscape->Import(
        FGuid::NewGuid(),
        0, 0, V - 1, V - 1,
        Field->NumSubsections,
        Field->SubsectionSizeQuads,
        HeightData,
        nullptr,
        MaterialLayers,
        ELandscapeImportAlphamapType::Additive,
        TArrayView<const FLandscapeLayer>());

    // Without a LandscapeInfo the landscape renders but behaves like a prop:
    // no sculpting, no layer painting.
    if (ULandscapeInfo* Info = Landscape->GetLandscapeInfo())
    {
        Info->UpdateLayerInfoMap(Landscape);
    }

    SpawnedLandscape = Landscape;
#else
    (void)Field;
    UE_LOG(LogTemp, Warning,
           TEXT("TitanBridge: Landscape output is editor only; nothing was built."));
#endif
}
