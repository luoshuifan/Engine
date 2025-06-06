// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class DMXControlConsole : ModuleRules
{
	public DMXControlConsole(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"CoreUObject",
				"DMXGDTF",
				"DMXProtocol",
				"DMXRuntime",
				"Engine",
			}
		);
	}
}
