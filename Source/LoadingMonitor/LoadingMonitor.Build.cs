using UnrealBuildTool;

public class LoadingMonitor : ModuleRules
{
    public LoadingMonitor(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core"
        });

        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "CoreUObject",
            "Engine",
            "Slate",
            "SlateCore",
            "MoviePlayer",
            "Projects",
            "DeveloperSettings",
            "UMG"
        });

        // Keep the default loading image available in packaged builds.
        RuntimeDependencies.Add("$(PluginDir)/Resources/LoadingBackground.png");
    }
}
