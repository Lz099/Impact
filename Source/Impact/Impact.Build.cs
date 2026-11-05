using UnrealBuildTool;

public class Impact : ModuleRules
{
	public Impact(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] {
			"Core",
			"CoreUObject",
			"Engine",
			"InputCore",
			"EnhancedInput",
			"GameplayTags",
			"AIModule",
			"NavigationSystem",
			"PhysicsCore",
			"Slate",
			"SlateCore",
			"UMG",
			"Json",
			"AnimGraphRuntime",
			"AnimationCore",
			"ALS",
			"ALSCamera",
			"RHI"
		});

		if (Target.bBuildEditor)
		{
			PrivateDependencyModuleNames.AddRange(new string[] {
				"UnrealEd", "AnimGraph", "BlueprintGraph", "AssetRegistry"
			});
		}
	}
}
