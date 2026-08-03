#include "TitanTerrainActor.h"

#include "Async/Async.h"
#include "ProceduralMeshComponent.h"
#include "Landscape.h"
#include "LandscapeInfo.h"
#include "LandscapeProxy.h"
#include "LandscapeSubsystem.h"
#include "LandscapeStreamingProxy.h"
#include "EngineUtils.h"
#include "WorldPartition/WorldPartition.h"
#include "Engine/World.h"
#include "Materials/MaterialInterface.h"
#include "UObject/ConstructorHelpers.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Dom/JsonObject.h"
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

    // An engine material by default, so generated terrain arrives shaded
    // instead of as the grey "no material assigned" placeholder. The plugin
    // declares CanContainContent false and so cannot ship a material of its
    // own; this is a stand-in the user is expected to replace, which is why
    // it is an exposed property rather than something applied unconditionally.
    static ConstructorHelpers::FObjectFinder<UMaterialInterface> DefaultMat(
        TEXT("/Engine/BasicShapes/BasicShapeMaterial"));
    if (DefaultMat.Succeeded())
    {
        TerrainMaterial = DefaultMat.Object;
    }
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

            // Where to put the landscape so it lands where the mesh would.
            //
            // A landscape's world Z at sample v is
            //     ActorZ + (v - 32768) * (1/128) * ScaleZ
            // so v = 0 — the terrain's lowest point — sits 32768 quantisation
            // steps BELOW the actor. Spawn at the actor's own Z and the
            // terrain straddles it: the lower half buried, the upper half
            // hanging in the air. The mesh path has no such offset because it
            // writes true heights, so the two outputs disagreed about where
            // the same terrain was by half its vertical range.
            //
            // Offsetting by that half-range puts the landscape's floor exactly
            // where the mesh's floor is.
            // Only the 32768-step term. Including MinH here would carry the
            // terrain's own lowest height as an offset, floating the base
            // above the actor by however far the engine's minimum happens to
            // sit above zero — 145 cm on a typical project. The base belongs
            // at the actor's Z, so the terrain lands on the ground you placed
            // it on rather than hovering a metre and a half over it.
            HF->BaseOffsetCm = 32768.0 * kLandscapeZScale * ScaleZ;

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

        // The mesh's floor goes to the actor's Z as well, so the two outputs
        // agree about where the terrain is. The engine's lowest sample is not
        // zero — erosion and deposition move both ends of the range — so
        // writing raw heights left the terrain hovering above its own actor.
        float MeshMinH = TNumericLimits<float>::Max();
        {
            const int32 FieldCount = S.Resolution * S.Resolution;
            TArray<float> Field;
            Field.SetNumUninitialized(FieldCount);
            titan_read_height(Engine, Field.GetData(), FieldCount);
            for (int32 i = 0; i < FieldCount; ++i)
            {
                MeshMinH = FMath::Min(MeshMinH, Field[i]);
            }
        }
        if (!FMath::IsFinite(MeshMinH)) { MeshMinH = 0.0f; }

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
                                       (Positions[V * 3 + 1] - MeshMinH) * U));
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

    if (TerrainMaterial)
    {
        ProceduralMesh->SetMaterial(0, TerrainMaterial);
    }
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
    FVector SpawnAt = GetActorLocation();
    SpawnAt.Z += Field->BaseOffsetCm;
    ALandscape* Landscape = World->SpawnActor<ALandscape>(
        SpawnAt, GetActorRotation(), Params);
    if (!Landscape)
    {
        UE_LOG(LogTemp, Warning, TEXT("TitanBridge: could not spawn a Landscape actor."));
        return;
    }

    Landscape->SetActorRelativeScale3D(Field->Scale);
    if (TerrainMaterial)
    {
        Landscape->LandscapeMaterial = TerrainMaterial;
    }

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
    ULandscapeInfo* Info = Landscape->GetLandscapeInfo();
    if (Info)
    {
        Info->UpdateLayerInfoMap(Landscape);
    }

    // Split into streaming proxies, or the landscape never streams.
    //
    // A landscape imported as a single actor holds every component always
    // loaded — which is precisely what World Partition exists to avoid, so a
    // large world built this way pays the cost of WP and gets none of the
    // benefit. ChangeGridSize is what Unreal's own New Landscape tool calls
    // for this, and it replaces the one actor with a grid of
    // ALandscapeStreamingProxy actors that WP can stream in and out.
    //
    // Only in a partitioned world: outside one there are no proxies to make
    // and nothing to stream.
    ULandscapeSubsystem* Subsystem = World->GetSubsystem<ULandscapeSubsystem>();
    if (Info && World->GetWorldPartition() != nullptr && Subsystem)
    {
        Subsystem->ChangeGridSize(Info, static_cast<uint32>(
            FMath::Clamp(WorldPartitionGridSize, 1, 16)));

        // Reported, because it is the difference between a world that streams
        // and one that only looks like it does, and nothing else says so.
        // Note a landscape smaller than one grid cell correctly yields zero
        // proxies — there is nothing to split.
        int32 ProxyCount = 0;
        for (TActorIterator<ALandscapeStreamingProxy> It(World); It; ++It)
        {
            if (It->GetLandscapeActor() == Landscape) { ++ProxyCount; }
        }
        UE_LOG(LogTemp, Log,
               TEXT("TitanBridge: %dx%d landscape split into %d streaming proxies "
                    "(grid size %d)"), V, V, ProxyCount, WorldPartitionGridSize);
    }

    SpawnedLandscape = Landscape;
#else
    (void)Field;
    UE_LOG(LogTemp, Warning,
           TEXT("TitanBridge: Landscape output is editor only; nothing was built."));
#endif
}

// ---------------------------------------------------------------------------
// TitanLab project import
// ---------------------------------------------------------------------------

void ATitanTerrainActor::ImportProject()
{
    ImportReport.Empty();
    TArray<FString> Notes;
    auto Fail = [&](const FString& Why)
    {
        ImportReport = FString::Printf(TEXT("Import failed: %s"), *Why);
        UE_LOG(LogTemp, Warning, TEXT("TitanBridge: %s"), *ImportReport);
    };

    const FString Path = ProjectFile.FilePath;
    if (Path.IsEmpty())
    {
        Fail(TEXT("no project file set."));
        return;
    }

    FString Resolved = Path;
    if (FPaths::IsRelative(Resolved))
    {
        Resolved = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), Resolved);
    }

    FString Text;
    if (!FFileHelper::LoadFileToString(Text, *Resolved))
    {
        Fail(FString::Printf(TEXT("could not read '%s'."), *Resolved));
        return;
    }

    TSharedPtr<FJsonObject> Root;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
    if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
    {
        Fail(TEXT("the file is not valid JSON."));
        return;
    }

    // Version is a minimum-reader rule, not a stamp of what wrote the file:
    // refusing a version this build cannot honour is the point of it. A v4
    // file is graph-driven, and a graph is macOS-only — there is nothing here
    // that could reproduce it, and approximating it would be worse than
    // saying so.
    const int32 Version = static_cast<int32>(Root->GetNumberField(TEXT("version")));
    if (Version >= 4)
    {
        Fail(FString::Printf(
            TEXT("this project is v%d — its terrain is made by a node graph, which "
                 "this plugin cannot evaluate. Bake the graph to a layer stack in "
                 "TitanLab and save again."), Version));
        return;
    }

    const TSharedPtr<FJsonObject>* Params = nullptr;
    if (!Root->TryGetObjectField(TEXT("params"), Params) || !Params)
    {
        Fail(TEXT("the file has no params block."));
        return;
    }

    // --- base terrain: this part reproduces exactly ------------------------
    const TSharedPtr<FJsonObject>& P = *Params;
    P->TryGetStringField(TEXT("seed"), Seed);

    double Number = 0.0;
    if (P->TryGetNumberField(TEXT("size"), Number))
    {
        Resolution = FMath::Clamp(static_cast<int32>(Number), 64, 1024);
    }
    if (P->TryGetNumberField(TEXT("worldSize"), Number))
    {
        WorldSize = FMath::Clamp(static_cast<int32>(Number), 64, 8192);
    }
    if (P->TryGetNumberField(TEXT("scale"), Number))            { Scale = Number; }
    if (P->TryGetNumberField(TEXT("heightMultiplier"), Number)) { HeightMultiplier = Number; }
    if (P->TryGetNumberField(TEXT("octaves"), Number))          { Octaves = static_cast<int32>(Number); }
    if (P->TryGetNumberField(TEXT("exponent"), Number))         { Exponent = Number; }
    if (P->TryGetNumberField(TEXT("warpStrength"), Number))     { WarpStrength = Number; }

    // TitanLab stores the structure by name. Only the four this actor exposes
    // can be honoured; the rest are real engine noise types with no property
    // here, so say so rather than quietly substituting one.
    FString NoiseName;
    if (P->TryGetStringField(TEXT("noiseType"), NoiseName))
    {
        if (NoiseName == TEXT("none"))          { NoiseType = ETitanNoiseType::Flat; }
        else if (NoiseName == TEXT("standard")) { NoiseType = ETitanNoiseType::Simplex; }
        else if (NoiseName == TEXT("ridged"))   { NoiseType = ETitanNoiseType::Ridged; }
        else if (NoiseName == TEXT("billow"))   { NoiseType = ETitanNoiseType::Billow; }
        else
        {
            Notes.Add(FString::Printf(
                TEXT("noise structure '%s' has no equivalent here; left as-is"),
                *NoiseName));
        }
    }

    // Persistence and lacunarity are fixed at 0.5 / 2.0 in this plugin's
    // configure call, so a project that moved them will not reproduce.
    double Persistence = 0.5, Lacunarity = 2.0;
    P->TryGetNumberField(TEXT("persistence"), Persistence);
    P->TryGetNumberField(TEXT("lacunarity"), Lacunarity);
    if (!FMath::IsNearlyEqual(static_cast<float>(Persistence), 0.5f, 1e-3f)
        || !FMath::IsNearlyEqual(static_cast<float>(Lacunarity), 2.0f, 1e-3f))
    {
        Notes.Add(FString::Printf(
            TEXT("persistence %.2f / lacunarity %.2f cannot be set here (fixed at 0.5 / 2.0)"),
            Persistence, Lacunarity));
    }

    // --- the stack: partly ------------------------------------------------
    //
    // This actor has three erosion toggles; TitanLab has an ordered stack of
    // any length. Matching by kind is the honest subset — it cannot preserve
    // ordering, repeats, masks or per-layer curves, so anything it cannot
    // carry is listed rather than dropped in silence.
    bRiverNetworks = false;
    bHydraulicErosion = false;
    bThermalWeathering = false;

    int32 Recognised = 0;
    TArray<FString> Skipped;
    const TArray<TSharedPtr<FJsonValue>>* Stack = nullptr;
    if (Root->TryGetArrayField(TEXT("stack"), Stack) && Stack)
    {
        for (const TSharedPtr<FJsonValue>& Entry : *Stack)
        {
            const TSharedPtr<FJsonObject> Layer = Entry->AsObject();
            if (!Layer.IsValid()) { continue; }
            if (!Layer->GetBoolField(TEXT("enabled"))) { continue; }

            const FString Type = Layer->GetStringField(TEXT("type"));
            const TSharedPtr<FJsonObject>* LP = nullptr;
            Layer->TryGetObjectField(TEXT("params"), LP);

            auto Param = [&](const TCHAR* Key, double Fallback) -> double
            {
                double Out = Fallback;
                if (LP && (*LP)->TryGetNumberField(Key, Out)) { return Out; }
                return Fallback;
            };

            if (Type == TEXT("fluvial"))
            {
                bRiverNetworks = true;
                RiverPasses = FMath::Clamp(static_cast<int32>(Param(TEXT("passes"), 3)), 1, 12);
                RiverStrength = Param(TEXT("strength"), 1.2);
                ++Recognised;
            }
            else if (Type == TEXT("hydraulic"))
            {
                bHydraulicErosion = true;
                const int32 Iterations = static_cast<int32>(Param(TEXT("iterations"), 65536));
                DropletRounds = FMath::Clamp(FMath::RoundToInt(Iterations / 16384.0f), 1, 16);
                ++Recognised;
            }
            else if (Type == TEXT("thermal"))
            {
                bThermalWeathering = true;
                ThermalPasses = FMath::Clamp(static_cast<int32>(Param(TEXT("passes"), 12)), 1, 50);
                TalusAngle = Param(TEXT("talusAngle"), 35.0);
                ++Recognised;
            }
            else
            {
                Skipped.AddUnique(Type);
            }
        }
    }

    if (Skipped.Num() > 0)
    {
        Notes.Add(FString::Printf(TEXT("%d layer kind(s) not reproduced: %s"),
                                  Skipped.Num(), *FString::Join(Skipped, TEXT(", "))));
    }

    ImportReport = FString::Printf(TEXT("Imported v%d: base terrain, %d erosion layer(s)."),
                                   Version, Recognised);
    if (Notes.Num() > 0)
    {
        ImportReport += FString::Printf(TEXT(" Not carried over: %s."),
                                        *FString::Join(Notes, TEXT("; ")));
    }
    UE_LOG(LogTemp, Log, TEXT("TitanBridge: %s"), *ImportReport);

    GenerateTerrain();
}
