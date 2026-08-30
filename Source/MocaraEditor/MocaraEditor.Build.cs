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
			"HTTP",
			"InputCore",
			"Json",
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
