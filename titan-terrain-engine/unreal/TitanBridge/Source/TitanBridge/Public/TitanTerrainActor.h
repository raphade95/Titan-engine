#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include <atomic>
#include "TitanTerrainActor.generated.h"

class UProceduralMeshComponent;

UENUM(BlueprintType)
enum class ETitanNoiseType : uint8
{
    Flat = 0     UMETA(DisplayName = "Flat"),
    Simplex = 1  UMETA(DisplayName = "Simplex fBm"),
    Ridged = 2   UMETA(DisplayName = "Ridged Multifractal"),
    Billow = 3   UMETA(DisplayName = "Billow")
};

UENUM(BlueprintType)
enum class ETitanRainfall : uint8
{
    Uniform = 0   UMETA(DisplayName = "Uniform"),
    Highlands = 1 UMETA(DisplayName = "Highlands")
};

UENUM(BlueprintType)
enum class ETitanOutput : uint8
{
    /** A procedural mesh on this actor. Fast to regenerate; good for iterating. */
    ProceduralMesh = 0 UMETA(DisplayName = "Procedural Mesh"),
    /**
     * A real Landscape actor. Slower to build and quantised to a valid
     * landscape size, but it is what the rest of the engine expects terrain to
     * be: landscape materials and layer painting, foliage, World Partition,
     * and the sculpt tools all work on it and none of them work on a
     * procedural mesh. Editor only.
     */
    Landscape = 1 UMETA(DisplayName = "Landscape")
};

/**
 * Deterministic procedural terrain powered by libTitanCore.
 * Same seed + same settings = identical terrain, on Mac and Windows,
 * in TitanLab and in this editor.
 */
UCLASS(HideCategories = (Input, Replication, Collision, HLOD, Physics))
class TITANBRIDGE_API ATitanTerrainActor : public AActor
{
    GENERATED_BODY()

public:
    ATitanTerrainActor();

    // --- Base terrain ------------------------------------------------------

    /**
     * What Generate Terrain builds. Landscape is editor only: it spawns a
     * separate Landscape actor next to this one, and this actor's own
     * procedural mesh is cleared.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Titan|Base")
    ETitanOutput Output = ETitanOutput::ProceduralMesh;

    /**
     * Material for the generated terrain, applied to whichever output is
     * used. Defaults to an engine material so terrain arrives shaded rather
     * than as the grey default-material placeholder.
     *
     * The engine writes biome colour into vertex colours; a material that
     * reads Vertex Color will show it. Landscape output needs a material
     * built for landscape if you want layer painting.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Titan|Base")
    TObjectPtr<UMaterialInterface> TerrainMaterial;

    /**
     * Landscape components per streaming proxy, in a World Partition world.
     *
     * A landscape built as one actor never streams — every component is always
     * loaded, which is exactly what World Partition exists to avoid. Splitting
     * it into proxies on a grid is how it becomes streamable. Smaller values
     * mean more, smaller proxies: finer streaming granularity, more actors.
     * Four matches Unreal's own New Landscape default.
     *
     * Ignored outside World Partition, where there is nothing to stream.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Titan|Base",
              meta = (ClampMin = 1, ClampMax = 16,
                      EditCondition = "Output == ETitanOutput::Landscape"))
    int32 WorldPartitionGridSize = 4;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Titan|Base")
    FString Seed = TEXT("titan");

    /**
     * Sample density per side. Detail only — this does not change how large
     * the terrain is. Set World Size for that.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Titan|Base",
              meta = (ClampMin = 64, ClampMax = 1024))
    int32 Resolution = 256;

    /**
     * How much world the terrain covers, in Titan world units. This is the
     * same control as World Size in TitanLab, and it must match the project
     * you are reproducing: noise is sampled in world space, so a terrain
     * generated over 128 units is a different landscape from the same seed
     * generated over 256, at any resolution.
     *
     * Multiply by Units Per World Unit to get the size in centimetres.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Titan|Base",
              meta = (ClampMin = 64, ClampMax = 8192))
    int32 WorldSize = 256;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Titan|Base")
    ETitanNoiseType NoiseType = ETitanNoiseType::Ridged;

    /** Noise features across the terrain extent. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Titan|Base",
              meta = (ClampMin = "0.1", ClampMax = "10.0"))
    float Scale = 2.5f;

    /** Peak height in engine (core) units before world scaling. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Titan|Base",
              meta = (ClampMin = "10.0", ClampMax = "200.0"))
    float HeightMultiplier = 70.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Titan|Base",
              meta = (ClampMin = 1, ClampMax = 12))
    int32 Octaves = 8;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Titan|Base",
              meta = (ClampMin = "0.5", ClampMax = "3.0"))
    float Exponent = 1.1f;

    /** Domain warp strength — bends noise into tectonic-looking flows. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Titan|Base",
              meta = (ClampMin = "0.0", ClampMax = "2.0"))
    float WarpStrength = 0.6f;

    /**
     * Unreal centimetres per Titan world unit. The actor's terrain ends up
     * World Size * this across, so the default 256 units at 100 cm is a
     * 256 m tile.
     *
     * Named per cell before World Size existed, when a cell was a world unit
     * by accident of the two being locked together.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Titan|Base",
              meta = (ClampMin = "10.0", ClampMax = "10000.0"))
    float UnitsPerWorldUnit = 100.0f;

    // --- Simulation stack (runs in order) ----------------------------------

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Titan|Erosion")
    bool bRiverNetworks = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Titan|Erosion",
              meta = (EditCondition = "bRiverNetworks", ClampMin = 1, ClampMax = 10))
    int32 RiverPasses = 3;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Titan|Erosion",
              meta = (EditCondition = "bRiverNetworks", ClampMin = "0.1", ClampMax = "3.0"))
    float RiverStrength = 1.2f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Titan|Erosion")
    bool bHydraulicErosion = true;

    /** Droplet count in rounds of 16384 (the determinism batch size). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Titan|Erosion",
              meta = (EditCondition = "bHydraulicErosion", ClampMin = 1, ClampMax = 12))
    int32 DropletRounds = 4;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Titan|Erosion",
              meta = (EditCondition = "bHydraulicErosion"))
    ETitanRainfall Rainfall = ETitanRainfall::Highlands;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Titan|Erosion")
    bool bThermalWeathering = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Titan|Erosion",
              meta = (EditCondition = "bThermalWeathering", ClampMin = 1, ClampMax = 50))
    int32 ThermalPasses = 12;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Titan|Erosion",
              meta = (EditCondition = "bThermalWeathering", ClampMin = "20.0", ClampMax = "45.0"))
    float TalusAngle = 35.0f;

    // --- Collision ----------------------------------------------------------

    /** Off while iterating (collision cooking on large meshes is slow);
        finalize once the terrain is settled. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Titan|Collision")
    bool bGenerateCollision = false;

    // --- Actions ------------------------------------------------------------

    /** Regenerates the terrain with the current settings (async; editor stays responsive). */
    UFUNCTION(CallInEditor, BlueprintCallable, Category = "Titan")
    void GenerateTerrain();

    /** Picks a fresh random seed, then regenerates. */
    UFUNCTION(CallInEditor, BlueprintCallable, Category = "Titan")
    void RandomizeSeed();

    /** Rebuilds the mesh section with collision enabled. */
    UFUNCTION(CallInEditor, BlueprintCallable, Category = "Titan")
    void FinalizeCollision();

    // --- TitanLab project ---------------------------------------------------

    /** A .titan project saved from TitanLab. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Titan|Project",
              meta = (FilePathFilter = "titan", RelativeToGameDir))
    FFilePath ProjectFile;

    /**
     * Loads Project File into this actor's settings and regenerates.
     *
     * Reproduces the project's base terrain exactly — seed, world size, noise
     * structure and shape are all carried across. The layer stack is only
     * partly understood: this actor has three erosion toggles where TitanLab
     * has an ordered stack of any length, so erosion layers are matched to
     * those toggles and everything else is reported as skipped rather than
     * silently dropped. Import Report says exactly what happened.
     */
    UFUNCTION(CallInEditor, BlueprintCallable, Category = "Titan|Project")
    void ImportProject();

    /** What the last import did, including anything it could not reproduce. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Titan|Project")
    FString ImportReport;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Titan")
    TObjectPtr<UProceduralMeshComponent> ProceduralMesh;

protected:
    virtual void BeginPlay() override;

private:
    struct FTitanMeshData
    {
        TArray<FVector> Vertices;
        TArray<int32> Triangles;
        TArray<FVector> Normals;
        TArray<FVector2D> UVs;
        TArray<FColor> VertexColors;
    };

    /**
     * A heightfield on its way to a Landscape: already resampled to a valid
     * landscape vertex count and quantised to uint16, because both of those
     * are worth doing off the game thread.
     */
    struct FTitanHeightField
    {
        TArray<uint16> Heights;
        int32 VertsPerSide = 0;
        int32 SubsectionSizeQuads = 0;
        int32 NumSubsections = 0;
        /** Actor scale that makes the quantised data span the intended world. */
        FVector Scale = FVector::OneVector;
        /**
         * Z to spawn the landscape at so its lowest point sits where the
         * procedural mesh's lowest point would. Landscape treats 32768 as its
         * zero plane, so without this the terrain straddles the actor —
         * half of it buried, half in the air.
         */
        double BaseOffsetCm = 0.0;
    };

    void RunGeneration(bool bWithCollision);
    void ApplyMesh(TSharedPtr<FTitanMeshData> Data, bool bWithCollision);
    void ApplyLandscape(TSharedPtr<FTitanHeightField> Field);

    /**
     * The Landscape this actor last built, so regenerating replaces it rather
     * than stacking a new one on top.
     *
     * Visible rather than a bare UPROPERTY: a bare one is invisible to
     * Blueprint and to Python, so neither the editor test nor a user could ask
     * the actor what it had produced — which read as "the landscape was never
     * built" when it had been.
     */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Titan",
              meta = (AllowPrivateAccess = "true"))
    TObjectPtr<AActor> SpawnedLandscape;

    std::atomic<int32> GenerationCounter{0};
};
