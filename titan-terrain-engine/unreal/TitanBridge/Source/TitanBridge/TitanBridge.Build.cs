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
        PublicIncludePaths.Add(Path.Combine(ThirdParty, "include"));

        if (Target.Platform == UnrealTargetPlatform.Mac)
        {
            string Lib = Path.Combine(ThirdParty, "lib", "Mac", "libTitanCore.a");
            if (File.Exists(Lib))
            {
                PublicAdditionalLibraries.Add(Lib);
            }
            else
            {
                System.Console.WriteLine("TitanBridge: missing " + Lib + " — build cpp/ with CMake first.");
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
                System.Console.WriteLine("TitanBridge: missing " + Lib + " — build it with CMake/MSVC (see ThirdParty/TitanCore/lib/Win64/README.md).");
            }
        }
    }
}
