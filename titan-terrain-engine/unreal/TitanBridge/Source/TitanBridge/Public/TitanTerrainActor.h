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

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Titan|Base")
    FString Seed = TEXT("titan");

    /** Grid resolution per side (vertices). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Titan|Base",
              meta = (ClampMin = 64, ClampMax = 1024))
    int32 Resolution = 256;

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

    /** Unreal centimetres per terrain cell. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Titan|Base",
              meta = (ClampMin = "10.0", ClampMax = "10000.0"))
    float UnitsPerCell = 100.0f;

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

    void RunGeneration(bool bWithCollision);
    void ApplyMesh(TSharedPtr<FTitanMeshData> Data, bool bWithCollision);

    std::atomic<int32> GenerationCounter{0};
};
