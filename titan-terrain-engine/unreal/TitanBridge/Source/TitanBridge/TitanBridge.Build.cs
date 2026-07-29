// TitanBridge module rules: links the prebuilt libTitanCore static library.
// The library is pure C++20 with a C ABI surface — no engine types cross the
// boundary, so there are no allocator or libc++ version hazards.

using System.IO;
using UnrealBuildTool;

public class TitanBridge : ModuleRules
{
    public TitanBridge(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        CppStandard = CppStandardVersion.Cpp20;

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "ProceduralMeshComponent"
        });

        string ThirdParty = Path.Combine(ModuleDirectory, "..", "..", "ThirdParty", "TitanCore");

        // Prefer the engine's real headers when building inside the Titan
        // repo, and fall back to the packaged copy in a shipped plugin (which
        // cannot reach cpp/). The copy exists only for packaging, and it had
        // already drifted 123 lines out of date in TitanCAPI.h — CI enforces
        // that the two match, but preferring the source here means an
        // in-repo build can never compile against a stale declaration.
        string EngineHeaders = Path.Combine(ModuleDirectory,
            "..", "..", "..", "..", "cpp", "libTitanCore", "include");
        PublicIncludePaths.Add(Directory.Exists(EngineHeaders)
            ? EngineHeaders
            : Path.Combine(ThirdParty, "include"));

        if (Target.Platform == UnrealTargetPlatform.Mac)
        {
            string Lib = Path.Combine(ThirdParty, "lib", "Mac", "libTitanCore.a");
            if (File.Exists(Lib))
            {
                PublicAdditionalLibraries.Add(Lib);
            }
            else
            {
                // Fail the build rather than link nothing: a module that
                // silently omits the engine produces unresolved symbols much
                // later, with a far less obvious message.
                throw new BuildException(
                    "TitanBridge: missing " + Lib +
                    ". Build it with: cd cpp && cmake -B build -DCMAKE_BUILD_TYPE=Release " +
                    "&& cmake --build build --target TitanCore, then copy " +
                    "build/libTitanCore.a into ThirdParty/TitanCore/lib/Mac/.");
            }
        }
        else if (Target.Platform == UnrealTargetPlatform.Win64)
        {
            string Lib = Path.Combine(ThirdParty, "lib", "Win64", "TitanCore.lib");
            if (File.Exists(Lib))
            {
                PublicAdditionalLibraries.Add(Lib);
            }
            else
            {
                throw new BuildException(
                    "TitanBridge: missing " + Lib +
                    ". Build it with CMake/MSVC and copy TitanCore.lib into " +
                    "ThirdParty/TitanCore/lib/Win64/ (see the README there). " +
                    "CI publishes this library as a build artifact.");
            }
        }
    }
}
