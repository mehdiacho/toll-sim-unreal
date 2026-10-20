using UnrealBuildTool;

public class TollSimUETarget : TargetRules
{
	public TollSimUETarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("TollSimUE");
	}
}
