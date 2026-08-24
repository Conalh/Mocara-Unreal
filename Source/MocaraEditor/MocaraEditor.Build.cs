using UnrealBuildTool;

public class MocaraEditor : ModuleRules
{
	public MocaraEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"InputCore",
			"Slate",
			"SlateCore"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"UnrealEd",
			"EditorFramework",
			"EditorStyle",
			"EditorWidgets",
			"ToolMenus",
			"WorkspaceMenuStructure",
			"HTTP",
			"Json",
			"JsonUtilities",
			"AssetRegistry",
			"AssetTools",
			"AnimationCore",
			"AnimGraph",
			"AnimationBlueprintLibrary",
			"AnimGraphRuntime",
			"SkeletalMeshDescription",
			"MeshDescription",
			"StaticMeshDescription",
			"SkeletalMeshUtilitiesCommon",
			"IKRig",
			"IKRigEditor",
			"AdvancedPreviewScene",
			"Persona",
			"DeveloperSettings",
			"Projects",
			"DesktopPlatform",
			"RenderCore",
			"RHI",
			"ApplicationCore"
		});
	}
}
