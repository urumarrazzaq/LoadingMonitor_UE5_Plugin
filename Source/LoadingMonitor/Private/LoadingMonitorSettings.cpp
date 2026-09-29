// Fill out your copyright notice in the Description page of Project Settings.

#include "LoadingMonitorSettings.h"

#if WITH_EDITOR
#include "LoadingMonitorPreview.h"
#endif

ULoadingMonitorSettings::ULoadingMonitorSettings()
{
	SectionName = TEXT("Loading Monitor");

	// BackgroundImages starts empty on purpose - LoadingMonitor.cpp falls back to the plugin's
	// bundled Resources/LoadingBackground.png until you pick real textures here.
	GameName = FText::FromString(TEXT("MY GAME"));

	// Populated so flipping bShowCustomMessages on immediately has good copy to show - trim, reorder,
	// or replace freely. bShowCustomMessages itself defaults to false so existing setups are unaffected.
	CustomMessages =
	{
		FText::FromString(TEXT("Wrapping your rewards...")),
		FText::FromString(TEXT("Preparing something special...")),
		FText::FromString(TEXT("Testing your luck...")),
		FText::FromString(TEXT("Spinning up the magic...")),
		FText::FromString(TEXT("Almost there...")),
		FText::FromString(TEXT("Good things are coming...")),
		FText::FromString(TEXT("Unlocking surprises...")),
		FText::FromString(TEXT("The wheel never lies...")),
		FText::FromString(TEXT("Rolling the odds...")),
		FText::FromString(TEXT("Double-checking your luck...")),
		FText::FromString(TEXT("Please wait...")),
		FText::FromString(TEXT("Processing your spin...")),
		FText::FromString(TEXT("Shuffling the deck of fortune...")),
		FText::FromString(TEXT("Counting your lucky stars...")),
		FText::FromString(TEXT("Warming up the jackpot...")),
		FText::FromString(TEXT("Fortune favors the patient...")),
		FText::FromString(TEXT("Charging up your luck...")),
		FText::FromString(TEXT("Loading your next big win...")),
		FText::FromString(TEXT("Polishing the prizes...")),
		FText::FromString(TEXT("Aligning the stars for you...")),
		FText::FromString(TEXT("Great things take a moment...")),
		FText::FromString(TEXT("Calculating the odds in your favor...")),
	};

	// Cyan Ops is the shipped default - ProgressBarFillColor/ThrobberColor/AccentStripColor/
	// PanelBackgroundColor/StatusTextColor above are already set to it directly so the constructor
	// doesn't need to touch config on every fresh load; ApplyPreset(CyanOps) exists purely so the
	// "Apply: Cyan Ops" button can get back here after trying something else.
}

#if WITH_EDITOR

namespace LoadingMonitorPresets
{
	struct FScheme
	{
		FLinearColor Accent;
		FLinearColor PanelBackground;
		FLinearColor StatusText;
	};

	/** sRGB hex -> FLinearColor, the same conversion the color picker's hex field uses - so these match what you'd get by typing the hex in by hand. */
	static FLinearColor Hex(uint8 R, uint8 G, uint8 B)
	{
		return FLinearColor(FColor(R, G, B, 255));
	}

	static FScheme GetScheme(ELoadingMonitorPreset Preset)
	{
		switch (Preset)
		{
		case ELoadingMonitorPreset::EmberAlert:
			return FScheme{ Hex(0xFF, 0x7A, 0x45), Hex(0x1C, 0x15, 0x12), Hex(0xF2, 0xE6, 0xDD) };
		case ELoadingMonitorPreset::ToxicSignal:
			return FScheme{ Hex(0x3D, 0xDC, 0x84), Hex(0x12, 0x1A, 0x15), Hex(0xDF, 0xF2, 0xE6) };
		case ELoadingMonitorPreset::VioletCore:
			return FScheme{ Hex(0xA6, 0x79, 0xFF), Hex(0x17, 0x13, 0x26), Hex(0xEB, 0xE3, 0xF9) };
		case ELoadingMonitorPreset::SolarGold:
			return FScheme{ Hex(0xE8, 0xB8, 0x4B), Hex(0x1C, 0x18, 0x10), Hex(0xF5, 0xED, 0xD9) };
		case ELoadingMonitorPreset::SignalRed:
			return FScheme{ Hex(0xFF, 0x4D, 0x6D), Hex(0x1C, 0x11, 0x14), Hex(0xF6, 0xDF, 0xE4) };
		case ELoadingMonitorPreset::CyanOps:
		default:
			return FScheme{ Hex(0x3B, 0x8E, 0xF6), Hex(0x12, 0x16, 0x1F), Hex(0xE7, 0xEA, 0xF0) };
		}
	}
}

void ULoadingMonitorSettings::ApplyPreset(ELoadingMonitorPreset Preset)
{
	const LoadingMonitorPresets::FScheme Scheme = LoadingMonitorPresets::GetScheme(Preset);

	// The "brand" trio - these three should always read as one color across the whole screen.
	ProgressBarFillColor = Scheme.Accent;
	ThrobberColor = Scheme.Accent;
	AccentStripColor = Scheme.Accent;

	// Panel tint follows the scheme; alpha is left as whatever's already configured so switching
	// presets doesn't fight a translucency choice you've already made.
	PanelBackgroundColor = FLinearColor(Scheme.PanelBackground.R, Scheme.PanelBackground.G, Scheme.PanelBackground.B, PanelBackgroundColor.A);

	StatusTextColor = Scheme.StatusText;

	// GameNameColor and PercentageTextColor are deliberately left alone - pure white reads clearly
	// against every one of these schemes, which is exactly why the reference mockup kept them fixed.

	SaveConfig();
}

void ULoadingMonitorSettings::ApplyPreset_CyanOps() { ApplyPreset(ELoadingMonitorPreset::CyanOps); }
void ULoadingMonitorSettings::ApplyPreset_EmberAlert() { ApplyPreset(ELoadingMonitorPreset::EmberAlert); }
void ULoadingMonitorSettings::ApplyPreset_ToxicSignal() { ApplyPreset(ELoadingMonitorPreset::ToxicSignal); }
void ULoadingMonitorSettings::ApplyPreset_VioletCore() { ApplyPreset(ELoadingMonitorPreset::VioletCore); }
void ULoadingMonitorSettings::ApplyPreset_SolarGold() { ApplyPreset(ELoadingMonitorPreset::SolarGold); }
void ULoadingMonitorSettings::ApplyPreset_SignalRed() { ApplyPreset(ELoadingMonitorPreset::SignalRed); }

void ULoadingMonitorSettings::PreviewLoadingScreen()
{
	OpenLoadingMonitorPreviewWindow();
}

#endif // WITH_EDITOR
