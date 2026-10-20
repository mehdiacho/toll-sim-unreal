using UnrealBuildTool;

public class TollSimUEEditorTarget : TargetRules
{
	public TollSimUEEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("TollSimUE");
	}
}
