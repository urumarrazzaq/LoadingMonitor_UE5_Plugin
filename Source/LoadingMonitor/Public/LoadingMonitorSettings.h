// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "Framework/Text/TextLayout.h"
#include "Layout/Margin.h"
#include "LoadingMonitorSettings.generated.h"

class UTexture2D;
class UUserWidget;

/** How the status panel (throbber/progress/status line) is laid out on screen. */
UENUM(BlueprintType)
enum class ELoadingPanelLayoutMode : uint8
{
	/** A floating rounded card, positioned/sized by PanelHorizontalAlignment, PanelVerticalAlignment, PanelWidth and PanelMargin. */
	FloatingCard,

	/** A bar spanning the full screen width, pinned to the bottom edge - the progress bar itself runs edge to edge along the very bottom, with the throbber/status/percentage row sitting just above it. */
	FullWidthBottomBar
};

/** A named color scheme applied to the "brand" properties (fill/strip/throbber/panel/status text) in one click - see the Apply buttons under Presets. */
UENUM(BlueprintType)
enum class ELoadingMonitorPreset : uint8
{
	CyanOps,
	EmberAlert,
	ToxicSignal,
	VioletCore,
	SolarGold,
	SignalRed
};

/**
 * Project Settings > Plugins > Loading Monitor.
 *
 * Everything the loading screen shows - which background images it cycles through, the game
 * title's text style and placement, the status panel's layout/colors, and the progress bar - is
 * read from here each time a map load starts. Nothing about the look is hardcoded in LoadingMonitor.cpp.
 */
UCLASS(config = Game, defaultconfig, meta = (DisplayName = "Loading Monitor"))
class LOADINGMONITOR_API ULoadingMonitorSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	ULoadingMonitorSettings();

	virtual FName GetCategoryName() const override { return TEXT("Plugins"); }

	// ------------------------------------------------------------
	// Preview & presets
	// ------------------------------------------------------------

#if WITH_EDITOR
	/** Opens a real, live window showing exactly what the loading screen will look like with the settings below - same widget, same rotating images, same fonts/colors, driven by simulated progress instead of a real map load. If Use Custom Widget is on, this instead opens your assigned Widget Blueprint directly (statically - see that property's tooltip for why). No need to actually trigger a freeze to check how a change looks. */
	UFUNCTION(CallInEditor, Category = "Preview", meta = (DisplayName = "Open Live Preview"))
	void PreviewLoadingScreen();

	/** Sets ProgressBarFillColor / AccentStripColor / ThrobberColor / PanelBackgroundColor / StatusTextColor to this scheme. GameName/percentage text stay pure white on purpose - that's what keeps every preset readable. */
	UFUNCTION(CallInEditor, Category = "Presets", meta = (DisplayName = "Apply: Cyan Ops (default)", EditCondition = "!bUseCustomWidget"))
	void ApplyPreset_CyanOps();

	UFUNCTION(CallInEditor, Category = "Presets", meta = (DisplayName = "Apply: Ember Alert", EditCondition = "!bUseCustomWidget"))
	void ApplyPreset_EmberAlert();

	UFUNCTION(CallInEditor, Category = "Presets", meta = (DisplayName = "Apply: Toxic Signal", EditCondition = "!bUseCustomWidget"))
	void ApplyPreset_ToxicSignal();

	UFUNCTION(CallInEditor, Category = "Presets", meta = (DisplayName = "Apply: Violet Core", EditCondition = "!bUseCustomWidget"))
	void ApplyPreset_VioletCore();

	UFUNCTION(CallInEditor, Category = "Presets", meta = (DisplayName = "Apply: Solar Gold", EditCondition = "!bUseCustomWidget"))
	void ApplyPreset_SolarGold();

	UFUNCTION(CallInEditor, Category = "Presets", meta = (DisplayName = "Apply: Signal Red", EditCondition = "!bUseCustomWidget"))
	void ApplyPreset_SignalRed();

	/** Applies one of the named color schemes above without needing the individual buttons - handy for calling from a Python/editor-utility script. */
	void ApplyPreset(ELoadingMonitorPreset Preset);
#endif

	// ------------------------------------------------------------
	// General
	// ------------------------------------------------------------

	/**
	 * Which levels show this loading screen at all. Empty (default) = every level load shows it, same
	 * as before. Add entries to restrict it to just those levels - match either the short map name
	 * (e.g. "MainMap") or the full path (e.g. "/Game/Maps/MainMap"), case-insensitive. Applies
	 * regardless of whether you're using the built-in panel or a custom widget below.
	 */
	UPROPERTY(config, EditAnywhere, Category = "General")
	TArray<FString> AllowedLevelNames;

	/**
	 * Keeps the loading screen up for at least this many seconds after the level actually finishes
	 * loading, instead of it vanishing the instant loading completes (which can look like a flash if
	 * the load happened to be fast). The progress bar finishes climbing to 100% right as loading
	 * completes (see ProgressBarSmoothingSpeed) rather than jumping straight to the level opening -
	 * this is the window that finish is actually visible in, so 0 means you likely won't see it at
	 * all. Has no effect when Use Custom Widget is on - a custom widget isn't run through this timing
	 * system at all.
	 */
	UPROPERTY(config, EditAnywhere, Category = "General", meta = (ClampMin = "0.0", UIMin = "0.0", UIMax = "5.0", EditCondition = "!bUseCustomWidget"))
	float MinimumDisplayTimeSeconds = 0.6f;

	// ------------------------------------------------------------
	// Background images
	// ------------------------------------------------------------

	/** Textures to cycle through while loading - pick any UTexture2D straight from the Content Browser (e.g. Content/Assets/LoadingImages/...). Leave empty to fall back to the plugin's bundled Resources/LoadingBackground.png. */
	UPROPERTY(config, EditAnywhere, Category = "Background", meta = (EditCondition = "!bUseCustomWidget"))
	TArray<TSoftObjectPtr<UTexture2D>> BackgroundImages;

	/** How long (seconds) each background image stays up before switching to the next one. Only matters if there's more than one image. */
	UPROPERTY(config, EditAnywhere, Category = "Background", meta = (ClampMin = "1.0", UIMin = "1.0", EditCondition = "!bUseCustomWidget"))
	float BackgroundImageIntervalSeconds = 8.0f;

	/** How long (seconds) the crossfade between two images takes. Must be shorter than BackgroundImageIntervalSeconds - set to 0 for a hard cut. */
	UPROPERTY(config, EditAnywhere, Category = "Background", meta = (ClampMin = "0.0", UIMin = "0.0", UIMax = "4.0", EditCondition = "!bUseCustomWidget"))
	float BackgroundFadeDurationSeconds = 1.2f;

	/** If true, a fresh shuffled order is picked each map load (never repeating the same image twice in a row). If false, images cycle through BackgroundImages in list order. */
	UPROPERTY(config, EditAnywhere, Category = "Background", meta = (EditCondition = "!bUseCustomWidget"))
	bool bRandomizeBackgroundOrder = true;

	/** Darkening overlay drawn on top of the background image so text stays readable no matter which image is showing. Alpha controls strength. */
	UPROPERTY(config, EditAnywhere, Category = "Background", meta = (EditCondition = "!bUseCustomWidget"))
	FLinearColor ScreenDarkenColor = FLinearColor(0.0f, 0.0f, 0.0f, 0.32f);

	// ------------------------------------------------------------
	// Game title
	// ------------------------------------------------------------

	/** Displayed as the loading screen's title/logo text. Leave empty to hide it entirely. */
	UPROPERTY(config, EditAnywhere, Category = "Game Title", meta = (EditCondition = "!bUseCustomWidget"))
	FText GameName;

	UPROPERTY(config, EditAnywhere, Category = "Game Title", meta = (ClampMin = "8", ClampMax = "200", UIMin = "8", UIMax = "120", EditCondition = "!bUseCustomWidget"))
	int32 GameNameFontSize = 44;

	UPROPERTY(config, EditAnywhere, Category = "Game Title", meta = (EditCondition = "!bUseCustomWidget"))
	bool bGameNameBold = true;

	UPROPERTY(config, EditAnywhere, Category = "Game Title", meta = (EditCondition = "!bUseCustomWidget"))
	FLinearColor GameNameColor = FLinearColor::White;

	/** Where the title sits on screen - independent of the status panel below, e.g. a big title top-center with the panel still bottom-left. */
	UPROPERTY(config, EditAnywhere, Category = "Game Title", meta = (EditCondition = "!bUseCustomWidget"))
	TEnumAsByte<EHorizontalAlignment> GameNameHorizontalAlignment = HAlign_Left;

	UPROPERTY(config, EditAnywhere, Category = "Game Title", meta = (EditCondition = "!bUseCustomWidget"))
	TEnumAsByte<EVerticalAlignment> GameNameVerticalAlignment = VAlign_Top;

	/** Text justification within its own line - matters once the title wraps, or when the horizontal alignment above is set to Fill. */
	UPROPERTY(config, EditAnywhere, Category = "Game Title", meta = (EditCondition = "!bUseCustomWidget"))
	TEnumAsByte<ETextJustify::Type> GameNameJustification = ETextJustify::Left;

	UPROPERTY(config, EditAnywhere, Category = "Game Title", meta = (ClampMin = "0.0", EditCondition = "!bUseCustomWidget"))
	float GameNameMargin = 48.0f;

	// ------------------------------------------------------------
	// Status log
	// ------------------------------------------------------------

	/** Number of most-recent status lines kept on screen. 1 (the default) shows only the latest message in place instead of stacking a scrolling history. */
	UPROPERTY(config, EditAnywhere, Category = "Status Log", meta = (ClampMin = "1", ClampMax = "12", EditCondition = "!bUseCustomWidget"))
	int32 NumVisibleLogLines = 1;

	UPROPERTY(config, EditAnywhere, Category = "Status Log", meta = (ClampMin = "6", ClampMax = "48", EditCondition = "!bUseCustomWidget"))
	int32 StatusFontSize = 13;

	UPROPERTY(config, EditAnywhere, Category = "Status Log", meta = (EditCondition = "!bUseCustomWidget"))
	FLinearColor StatusTextColor = FLinearColor(0.905882f, 0.917647f, 0.941176f, 1.0f);

	UPROPERTY(config, EditAnywhere, Category = "Status Log", meta = (EditCondition = "!bUseCustomWidget"))
	bool bStatusTextBold = false;

	/**
	 * Off (default): status lines stay generic - "Preparing Textures", nothing else. On: the specific
	 * asset name is appended - "Preparing Textures: T_Character_Diffuse", "Building Distance Fields:
	 * SM_Rock_01", etc. More informative, but the line changes as fast as the engine is actually
	 * naming assets internally, which during a heavy compile burst can be several times a second -
	 * expect it to feel busy rather than calm.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Status Log", meta = (EditCondition = "!bUseCustomWidget"))
	bool bShowDetailedAssetNames = false;

	// ------------------------------------------------------------
	// Status messages
	// ------------------------------------------------------------
	// What actually gets displayed for each kind of engine activity this plugin detects, instead of
	// the raw, technical log line (asset paths, verbosity tags, etc). Reword or localize freely -
	// whatever's here is shown verbatim. The Preview button above cycles through all of these so you
	// can check wording/length without waiting for a real compile of that type to happen.

	UPROPERTY(config, EditAnywhere, Category = "Status Messages", meta = (EditCondition = "!bUseCustomWidget"))
	FText Message_LoadingMap = FText::FromString(TEXT("Loading Map"));

	UPROPERTY(config, EditAnywhere, Category = "Status Messages", meta = (EditCondition = "!bUseCustomWidget"))
	FText Message_CompilingShaders = FText::FromString(TEXT("Compiling Shaders"));

	UPROPERTY(config, EditAnywhere, Category = "Status Messages", meta = (EditCondition = "!bUseCustomWidget"))
	FText Message_PreparingTextures = FText::FromString(TEXT("Preparing Textures"));

	UPROPERTY(config, EditAnywhere, Category = "Status Messages", meta = (EditCondition = "!bUseCustomWidget"))
	FText Message_PreparingStaticMeshes = FText::FromString(TEXT("Preparing Static Meshes"));

	UPROPERTY(config, EditAnywhere, Category = "Status Messages", meta = (EditCondition = "!bUseCustomWidget"))
	FText Message_BuildingDistanceFields = FText::FromString(TEXT("Building Distance Fields"));

	UPROPERTY(config, EditAnywhere, Category = "Status Messages", meta = (EditCondition = "!bUseCustomWidget"))
	FText Message_CompilingParticleSystems = FText::FromString(TEXT("Compiling Particle Systems"));

	UPROPERTY(config, EditAnywhere, Category = "Status Messages", meta = (EditCondition = "!bUseCustomWidget"))
	FText Message_StreamingWorldData = FText::FromString(TEXT("Streaming World Data"));

	UPROPERTY(config, EditAnywhere, Category = "Status Messages", meta = (EditCondition = "!bUseCustomWidget"))
	FText Message_FetchingCachedData = FText::FromString(TEXT("Fetching Cached Data"));

	UPROPERTY(config, EditAnywhere, Category = "Status Messages", meta = (EditCondition = "!bUseCustomWidget"))
	FText Message_FinishingWorldSetup = FText::FromString(TEXT("Finishing World Setup"));

	/** Shown once progress crosses AlmostThereThresholdPercent (Progress Bar category below) - a reassuring final beat instead of the last real status line sitting there for however long the tail end of loading takes. */
	UPROPERTY(config, EditAnywhere, Category = "Status Messages", meta = (EditCondition = "!bUseCustomWidget"))
	FText Message_AlmostThere = FText::FromString(TEXT("Almost there, preparing things..."));

	// ------------------------------------------------------------
	// Custom messages
	// ------------------------------------------------------------
	// A separate rotating line of your own flavor text - "tips"/branding copy, not tied to any real
	// engine activity - shown above the technical status line, crossfading between entries exactly
	// like the background images do. Turn off Status Log's panel entry (NumVisibleLogLines can stay,
	// it just won't be the only thing shown) if you'd rather these replace it outright.

	/** Master switch - off by default so existing setups are unaffected until you turn it on. */
	UPROPERTY(config, EditAnywhere, Category = "Custom Messages", meta = (EditCondition = "!bUseCustomWidget"))
	bool bShowCustomMessages = false;

	/** One entry is shown at a time, crossfading to the next every CustomMessageIntervalSeconds. Add as many as you like. */
	UPROPERTY(config, EditAnywhere, Category = "Custom Messages", meta = (EditCondition = "!bUseCustomWidget"))
	TArray<FText> CustomMessages;

	UPROPERTY(config, EditAnywhere, Category = "Custom Messages", meta = (ClampMin = "0.5", UIMin = "0.5", EditCondition = "!bUseCustomWidget"))
	float CustomMessageIntervalSeconds = 3.0f;

	/** How long the crossfade between two messages takes. 0 = hard cut. */
	UPROPERTY(config, EditAnywhere, Category = "Custom Messages", meta = (ClampMin = "0.0", UIMin = "0.0", UIMax = "2.0", EditCondition = "!bUseCustomWidget"))
	float CustomMessageFadeDurationSeconds = 0.6f;

	/** If true, a fresh shuffled order is picked each map load (never repeating the same message twice in a row). If false, messages cycle through CustomMessages in list order. */
	UPROPERTY(config, EditAnywhere, Category = "Custom Messages", meta = (EditCondition = "!bUseCustomWidget"))
	bool bRandomizeCustomMessageOrder = true;

	UPROPERTY(config, EditAnywhere, Category = "Custom Messages", meta = (ClampMin = "6", ClampMax = "48", EditCondition = "!bUseCustomWidget"))
	int32 CustomMessageFontSize = 14;

	UPROPERTY(config, EditAnywhere, Category = "Custom Messages", meta = (EditCondition = "!bUseCustomWidget"))
	FLinearColor CustomMessageColor = FLinearColor(0.92f, 0.93f, 0.96f, 1.0f);

	UPROPERTY(config, EditAnywhere, Category = "Custom Messages", meta = (EditCondition = "!bUseCustomWidget"))
	bool bCustomMessageItalic = true;

	/** Left/center/right alignment of the message within the panel's full content width. */
	UPROPERTY(config, EditAnywhere, Category = "Custom Messages", meta = (EditCondition = "!bUseCustomWidget"))
	TEnumAsByte<ETextJustify::Type> CustomMessageJustification = ETextJustify::Left;

	// ------------------------------------------------------------
	// Progress bar
	// ------------------------------------------------------------

	UPROPERTY(config, EditAnywhere, Category = "Progress Bar", meta = (EditCondition = "!bUseCustomWidget"))
	bool bShowProgressBar = true;

	UPROPERTY(config, EditAnywhere, Category = "Progress Bar", meta = (EditCondition = "!bUseCustomWidget"))
	bool bShowPercentageText = true;

	/**
	 * The real underlying data (how many assets are still compiling) arrives in chunks, not a smooth
	 * stream - without this, the bar visibly jumps whenever a batch finishes. This is how quickly the
	 * displayed value eases toward the real one instead of snapping to it. Higher = snappier/more
	 * responsive, lower = smoother/more gradual. This only smooths what's *displayed* - the underlying
	 * progress calculation and the percentage text are unaffected.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Progress Bar", meta = (ClampMin = "0.5", ClampMax = "20.0", EditCondition = "!bUseCustomWidget"))
	float ProgressBarSmoothingSpeed = 4.0f;

	/** Once the (smoothed) percentage crosses this, Message_AlmostThere replaces whatever status line was showing - fires once per load. */
	UPROPERTY(config, EditAnywhere, Category = "Progress Bar", meta = (ClampMin = "50.0", ClampMax = "99.0", EditCondition = "!bUseCustomWidget"))
	float AlmostThereThresholdPercent = 90.0f;

	UPROPERTY(config, EditAnywhere, Category = "Progress Bar", meta = (EditCondition = "!bUseCustomWidget"))
	FLinearColor ProgressBarFillColor = FLinearColor(0.231373f, 0.556863f, 0.964706f, 1.0f);

	/** The unfilled part ("track") of the bar. */
	UPROPERTY(config, EditAnywhere, Category = "Progress Bar", meta = (EditCondition = "!bUseCustomWidget"))
	FLinearColor ProgressBarBackgroundColor = FLinearColor(1.0f, 1.0f, 1.0f, 0.08f);

	UPROPERTY(config, EditAnywhere, Category = "Progress Bar", meta = (ClampMin = "2.0", ClampMax = "40.0", EditCondition = "!bUseCustomWidget"))
	float ProgressBarHeight = 8.0f;

	/** 0 = square corners, Height/2 = a full pill/capsule shape. Forced to 0 automatically in Full Width Bottom Bar layout mode, where the bar runs flush to the screen edges. */
	UPROPERTY(config, EditAnywhere, Category = "Progress Bar", meta = (ClampMin = "0.0", ClampMax = "20.0", EditCondition = "!bUseCustomWidget"))
	float ProgressBarCornerRadius = 4.0f;

	UPROPERTY(config, EditAnywhere, Category = "Progress Bar", meta = (EditCondition = "!bUseCustomWidget"))
	FLinearColor PercentageTextColor = FLinearColor::White;

	UPROPERTY(config, EditAnywhere, Category = "Progress Bar", meta = (ClampMin = "6", ClampMax = "48", EditCondition = "!bUseCustomWidget"))
	int32 PercentageFontSize = 13;

	/** Puts the percentage next to the throbber (left side) instead of at the far end of the info row. */
	UPROPERTY(config, EditAnywhere, Category = "Progress Bar", meta = (EditCondition = "!bUseCustomWidget"))
	bool bPercentageOnLeft = false;

	// ------------------------------------------------------------
	// Throbber & accent
	// ------------------------------------------------------------

	UPROPERTY(config, EditAnywhere, Category = "Throbber", meta = (EditCondition = "!bUseCustomWidget"))
	bool bShowThrobber = true;

	UPROPERTY(config, EditAnywhere, Category = "Throbber", meta = (EditCondition = "!bUseCustomWidget"))
	FLinearColor ThrobberColor = FLinearColor(0.231373f, 0.556863f, 0.964706f, 1.0f);

	UPROPERTY(config, EditAnywhere, Category = "Throbber", meta = (ClampMin = "4.0", ClampMax = "60.0", EditCondition = "!bUseCustomWidget"))
	float ThrobberSize = 14.0f;

	/** Puts the throbber on the right end of the info row instead of the left. */
	UPROPERTY(config, EditAnywhere, Category = "Throbber", meta = (EditCondition = "!bUseCustomWidget"))
	bool bThrobberOnRight = false;

	/** Vertical alignment of the throbber/status text/percentage within the info row (Full Width Bottom Bar), or of the throbber/progress row within the card (Floating Card). */
	UPROPERTY(config, EditAnywhere, Category = "Throbber", meta = (EditCondition = "!bUseCustomWidget"))
	TEnumAsByte<EVerticalAlignment> InfoRowVerticalAlignment = VAlign_Center;

	/** A colored accent line - along the panel's left edge in Floating Card mode, or as a thin top border in Full Width Bottom Bar mode. */
	UPROPERTY(config, EditAnywhere, Category = "Throbber", meta = (EditCondition = "!bUseCustomWidget"))
	bool bShowAccentStrip = true;

	UPROPERTY(config, EditAnywhere, Category = "Throbber", meta = (EditCondition = "!bUseCustomWidget"))
	FLinearColor AccentStripColor = FLinearColor(0.231373f, 0.556863f, 0.964706f, 1.0f);

	UPROPERTY(config, EditAnywhere, Category = "Throbber", meta = (ClampMin = "1.0", ClampMax = "20.0", EditCondition = "!bUseCustomWidget"))
	float AccentStripWidth = 4.0f;

	// ------------------------------------------------------------
	// Status panel layout
	// ------------------------------------------------------------

	/** Turn off to hide the whole status panel/bar - e.g. if you only want the background image and game title. */
	UPROPERTY(config, EditAnywhere, Category = "Panel Layout", meta = (EditCondition = "!bUseCustomWidget"))
	bool bShowStatusPanel = true;

	UPROPERTY(config, EditAnywhere, Category = "Panel Layout", meta = (EditCondition = "!bUseCustomWidget"))
	ELoadingPanelLayoutMode PanelLayoutMode = ELoadingPanelLayoutMode::FullWidthBottomBar;

	/** Only used in Floating Card mode. */
	UPROPERTY(config, EditAnywhere, Category = "Panel Layout", meta = (EditCondition = "!bUseCustomWidget && PanelLayoutMode == ELoadingPanelLayoutMode::FloatingCard"))
	TEnumAsByte<EHorizontalAlignment> PanelHorizontalAlignment = HAlign_Left;

	/** Only used in Floating Card mode - Full Width Bottom Bar is always pinned to the bottom. */
	UPROPERTY(config, EditAnywhere, Category = "Panel Layout", meta = (EditCondition = "!bUseCustomWidget && PanelLayoutMode == ELoadingPanelLayoutMode::FloatingCard"))
	TEnumAsByte<EVerticalAlignment> PanelVerticalAlignment = VAlign_Bottom;

	/** Only used in Floating Card mode - Full Width Bottom Bar always spans the screen. */
	UPROPERTY(config, EditAnywhere, Category = "Panel Layout", meta = (ClampMin = "300.0", EditCondition = "!bUseCustomWidget && PanelLayoutMode == ELoadingPanelLayoutMode::FloatingCard"))
	float PanelWidth = 640.0f;

	/** Distance from the screen edge to the panel (Floating Card: all 4 sides; Full Width Bottom Bar: bottom edge only, left/right are always flush). */
	UPROPERTY(config, EditAnywhere, Category = "Panel Layout", meta = (ClampMin = "0.0", EditCondition = "!bUseCustomWidget"))
	float PanelMargin = 0.0f;

	/** Space between the panel's edge and its content (throbber/progress/text), on all four sides. */
	UPROPERTY(config, EditAnywhere, Category = "Panel Layout", meta = (EditCondition = "!bUseCustomWidget"))
	FMargin PanelPadding = FMargin(22.0f, 16.0f, 22.0f, 14.0f);

	/** Vertical space between the throbber/progress row and the status text row (Floating Card), or between that row and the full-width bar beneath it (Full Width Bottom Bar). */
	UPROPERTY(config, EditAnywhere, Category = "Panel Layout", meta = (ClampMin = "0.0", EditCondition = "!bUseCustomWidget"))
	float PanelRowSpacing = 10.0f;

	UPROPERTY(config, EditAnywhere, Category = "Panel Layout", meta = (EditCondition = "!bUseCustomWidget"))
	FLinearColor PanelBackgroundColor = FLinearColor(0.070588f, 0.086275f, 0.121569f, 0.72f);

	/** Only used in Floating Card mode - Full Width Bottom Bar stays square so it sits flush against the screen edges. */
	UPROPERTY(config, EditAnywhere, Category = "Panel Layout", meta = (ClampMin = "0.0", ClampMax = "32.0", EditCondition = "!bUseCustomWidget && PanelLayoutMode == ELoadingPanelLayoutMode::FloatingCard"))
	float PanelCornerRadius = 10.0f;

	/** Thin outline around the panel - set the alpha to 0 (or width to 0) to turn it off. Helps a translucent panel read clearly against any background image. */
	UPROPERTY(config, EditAnywhere, Category = "Panel Layout", meta = (EditCondition = "!bUseCustomWidget"))
	FLinearColor PanelBorderColor = FLinearColor(1.0f, 1.0f, 1.0f, 0.14f);

	UPROPERTY(config, EditAnywhere, Category = "Panel Layout", meta = (ClampMin = "0.0", ClampMax = "8.0", EditCondition = "!bUseCustomWidget"))
	float PanelBorderThickness = 1.0f;

	/** Only used in Floating Card mode - Full Width Bottom Bar sits flush against the screen edge, where a drop shadow has nowhere to fall. */
	UPROPERTY(config, EditAnywhere, Category = "Panel Layout", meta = (EditCondition = "!bUseCustomWidget && PanelLayoutMode == ELoadingPanelLayoutMode::FloatingCard"))
	bool bShowPanelShadow = true;

	UPROPERTY(config, EditAnywhere, Category = "Panel Layout", meta = (EditCondition = "!bUseCustomWidget"))
	FLinearColor PanelShadowColor = FLinearColor(0.0f, 0.0f, 0.0f, 0.45f);

	UPROPERTY(config, EditAnywhere, Category = "Panel Layout", meta = (ClampMin = "0.0", ClampMax = "40.0", EditCondition = "!bUseCustomWidget"))
	float PanelShadowOffset = 6.0f;

	// ------------------------------------------------------------
	// Custom widget (advanced)
	// ------------------------------------------------------------

	/**
	 * Replaces the entire built-in panel above with your own Widget Blueprint. Turns off (greys out)
	 * every setting above, since none of it applies once you're providing your own widget.
	 *
	 * How it's shown is different from the built-in panel, for a concrete reason: the built-in panel
	 * is plain native Slate with no UObject/Blueprint content, which is what lets it safely render on
	 * MoviePlayer's dedicated loading thread during a real blocking load. A Widget Blueprint is a
	 * UObject running actual Blueprint graphs, UMG bindings, and animations - none of which are safe
	 * to execute outside the game thread, and MoviePlayer's separate thread is exactly that. Routing a
	 * UMG widget through it is a real, confirmed crash, not just a theoretical one.
	 *
	 * So instead, your widget is added as a normal, ordinary UMG viewport overlay (AddToViewport) right
	 * before the freeze starts, and removed once the level finishes loading - the same supported way
	 * any UMG widget is shown. Net effect: it displays safely, but like the rest of the UI, it visibly
	 * freezes for the duration of the actual blocking load and resumes (animations, ticks, bindings)
	 * the moment the freeze ends - it does not animate live *through* the freeze. If you need a status
	 * readout that keeps moving during the freeze itself, that's what the built-in panel above is for.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Custom Widget (Advanced)")
	bool bUseCustomWidget = false;

	UPROPERTY(config, EditAnywhere, Category = "Custom Widget (Advanced)", meta = (EditCondition = "bUseCustomWidget"))
	TSoftClassPtr<UUserWidget> CustomWidgetBlueprint;
};
