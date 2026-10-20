using UnrealBuildTool;

public class TollSimUE : ModuleRules
{
	public TollSimUE(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine" });
		PrivateDependencyModuleNames.AddRange(new[] { "Json", "HTTP", "ProceduralMeshComponent", "Projects" });
	}
}
