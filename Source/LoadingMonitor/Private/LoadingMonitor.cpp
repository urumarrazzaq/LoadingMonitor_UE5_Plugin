#include "Modules/ModuleManager.h"

#include "CoreGlobals.h"
#include "HAL/CriticalSection.h"
#include "HAL/PlatformTime.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/OutputDevice.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "UObject/UObjectGlobals.h"

#include "MoviePlayer.h"
#include "AssetCompilingManager.h"
#include "LoadingMonitorSettings.h"
#include "LoadingMonitorPreview.h"
#include "Engine/Engine.h"
#include "Engine/Texture2D.h"
#include "UObject/StrongObjectPtr.h"
#include "Blueprint/UserWidget.h"
#include "Containers/Ticker.h"

#include "Brushes/SlateImageBrush.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Framework/Application/SlateApplication.h"
#include "Layout/Clipping.h"
#include "Layout/Margin.h"
#include "Layout/Visibility.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Images/SThrobber.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Notifications/SProgressBar.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/SOverlay.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

namespace LoadingMonitor
{
    static constexpr int32 DefaultMaxVisibleLines = 1;

    // Defined further down this namespace - forward-declared so FLoadingMonitorPreviewSession can use it.
    static UWorld* ResolveAnyValidWorld();

    /**
     * Every kind of engine activity this plugin recognizes. FLoadingLogOutputDevice maps raw,
     * technical log lines onto one of these; what actually gets displayed is the corresponding
     * entry in ULoadingMonitorSettings (e.g. "Compiling Shaders") - never the raw line itself.
     */
    enum class EStatusCategory : uint8
    {
        LoadingMap,
        CompilingShaders,
        PreparingTextures,
        PreparingStaticMeshes,
        BuildingDistanceFields,
        CompilingParticleSystems,
        StreamingWorldData,
        FetchingCachedData,
        FinishingWorldSetup,
        AlmostThere,
        Count
    };

    class FLoadingState final : public TSharedFromThis<FLoadingState, ESPMode::ThreadSafe>
    {
    public:
        void Reset(const FString& MapName)
        {
            FScopeLock ScopeLock(&Mutex);
            Lines.Reset();

            FString ShortName = FPaths::GetBaseFilename(MapName);
            if (ShortName.IsEmpty())
            {
                ShortName = MapName;
            }

            const FString LoadingMapLabel = GetLabel_NoLock(EStatusCategory::LoadingMap);
            Lines.Add(LoadingMapLabel.IsEmpty()
                ? ShortName
                : FString::Printf(TEXT("%s: %s"), *LoadingMapLabel, *ShortName));
            TrimLines();

            TotalCompleted = 0;
            Percent = 0.0f;
            bHasProgress = false;
            bCompleted = false;
            DisplayedPercent = 0.0f;
            LastDisplayUpdateTime = 0.0;
            bAnnouncedAlmostThere = false;
        }

        /** See ULoadingMonitorSettings::AlmostThereThresholdPercent. */
        void SetAlmostThereThreshold(float InThresholdPercent)
        {
            FScopeLock ScopeLock(&Mutex);
            AlmostThereThresholdPercent = FMath::Clamp(InThresholdPercent, 1.0f, 100.0f);
        }

        /** How many of the most-recent lines to keep - from ULoadingMonitorSettings::NumVisibleLogLines. Defaults to 1, i.e. only the latest message is ever shown, nothing stacks. */
        void SetMaxVisibleLines(int32 InMaxVisibleLines)
        {
            FScopeLock ScopeLock(&Mutex);
            MaxVisibleLines = FMath::Max(1, InMaxVisibleLines);
            TrimLines();
        }

        /** Indexed by EStatusCategory - the human-readable label shown for each category, sourced from ULoadingMonitorSettings on the game thread. */
        void SetMessageLabels(const TArray<FString>& InLabels)
        {
            FScopeLock ScopeLock(&Mutex);
            Labels = InLabels;
        }

        /** See ULoadingMonitorSettings::bShowDetailedAssetNames. */
        void SetShowAssetDetail(bool bInShowAssetDetail)
        {
            FScopeLock ScopeLock(&Mutex);
            bShowAssetDetail = bInShowAssetDetail;
        }

        /**
         * Pushes the configured label for Category (not raw log text) - called from
         * FLoadingLogOutputDevice, potentially from any thread, and also used to drive the simulated
         * preview. Detail (e.g. the specific asset name) is appended only when
         * ULoadingMonitorSettings::bShowDetailedAssetNames is on; otherwise the label alone is shown,
         * same as before that option existed.
         */
        void PushCategory(EStatusCategory Category, const FString& Detail = FString())
        {
            FString Label;
            bool bIncludeDetail;
            {
                FScopeLock ScopeLock(&Mutex);
                Label = GetLabel_NoLock(Category);
                bIncludeDetail = bShowAssetDetail;
            }

            if (Label.IsEmpty())
            {
                return;
            }

            if (bIncludeDetail && !Detail.IsEmpty())
            {
                Push(FString::Printf(TEXT("%s: %s"), *Label, *Detail));
            }
            else
            {
                Push(MoveTemp(Label));
            }
        }

        FString GetText() const
        {
            FScopeLock ScopeLock(&Mutex);
            return FString::Join(Lines, TEXT("\n"));
        }

        /**
         * Called from HandleAssetPostCompile (a thread-safe engine delegate - see the module below)
         * whenever a batch of assets finishes compiling - or, in the preview window, from a simulated
         * feed on a normal game-thread ticker. CurrentRemaining is the total in-flight right now
         * across every asset type combined (textures, static meshes, HLODs, ...); NumJustCompleted is
         * how many finished since the last call (0 for a baseline seed call).
         *
         * Percent is cumulative-completed / cumulative-ever-queued, and - deliberately - can only ever
         * go up. The raw ratio genuinely can dip when a new phase (HLODs right as static meshes
         * finish, say) adds a lot of new work relative to what's completed so far; a progress bar that
         * visibly moves backward reads as broken no matter how "honest" the number is, the same way a
         * real download/install bar never does either. So the displayed Percent is clamped to its own
         * highest value so far: a big new phase pauses the climb until enough of it completes to
         * organically pass the previous high point, it never steps backward to show it.
         *
         * No-ops once ForceComplete() has been called (see there for why).
         */
        void UpdateProgress(int32 CurrentRemaining, int32 NumJustCompleted = 0)
        {
            bool bShouldAnnounceAlmostThere = false;

            {
                FScopeLock ScopeLock(&Mutex);

                if (bCompleted)
                {
                    return;
                }

                TotalCompleted += FMath::Max(0, NumJustCompleted);
                const int32 TotalEverQueued = TotalCompleted + FMath::Max(0, CurrentRemaining);

                if (TotalEverQueued > 0)
                {
                    const float RawPercent = 100.0f * (static_cast<float>(TotalCompleted) / static_cast<float>(TotalEverQueued));
                    Percent = FMath::Max(Percent, RawPercent);
                    bHasProgress = true;
                }

                if (!bAnnouncedAlmostThere && bHasProgress && Percent >= AlmostThereThresholdPercent)
                {
                    bAnnouncedAlmostThere = true;
                    bShouldAnnounceAlmostThere = true;
                }
            }

            // Outside the lock above - PushCategory() takes its own lock (see Push()).
            if (bShouldAnnounceAlmostThere)
            {
                PushCategory(EStatusCategory::AlmostThere);
            }
        }

        /**
         * Called once loading has genuinely finished (HandlePostLoadMap), before the level actually
         * opens. The asset-compiling signal this is built on only ever covers part of a real load
         * (actor spawning, BeginPlay, GC, etc all happen too), so Percent reaching exactly 100% right
         * as the level becomes ready is the exception, not the rule - without this, the bar would jump
         * straight from wherever it happened to be (68%, 91%, whatever) to the level just opening.
         * This snaps the target to 100 and locks out any further real updates, so the existing
         * smoothing (ProgressBarSmoothingSpeed) visibly eases the displayed value the rest of the way
         * to 100% instead - a deliberate finishing flourish, not a jump cut. Pair with
         * MinimumDisplayTimeSeconds so there's actually time left on screen for it to be seen.
         */
        void ForceComplete()
        {
            FScopeLock ScopeLock(&Mutex);
            bCompleted = true;
            Percent = 100.0f;
            bHasProgress = true;
        }

        /** How quickly the displayed value eases toward the real one - see ULoadingMonitorSettings::ProgressBarSmoothingSpeed. */
        void SetSmoothingSpeed(float InSpeed)
        {
            FScopeLock ScopeLock(&Mutex);
            SmoothingSpeed = FMath::Max(0.1f, InSpeed);
        }

        /**
         * The real underlying signal (remaining asset-compile count) arrives in discrete jumps
         * whenever a batch finishes, not a smooth stream - returning Percent directly would make the
         * bar visibly snap each time. This eases the *displayed* value toward Percent over time
         * instead, using the real elapsed time between calls, so it animates smoothly no matter how
         * often (or rarely) the widget asks - which, again, can be from any thread.
         */
        float GetPercent() const
        {
            FScopeLock ScopeLock(&Mutex);

            const double Now = FPlatformTime::Seconds();

            if (LastDisplayUpdateTime <= 0.0)
            {
                LastDisplayUpdateTime = Now;
                DisplayedPercent = Percent;
                return DisplayedPercent;
            }

            const float DeltaTime = static_cast<float>(FMath::Clamp(Now - LastDisplayUpdateTime, 0.0, 0.5));
            LastDisplayUpdateTime = Now;

            if (DeltaTime > 0.0f)
            {
                const float Alpha = 1.0f - FMath::Exp(-SmoothingSpeed * DeltaTime);
                DisplayedPercent = FMath::Lerp(DisplayedPercent, Percent, Alpha);

                // Snap once close enough - an exponential ease never quite reaches its target.
                if (FMath::Abs(DisplayedPercent - Percent) < 0.1f)
                {
                    DisplayedPercent = Percent;
                }
            }

            return DisplayedPercent;
        }

        /** False until the first progress sample comes in - lets the widget show an indeterminate state instead of a misleading "0%" before any work has actually been measured. */
        bool HasProgress() const
        {
            FScopeLock ScopeLock(&Mutex);
            return bHasProgress;
        }

    private:
        static constexpr int32 MaxLineLength = 190;

        void Push(FString Line)
        {
            Line.TrimStartAndEndInline();
            if (Line.IsEmpty())
            {
                return;
            }

            if (Line.Len() > MaxLineLength)
            {
                Line = Line.Left(MaxLineLength - 3) + TEXT("...");
            }

            FScopeLock ScopeLock(&Mutex);

            if (Lines.Num() > 0 && Lines.Last() == Line)
            {
                return;
            }

            Lines.Add(MoveTemp(Line));
            TrimLines();
        }

        FString GetLabel_NoLock(EStatusCategory Category) const
        {
            const int32 Index = static_cast<int32>(Category);
            return Labels.IsValidIndex(Index) ? Labels[Index] : FString();
        }

        void TrimLines()
        {
            // Mutex already held by the caller.
            while (Lines.Num() > MaxVisibleLines)
            {
                Lines.RemoveAt(0);
            }
        }

        mutable FCriticalSection Mutex;
        TArray<FString> Lines;
        TArray<FString> Labels;
        int32 MaxVisibleLines = DefaultMaxVisibleLines;

        int32 TotalCompleted = 0;
        float Percent = 0.0f;
        bool bHasProgress = false;
        bool bCompleted = false;
        float SmoothingSpeed = 4.0f;
        mutable float DisplayedPercent = 0.0f;
        mutable double LastDisplayUpdateTime = 0.0;
        float AlmostThereThresholdPercent = 90.0f;
        bool bAnnouncedAlmostThere = false;
        bool bShowAssetDetail = false;
    };

    using FLoadingStatePtr = TSharedPtr<FLoadingState, ESPMode::ThreadSafe>;
    using FLoadingStateWeakPtr = TWeakPtr<FLoadingState, ESPMode::ThreadSafe>;

    /**
     * Pulls a short, readable asset name out of a raw engine log line for
     * ULoadingMonitorSettings::bShowDetailedAssetNames - e.g. ".../Game/Textures/T_Character_Diffuse"
     * becomes "T_Character_Diffuse". Only used when that option is on; the category label alone
     * (never this) is what's shown otherwise.
     */
    static FString ExtractAssetName(const FString& Message)
    {
        FString Result = Message;
        int32 SlashIndex = INDEX_NONE;

        if (Result.FindLastChar(TEXT('/'), SlashIndex))
        {
            Result = Result.Mid(SlashIndex + 1);
        }

        // Strip common trailing wording so what's left reads as just the asset name.
        Result.ReplaceInline(TEXT(" being ready before playing"), TEXT(""));

        int32 DotIndex = INDEX_NONE;
        if (Result.FindChar(TEXT('.'), DotIndex))
        {
            Result = Result.Left(DotIndex);
        }

        Result.TrimStartAndEndInline();
        return Result;
    }

    class FLoadingLogOutputDevice final : public FOutputDevice
    {
    public:
        explicit FLoadingLogOutputDevice(const TSharedRef<FLoadingState, ESPMode::ThreadSafe>& InState)
            : State(InState)
        {
        }

        virtual bool CanBeUsedOnAnyThread() const override
        {
            return true;
        }

        virtual bool CanBeUsedOnMultipleThreads() const override
        {
            return true;
        }

        // UE 5.6 keeps this three-argument overload pure virtual.
        // Implement it explicitly so this output device is concrete.
        virtual void Serialize(
            const TCHAR* V,
            ELogVerbosity::Type Verbosity,
            const FName& Category) override
        {
            Serialize(V, Verbosity, Category, -1.0);
        }

        // Timestamped overload used by threaded log redirection paths.
        virtual void Serialize(
            const TCHAR* V,
            ELogVerbosity::Type Verbosity,
            const FName& Category,
            const double Time) override
        {
            if (V == nullptr)
            {
                return;
            }

            const FString Message(V);

            // Use names that cannot collide with UE's global DECLARE_LOG_CATEGORY symbols.
            static const FName ShaderCompilersCategory(TEXT("LogShaderCompilers"));
            static const FName StaticMeshCategory(TEXT("LogStaticMesh"));
            static const FName TextureCategory(TEXT("LogTexture"));
            static const FName MeshUtilitiesCategory(TEXT("LogMeshUtilities"));
            static const FName NiagaraCategory(TEXT("LogNiagara"));
            static const FName StreamingCategory(TEXT("LogStreaming"));
            static const FName LoadCategory(TEXT("LogLoad"));
            static const FName DerivedDataCacheCategory(TEXT("LogDerivedDataCache"));
            static const FName WorldPartitionCategory(TEXT("LogWorldPartition"));

            const bool bKnownCategory =
                Category == ShaderCompilersCategory ||
                Category == StaticMeshCategory ||
                Category == TextureCategory ||
                Category == MeshUtilitiesCategory ||
                Category == NiagaraCategory ||
                Category == StreamingCategory ||
                Category == LoadCategory ||
                Category == DerivedDataCacheCategory ||
                Category == WorldPartitionCategory;

            const bool bInterestingText =
                Message.Contains(TEXT("Waiting on static mesh"), ESearchCase::IgnoreCase) ||
                Message.Contains(TEXT("Waiting for static meshes"), ESearchCase::IgnoreCase) ||
                Message.Contains(TEXT("Building static mesh"), ESearchCase::IgnoreCase) ||
                Message.Contains(TEXT("Building textures"), ESearchCase::IgnoreCase) ||
                Message.Contains(TEXT("Waiting for textures"), ESearchCase::IgnoreCase) ||
                Message.Contains(TEXT("shader"), ESearchCase::IgnoreCase) ||
                Message.Contains(TEXT("distance field"), ESearchCase::IgnoreCase) ||
                Message.Contains(TEXT("Compiling System"), ESearchCase::IgnoreCase) ||
                Message.Contains(TEXT("flushing async loading"), ESearchCase::IgnoreCase);

            if (!bKnownCategory && !bInterestingText)
            {
                return;
            }

            // Map onto a fixed category - the displayed LABEL comes entirely from
            // ULoadingMonitorSettings (see FLoadingState::PushCategory), never from Message itself.
            // Detail (the specific asset name) is only appended when bShowDetailedAssetNames is on.
            const FString Detail = ExtractAssetName(Message);

            if (Message.Contains(TEXT("texture"), ESearchCase::IgnoreCase) || Category == TextureCategory)
            {
                State->PushCategory(EStatusCategory::PreparingTextures, Detail);
            }
            else if (Message.Contains(TEXT("static mesh"), ESearchCase::IgnoreCase) || Category == StaticMeshCategory)
            {
                State->PushCategory(EStatusCategory::PreparingStaticMeshes, Detail);
            }
            else if (Category == ShaderCompilersCategory || Message.Contains(TEXT("shader"), ESearchCase::IgnoreCase))
            {
                State->PushCategory(EStatusCategory::CompilingShaders, Detail);
            }
            else if (Message.Contains(TEXT("distance field"), ESearchCase::IgnoreCase))
            {
                State->PushCategory(EStatusCategory::BuildingDistanceFields, Detail);
            }
            else if (Category == NiagaraCategory || Message.Contains(TEXT("Compiling System"), ESearchCase::IgnoreCase))
            {
                State->PushCategory(EStatusCategory::CompilingParticleSystems, Detail);
            }
            else if (Category == StreamingCategory || Category == WorldPartitionCategory || Message.Contains(TEXT("flushing async loading"), ESearchCase::IgnoreCase))
            {
                State->PushCategory(EStatusCategory::StreamingWorldData, Detail);
            }
            else if (Category == DerivedDataCacheCategory)
            {
                State->PushCategory(EStatusCategory::FetchingCachedData, Detail);
            }
            else if (Category == LoadCategory || Category == MeshUtilitiesCategory)
            {
                State->PushCategory(EStatusCategory::LoadingMap, Detail);
            }
        }

    private:
        TSharedRef<FLoadingState, ESPMode::ThreadSafe> State;
    };

    /**
     * A plain (non-UObject) snapshot of ULoadingMonitorSettings, taken on the game thread right
     * before the loading screen goes up (or before a preview window opens). The widget below - which
     * runs on MoviePlayer's dedicated Slate loading thread during a real load, not the game thread -
     * only ever reads this snapshot, never the settings UObject itself.
     */
    struct FDisplayConfig
    {
        // Background
        TArray<TSharedPtr<FSlateBrush>> BackgroundBrushes;
        float BackgroundImageIntervalSeconds = 8.0f;
        float BackgroundFadeDurationSeconds = 1.2f;
        bool bRandomizeBackgroundOrder = true;
        FLinearColor ScreenDarkenColor = FLinearColor(0.0f, 0.0f, 0.0f, 0.32f);

        // Game title
        FText GameName;
        int32 GameNameFontSize = 44;
        bool bGameNameBold = true;
        FLinearColor GameNameColor = FLinearColor::White;
        EHorizontalAlignment GameNameHorizontalAlignment = HAlign_Left;
        EVerticalAlignment GameNameVerticalAlignment = VAlign_Top;
        ETextJustify::Type GameNameJustification = ETextJustify::Left;
        float GameNameMargin = 48.0f;

        // Status log
        int32 NumVisibleLogLines = 1;
        int32 StatusFontSize = 13;
        FLinearColor StatusTextColor = FLinearColor(0.9f, 0.92f, 0.94f, 1.0f);
        bool bStatusTextBold = false;
        bool bShowDetailedAssetNames = false;

        // Custom messages
        bool bShowCustomMessages = false;
        TArray<FText> CustomMessages;
        float CustomMessageIntervalSeconds = 3.0f;
        float CustomMessageFadeDurationSeconds = 0.6f;
        bool bRandomizeCustomMessageOrder = true;
        int32 CustomMessageFontSize = 14;
        FLinearColor CustomMessageColor = FLinearColor(0.92f, 0.93f, 0.96f, 1.0f);
        bool bCustomMessageItalic = true;
        ETextJustify::Type CustomMessageJustification = ETextJustify::Left;

        // Progress bar
        bool bShowProgressBar = true;
        bool bShowPercentageText = true;
        float ProgressBarSmoothingSpeed = 4.0f;
        float AlmostThereThresholdPercent = 90.0f;
        FLinearColor ProgressBarFillColor = FLinearColor(0.231373f, 0.556863f, 0.964706f, 1.0f);
        FLinearColor ProgressBarBackgroundColor = FLinearColor(1.0f, 1.0f, 1.0f, 0.08f);
        float ProgressBarHeight = 8.0f;
        float ProgressBarCornerRadius = 4.0f;
        FLinearColor PercentageTextColor = FLinearColor::White;
        int32 PercentageFontSize = 13;
        bool bPercentageOnLeft = false;

        // Throbber & accent
        bool bShowThrobber = true;
        FLinearColor ThrobberColor = FLinearColor(0.231373f, 0.556863f, 0.964706f, 1.0f);
        float ThrobberSize = 14.0f;
        bool bThrobberOnRight = false;
        EVerticalAlignment InfoRowVerticalAlignment = VAlign_Center;
        bool bShowAccentStrip = true;
        FLinearColor AccentStripColor = FLinearColor(0.231373f, 0.556863f, 0.964706f, 1.0f);
        float AccentStripWidth = 4.0f;

        // Panel layout
        bool bShowStatusPanel = true;
        ELoadingPanelLayoutMode PanelLayoutMode = ELoadingPanelLayoutMode::FullWidthBottomBar;
        EHorizontalAlignment PanelHorizontalAlignment = HAlign_Left;
        EVerticalAlignment PanelVerticalAlignment = VAlign_Bottom;
        float PanelWidth = 640.0f;
        float PanelMargin = 0.0f;
        FMargin PanelPadding = FMargin(22.0f, 16.0f, 22.0f, 14.0f);
        float PanelRowSpacing = 10.0f;
        FLinearColor PanelBackgroundColor = FLinearColor(0.07f, 0.086f, 0.12f, 0.72f);
        float PanelCornerRadius = 10.0f;
        FLinearColor PanelBorderColor = FLinearColor(1.0f, 1.0f, 1.0f, 0.14f);
        float PanelBorderThickness = 1.0f;
        bool bShowPanelShadow = true;
        FLinearColor PanelShadowColor = FLinearColor(0.0f, 0.0f, 0.0f, 0.45f);
        float PanelShadowOffset = 6.0f;

        // Indexed by EStatusCategory - handed to FLoadingState::SetMessageLabels.
        TArray<FString> MessageLabels;
    };

    class SLoadingMonitorWidget final : public SCompoundWidget
    {
    public:
        SLATE_BEGIN_ARGS(SLoadingMonitorWidget)
        {
        }
            SLATE_ARGUMENT(FLoadingStatePtr, LoadingState)
            SLATE_ARGUMENT(FDisplayConfig, Config)
        SLATE_END_ARGS()

        void Construct(const FArguments& InArgs)
        {
            LoadingState = InArgs._LoadingState;
            Config = InArgs._Config;
            const FLoadingStateWeakPtr WeakLoadingState = LoadingState;
            const bool bFullWidth = Config.PanelLayoutMode == ELoadingPanelLayoutMode::FullWidthBottomBar;

            StartTime = FPlatformTime::Seconds();
            BuildShuffledOrder();
            BuildBrushes();

            TSharedRef<SWidget> TitleWidget = BuildTitleWidget();

            TSharedRef<SWidget> PanelWidget = Config.bShowStatusPanel
                ? (bFullWidth ? BuildFullWidthBottomBar(WeakLoadingState) : BuildFloatingCard(WeakLoadingState))
                : StaticCastSharedRef<SWidget>(SNullWidget::NullWidget);

            const EHorizontalAlignment PanelHAlign = bFullWidth ? HAlign_Fill : Config.PanelHorizontalAlignment;
            const EVerticalAlignment PanelVAlign = bFullWidth ? VAlign_Bottom : Config.PanelVerticalAlignment;
            const FMargin PanelOuterMargin = bFullWidth
                ? FMargin(0.0f, 0.0f, 0.0f, Config.PanelMargin)
                : FMargin(Config.PanelMargin);

            ChildSlot
            [
                SNew(SOverlay)

                // Solid fallback behind everything, in case no background image loaded.
                + SOverlay::Slot()
                [
                    SNew(SBorder)
                    .BorderImage(FCoreStyle::Get().GetBrush(TEXT("WhiteBrush")))
                    .BorderBackgroundColor(FLinearColor(0.008f, 0.012f, 0.022f, 1.0f))
                ]

                // Crossfading background: outgoing image fades out underneath while the incoming one
                // fades in on top - both computed purely from wall-clock time (see ComputeBackgroundFadeCycle),
                // so it animates smoothly on MoviePlayer's Slate thread with no Tick() override needed.
                + SOverlay::Slot()
                [
                    SNew(SImage)
                    .Image_Lambda([this]() -> const FSlateBrush*
                    {
                        const FFadeCycleState Fade = ComputeBackgroundFadeCycle();
                        return Config.BackgroundBrushes.IsValidIndex(Fade.PrevIndex) ? Config.BackgroundBrushes[Fade.PrevIndex].Get() : nullptr;
                    })
                    .ColorAndOpacity_Lambda([this]() -> FSlateColor
                    {
                        const FFadeCycleState Fade = ComputeBackgroundFadeCycle();
                        return FSlateColor(FLinearColor(1.0f, 1.0f, 1.0f, 1.0f - Fade.FadeAlpha));
                    })
                ]

                + SOverlay::Slot()
                [
                    SNew(SImage)
                    .Image_Lambda([this]() -> const FSlateBrush*
                    {
                        const FFadeCycleState Fade = ComputeBackgroundFadeCycle();
                        return Config.BackgroundBrushes.IsValidIndex(Fade.CurrentIndex) ? Config.BackgroundBrushes[Fade.CurrentIndex].Get() : nullptr;
                    })
                    .ColorAndOpacity_Lambda([this]() -> FSlateColor
                    {
                        const FFadeCycleState Fade = ComputeBackgroundFadeCycle();
                        return FSlateColor(FLinearColor(1.0f, 1.0f, 1.0f, Fade.FadeAlpha));
                    })
                ]

                // Darkening overlay so text stays readable regardless of which image is up.
                + SOverlay::Slot()
                .HAlign(HAlign_Fill)
                .VAlign(VAlign_Fill)
                [
                    SNew(SBorder)
                    .BorderImage(FCoreStyle::Get().GetBrush(TEXT("WhiteBrush")))
                    .BorderBackgroundColor(Config.ScreenDarkenColor)
                ]

                // Game title - placed and styled independently of the status panel below.
                + SOverlay::Slot()
                .HAlign(Config.GameNameHorizontalAlignment)
                .VAlign(Config.GameNameVerticalAlignment)
                .Padding(FMargin(Config.GameNameMargin))
                [
                    TitleWidget
                ]

                // Status panel/bar.
                + SOverlay::Slot()
                .HAlign(PanelHAlign)
                .VAlign(PanelVAlign)
                .Padding(PanelOuterMargin)
                [
                    PanelWidget
                ]
            ];
        }

    private:
        void BuildBrushes()
        {
            const bool bFullWidth = Config.PanelLayoutMode == ELoadingPanelLayoutMode::FullWidthBottomBar;
            const float PanelRadius = bFullWidth ? 0.0f : Config.PanelCornerRadius;
            const float ProgressRadius = bFullWidth ? 0.0f : Config.ProgressBarCornerRadius;

            PanelBrush = MakeShared<FSlateRoundedBoxBrush>(
                Config.PanelBackgroundColor, PanelRadius,
                Config.PanelBorderColor, Config.PanelBorderThickness);

            PanelShadowBrush = MakeShared<FSlateRoundedBoxBrush>(
                Config.PanelShadowColor, PanelRadius);

            ProgressBackgroundBrush = MakeShared<FSlateRoundedBoxBrush>(
                Config.ProgressBarBackgroundColor, ProgressRadius);

            ProgressFillBrush = MakeShared<FSlateRoundedBoxBrush>(
                Config.ProgressBarFillColor, ProgressRadius);
        }

        TSharedRef<SWidget> BuildTitleWidget() const
        {
            if (Config.GameName.IsEmpty())
            {
                return SNullWidget::NullWidget;
            }

            const FSlateFontInfo TitleFont = FCoreStyle::GetDefaultFontStyle(
                Config.bGameNameBold ? TEXT("Bold") : TEXT("Regular"),
                Config.GameNameFontSize);

            return SNew(SOverlay)

                // Cheap drop-shadow: the same text, offset a couple pixels, dark and translucent.
                + SOverlay::Slot()
                .Padding(FMargin(2.0f, 2.0f, 0.0f, 0.0f))
                [
                    SNew(STextBlock)
                    .Text(Config.GameName)
                    .Font(TitleFont)
                    .ColorAndOpacity(FLinearColor(0.0f, 0.0f, 0.0f, 0.55f))
                    .Justification(Config.GameNameJustification)
                ]

                + SOverlay::Slot()
                [
                    SNew(STextBlock)
                    .Text(Config.GameName)
                    .Font(TitleFont)
                    .ColorAndOpacity(Config.GameNameColor)
                    .Justification(Config.GameNameJustification)
                ];
        }

        TSharedRef<SWidget> BuildThrobber() const
        {
            return SNew(SCircularThrobber)
                .Radius(Config.ThrobberSize)
                .NumPieces(8)
                .Period(0.85f)
                .ColorAndOpacity(Config.ThrobberColor);
        }

        TSharedRef<SWidget> BuildStatusText(const FLoadingStateWeakPtr& WeakLoadingState, bool bAllowWrap) const
        {
            return SNew(STextBlock)
                .Text_Lambda([WeakLoadingState]() -> FText
                {
                    const FLoadingStatePtr Pinned = WeakLoadingState.Pin();
                    return Pinned.IsValid() ? FText::FromString(Pinned->GetText()) : FText::GetEmpty();
                })
                .Font(FCoreStyle::GetDefaultFontStyle(Config.bStatusTextBold ? TEXT("Bold") : TEXT("Regular"), Config.StatusFontSize))
                .ColorAndOpacity(Config.StatusTextColor)
                .AutoWrapText(bAllowWrap)
                .Clipping(EWidgetClipping::ClipToBoundsAlways);
        }

        TSharedRef<SWidget> BuildPercentText(const FLoadingStateWeakPtr& WeakLoadingState) const
        {
            return SNew(STextBlock)
                .MinDesiredWidth(40.0f)
                .Justification(ETextJustify::Right)
                .Text_Lambda([WeakLoadingState]() -> FText
                {
                    const FLoadingStatePtr Pinned = WeakLoadingState.Pin();
                    const float Percent = Pinned.IsValid() ? Pinned->GetPercent() : 0.0f;
                    return FText::FromString(FString::Printf(TEXT("%d%%"), FMath::RoundToInt(Percent)));
                })
                .Font(FCoreStyle::GetDefaultFontStyle(TEXT("Bold"), Config.PercentageFontSize))
                .ColorAndOpacity(Config.PercentageTextColor);
        }

        TSharedRef<SWidget> BuildProgressBar(const FLoadingStateWeakPtr& WeakLoadingState) const
        {
            return SNew(SBox)
                .HeightOverride(Config.ProgressBarHeight)
                [
                    SNew(SProgressBar)
                    .BorderPadding(FVector2D::ZeroVector)
                    .BackgroundImage(ProgressBackgroundBrush.Get())
                    .FillImage(ProgressFillBrush.Get())
                    .Percent_Lambda([WeakLoadingState]() -> TOptional<float>
                    {
                        // Always a real 0..1 value, never an unset TOptional - an unset Percent puts
                        // SProgressBar into its built-in indeterminate "marquee" mode, which draws the
                        // engine's default style/marquee brush instead of our configured fill color.
                        // Starting at a real 0% and climbing looks like a loading bar; an animated
                        // marquee in a color nothing else on screen matches does not.
                        const FLoadingStatePtr Pinned = WeakLoadingState.Pin();
                        const float Percent = Pinned.IsValid() ? Pinned->GetPercent() : 0.0f;
                        return Percent / 100.0f;
                    })
                ];
        }

        /** The classic floating rounded card, bottom-left by default: throbber beside a progress row, status line beneath, inside one bordered/shadowed card. */
        TSharedRef<SWidget> BuildFloatingCard(const FLoadingStateWeakPtr& WeakLoadingState) const
        {
            TSharedRef<SVerticalBox> Content = SNew(SVerticalBox);

            if (Config.bShowProgressBar || Config.bShowPercentageText)
            {
                TSharedRef<SHorizontalBox> ProgressRow = SNew(SHorizontalBox);

                if (Config.bShowPercentageText && Config.bPercentageOnLeft)
                {
                    ProgressRow->AddSlot()
                        .AutoWidth()
                        .VAlign(Config.InfoRowVerticalAlignment)
                        .Padding(FMargin(0.0f, 0.0f, 10.0f, 0.0f))
                        [
                            BuildPercentText(WeakLoadingState)
                        ];
                }

                if (Config.bShowProgressBar)
                {
                    ProgressRow->AddSlot()
                        .FillWidth(1.0f)
                        .VAlign(Config.InfoRowVerticalAlignment)
                        .Padding(FMargin(0.0f, 0.0f, Config.bPercentageOnLeft ? 0.0f : 10.0f, 0.0f))
                        [
                            BuildProgressBar(WeakLoadingState)
                        ];
                }

                if (Config.bShowPercentageText && !Config.bPercentageOnLeft)
                {
                    ProgressRow->AddSlot()
                        .AutoWidth()
                        .VAlign(Config.InfoRowVerticalAlignment)
                        [
                            BuildPercentText(WeakLoadingState)
                        ];
                }

                Content->AddSlot()
                    .AutoHeight()
                    .Padding(FMargin(0.0f, 0.0f, 0.0f, Config.PanelRowSpacing))
                    [
                        ProgressRow
                    ];
            }

            if (Config.bShowCustomMessages && Config.CustomMessages.Num() > 0)
            {
                Content->AddSlot()
                    .AutoHeight()
                    .Padding(FMargin(0.0f, 0.0f, 0.0f, 3.0f))
                    [
                        BuildCustomMessageText()
                    ];
            }

            Content->AddSlot()
                .AutoHeight()
                [
                    BuildStatusText(WeakLoadingState, /*bAllowWrap=*/ true)
                ];

            TSharedRef<SHorizontalBox> Row = SNew(SHorizontalBox);

            if (Config.bShowAccentStrip)
            {
                Row->AddSlot()
                    .AutoWidth()
                    [
                        SNew(SBox)
                        .WidthOverride(Config.AccentStripWidth)
                        [
                            SNew(SBorder)
                            .BorderImage(FCoreStyle::Get().GetBrush(TEXT("WhiteBrush")))
                            .BorderBackgroundColor(Config.AccentStripColor)
                        ]
                    ];
            }

            TSharedRef<SHorizontalBox> InnerRow = SNew(SHorizontalBox);

            if (Config.bShowThrobber && !Config.bThrobberOnRight)
            {
                InnerRow->AddSlot()
                    .AutoWidth()
                    .VAlign(Config.InfoRowVerticalAlignment)
                    .Padding(FMargin(0.0f, 2.0f, 16.0f, 0.0f))
                    [
                        BuildThrobber()
                    ];
            }

            InnerRow->AddSlot()
                .FillWidth(1.0f)
                [
                    Content
                ];

            if (Config.bShowThrobber && Config.bThrobberOnRight)
            {
                InnerRow->AddSlot()
                    .AutoWidth()
                    .VAlign(Config.InfoRowVerticalAlignment)
                    .Padding(FMargin(16.0f, 2.0f, 0.0f, 0.0f))
                    [
                        BuildThrobber()
                    ];
            }

            Row->AddSlot()
                .FillWidth(1.0f)
                .Padding(Config.PanelPadding)
                [
                    InnerRow
                ];

            TSharedRef<SWidget> CardBody = SNew(SBorder)
                .BorderImage(PanelBrush.Get())
                .Padding(FMargin(0.0f))
                [
                    Row
                ];

            return SNew(SBox)
                .WidthOverride(Config.PanelWidth)
                [
                    SNew(SOverlay)

                    + SOverlay::Slot()
                    .Padding(Config.bShowPanelShadow ? FMargin(Config.PanelShadowOffset, Config.PanelShadowOffset, 0.0f, 0.0f) : FMargin(0.0f))
                    [
                        SNew(SBorder)
                        .Visibility(Config.bShowPanelShadow ? EVisibility::HitTestInvisible : EVisibility::Collapsed)
                        .BorderImage(PanelShadowBrush.Get())
                        .Padding(FMargin(0.0f))
                        [
                            SNullWidget::NullWidget
                        ]
                    ]

                    + SOverlay::Slot()
                    [
                        CardBody
                    ]
                ];
        }

        /**
         * A bar spanning the full screen width, pinned to the bottom: an info row (throbber, status
         * text, percentage) sits above a progress bar that runs flush edge-to-edge along the very
         * bottom of the screen - the classic AAA loading-bar arrangement.
         */
        TSharedRef<SWidget> BuildFullWidthBottomBar(const FLoadingStateWeakPtr& WeakLoadingState) const
        {
            TSharedRef<SVerticalBox> Bar = SNew(SVerticalBox);

            if (Config.bShowAccentStrip)
            {
                Bar->AddSlot()
                    .AutoHeight()
                    [
                        SNew(SBox)
                        .HeightOverride(Config.AccentStripWidth)
                        [
                            SNew(SBorder)
                            .BorderImage(FCoreStyle::Get().GetBrush(TEXT("WhiteBrush")))
                            .BorderBackgroundColor(Config.AccentStripColor)
                        ]
                    ];
            }

            {
                TSharedRef<SHorizontalBox> InfoRow = SNew(SHorizontalBox);

                if (Config.bShowThrobber && !Config.bThrobberOnRight)
                {
                    InfoRow->AddSlot()
                        .AutoWidth()
                        .VAlign(Config.InfoRowVerticalAlignment)
                        .Padding(FMargin(0.0f, 0.0f, 14.0f, 0.0f))
                        [
                            BuildThrobber()
                        ];
                }

                if (Config.bShowPercentageText && Config.bPercentageOnLeft)
                {
                    InfoRow->AddSlot()
                        .AutoWidth()
                        .VAlign(Config.InfoRowVerticalAlignment)
                        .Padding(FMargin(0.0f, 0.0f, 14.0f, 0.0f))
                        [
                            BuildPercentText(WeakLoadingState)
                        ];
                }

                InfoRow->AddSlot()
                    .FillWidth(1.0f)
                    .VAlign(Config.InfoRowVerticalAlignment)
                    [
                        BuildStatusText(WeakLoadingState, /*bAllowWrap=*/ false)
                    ];

                if (Config.bShowPercentageText && !Config.bPercentageOnLeft)
                {
                    InfoRow->AddSlot()
                        .AutoWidth()
                        .VAlign(Config.InfoRowVerticalAlignment)
                        .Padding(FMargin(14.0f, 0.0f, 0.0f, 0.0f))
                        [
                            BuildPercentText(WeakLoadingState)
                        ];
                }

                if (Config.bShowThrobber && Config.bThrobberOnRight)
                {
                    InfoRow->AddSlot()
                        .AutoWidth()
                        .VAlign(Config.InfoRowVerticalAlignment)
                        .Padding(FMargin(14.0f, 0.0f, 0.0f, 0.0f))
                        [
                            BuildThrobber()
                        ];
                }

                TSharedRef<SVerticalBox> BarContent = SNew(SVerticalBox)

                    + SVerticalBox::Slot()
                    .AutoHeight()
                    [
                        InfoRow
                    ];

                if (Config.bShowCustomMessages && Config.CustomMessages.Num() > 0)
                {
                    BarContent->AddSlot()
                        .AutoHeight()
                        .Padding(FMargin(0.0f, 4.0f, 0.0f, 0.0f))
                        [
                            BuildCustomMessageText()
                        ];
                }

                Bar->AddSlot()
                    .AutoHeight()
                    [
                        SNew(SBorder)
                        .BorderImage(PanelBrush.Get())
                        .Padding(Config.PanelPadding)
                        [
                            BarContent
                        ]
                    ];
            }

            if (Config.bShowProgressBar)
            {
                // Left/right padding matches the info row above it, so PanelPadding visibly affects
                // the whole bar consistently instead of only the row above. Top/bottom stay separate
                // (PanelRowSpacing above, flush with zero below) so it still sits right at the screen edge.
                Bar->AddSlot()
                    .AutoHeight()
                    .Padding(FMargin(Config.PanelPadding.Left, Config.PanelRowSpacing, Config.PanelPadding.Right, 0.0f))
                    [
                        BuildProgressBar(WeakLoadingState)
                    ];
            }

            return Bar;
        }

        void BuildShuffledOrder()
        {
            ShuffledOrder = MakeShuffleOrder(Config.BackgroundBrushes.Num(), Config.bRandomizeBackgroundOrder);
            MessageShuffledOrder = MakeShuffleOrder(Config.CustomMessages.Num(), Config.bRandomizeCustomMessageOrder);
        }

        static TArray<int32> MakeShuffleOrder(int32 NumItems, bool bRandomize)
        {
            TArray<int32> Order;
            Order.Reserve(NumItems);

            for (int32 Index = 0; Index < NumItems; ++Index)
            {
                Order.Add(Index);
            }

            if (bRandomize)
            {
                // Fisher-Yates - a fresh shuffle every time the loading screen is constructed (i.e. every map load).
                for (int32 Index = Order.Num() - 1; Index > 0; --Index)
                {
                    const int32 SwapIndex = FMath::RandRange(0, Index);
                    Order.Swap(Index, SwapIndex);
                }
            }

            return Order;
        }

        struct FFadeCycleState
        {
            int32 PrevIndex = INDEX_NONE;
            int32 CurrentIndex = INDEX_NONE;
            float FadeAlpha = 1.0f;
        };

        /**
         * Which two items (of NumItems, in OrderedIndices order) are up right now and how far the
         * crossfade between them has progressed, purely as a function of elapsed wall-clock time.
         * Shared by the background image rotation and the custom message rotation - same math, two
         * independent timelines. Deliberately stateless (no Tick() override, no mutation from paint) -
         * this widget's attributes already re-evaluate continuously on MoviePlayer's dedicated Slate
         * loading thread even while the game thread is blocked (that's how the status text and
         * progress bar stay live), so the fade rides the same mechanism instead of depending on
         * whether SWidget::Tick() itself gets pumped by that thread's stripped-down loop.
         */
        static FFadeCycleState ComputeFadeCycle(double Elapsed, int32 NumItems, float IntervalSeconds, float FadeDurationSeconds, const TArray<int32>& OrderedIndices)
        {
            FFadeCycleState Result;

            if (NumItems <= 0)
            {
                return Result;
            }

            if (NumItems == 1)
            {
                Result.PrevIndex = 0;
                Result.CurrentIndex = 0;
                Result.FadeAlpha = 1.0f;
                return Result;
            }

            const double Interval = FMath::Max(1.0, static_cast<double>(IntervalSeconds));
            const double FadeDuration = FMath::Clamp(static_cast<double>(FadeDurationSeconds), 0.0, Interval * 0.9);

            const int32 CycleIndex = FMath::Max(0, FMath::FloorToInt(Elapsed / Interval));
            const double TimeIntoCycle = Elapsed - (static_cast<double>(CycleIndex) * Interval);

            auto GetOrdered = [&OrderedIndices, NumItems](int32 Cycle) -> int32
            {
                const int32 SafeCycle = FMath::Max(0, Cycle);
                return OrderedIndices.Num() > 0 ? OrderedIndices[SafeCycle % OrderedIndices.Num()] : (SafeCycle % NumItems);
            };

            Result.CurrentIndex = GetOrdered(CycleIndex);
            Result.PrevIndex = CycleIndex > 0 ? GetOrdered(CycleIndex - 1) : Result.CurrentIndex;
            Result.FadeAlpha = FadeDuration > 0.0
                ? static_cast<float>(FMath::Clamp(TimeIntoCycle / FadeDuration, 0.0, 1.0))
                : 1.0f;

            return Result;
        }

        FFadeCycleState ComputeBackgroundFadeCycle() const
        {
            const double Elapsed = FPlatformTime::Seconds() - StartTime;
            return ComputeFadeCycle(Elapsed, Config.BackgroundBrushes.Num(), Config.BackgroundImageIntervalSeconds, Config.BackgroundFadeDurationSeconds, ShuffledOrder);
        }

        FFadeCycleState ComputeMessageFadeCycle() const
        {
            const double Elapsed = FPlatformTime::Seconds() - StartTime;
            return ComputeFadeCycle(Elapsed, Config.CustomMessages.Num(), Config.CustomMessageIntervalSeconds, Config.CustomMessageFadeDurationSeconds, MessageShuffledOrder);
        }

        /** A crossfading line of flavor text (Config.CustomMessages), independent of the technical status line - same fade mechanism as the background images. */
        TSharedRef<SWidget> BuildCustomMessageText() const
        {
            if (!Config.bShowCustomMessages || Config.CustomMessages.Num() == 0)
            {
                return SNullWidget::NullWidget;
            }

            const FSlateFontInfo Font = FCoreStyle::GetDefaultFontStyle(
                Config.bCustomMessageItalic ? TEXT("Italic") : TEXT("Regular"),
                Config.CustomMessageFontSize);

            return SNew(SOverlay)

                + SOverlay::Slot()
                [
                    SNew(STextBlock)
                    .Text_Lambda([this]() -> FText
                    {
                        const FFadeCycleState Fade = ComputeMessageFadeCycle();
                        return Config.CustomMessages.IsValidIndex(Fade.PrevIndex) ? Config.CustomMessages[Fade.PrevIndex] : FText::GetEmpty();
                    })
                    .Font(Font)
                    .Justification(Config.CustomMessageJustification)
                    .ColorAndOpacity_Lambda([this]() -> FSlateColor
                    {
                        const FFadeCycleState Fade = ComputeMessageFadeCycle();
                        FLinearColor Color = Config.CustomMessageColor;
                        Color.A *= (1.0f - Fade.FadeAlpha);
                        return FSlateColor(Color);
                    })
                    .Clipping(EWidgetClipping::ClipToBoundsAlways)
                ]

                + SOverlay::Slot()
                [
                    SNew(STextBlock)
                    .Text_Lambda([this]() -> FText
                    {
                        const FFadeCycleState Fade = ComputeMessageFadeCycle();
                        return Config.CustomMessages.IsValidIndex(Fade.CurrentIndex) ? Config.CustomMessages[Fade.CurrentIndex] : FText::GetEmpty();
                    })
                    .Font(Font)
                    .Justification(Config.CustomMessageJustification)
                    .ColorAndOpacity_Lambda([this]() -> FSlateColor
                    {
                        const FFadeCycleState Fade = ComputeMessageFadeCycle();
                        FLinearColor Color = Config.CustomMessageColor;
                        Color.A *= Fade.FadeAlpha;
                        return FSlateColor(Color);
                    })
                    .Clipping(EWidgetClipping::ClipToBoundsAlways)
                ];
        }

        FLoadingStatePtr LoadingState;
        FDisplayConfig Config;
        TSharedPtr<FSlateRoundedBoxBrush> PanelBrush;
        TSharedPtr<FSlateRoundedBoxBrush> PanelShadowBrush;
        TSharedPtr<FSlateRoundedBoxBrush> ProgressBackgroundBrush;
        TSharedPtr<FSlateRoundedBoxBrush> ProgressFillBrush;
        TArray<int32> ShuffledOrder;
        TArray<int32> MessageShuffledOrder;
        double StartTime = 0.0;
    };

    /**
     * Reads ULoadingMonitorSettings, on the game thread, and hands back a plain snapshot both the
     * real loading screen (FLoadingMonitorModule) and the editor preview window use. Background
     * textures are resolved and LoadSynchronous()'d here too - also game-thread-only. OutLoadedTextures
     * keeps a strong reference to each for as long as the caller needs the screen to keep showing
     * them, so GC can't pull one out from under the widget mid-display.
     */
    static FDisplayConfig BuildDisplayConfigFromSettings(TArray<TStrongObjectPtr<UTexture2D>>& OutLoadedTextures)
    {
        FDisplayConfig Config;
        OutLoadedTextures.Reset();

        const ULoadingMonitorSettings* Settings = GetDefault<ULoadingMonitorSettings>();
        if (Settings == nullptr)
        {
            return Config;
        }

        for (const TSoftObjectPtr<UTexture2D>& SoftTexture : Settings->BackgroundImages)
        {
            if (SoftTexture.IsNull())
            {
                continue;
            }

            UTexture2D* Texture = SoftTexture.LoadSynchronous();
            if (Texture == nullptr)
            {
                UE_LOG(LogTemp, Warning,
                    TEXT("LoadingMonitor: could not load background texture '%s' - skipping."),
                    *SoftTexture.ToString());
                continue;
            }

            OutLoadedTextures.Add(TStrongObjectPtr<UTexture2D>(Texture));

            const FVector2D ImageSize(
                static_cast<float>(FMath::Max(1, Texture->GetSizeX())),
                static_cast<float>(FMath::Max(1, Texture->GetSizeY())));

            Config.BackgroundBrushes.Add(MakeShared<FSlateImageBrush>(
                Texture,
                ImageSize,
                FLinearColor::White,
                ESlateBrushTileType::NoTile,
                ESlateBrushImageType::FullColor));
        }

        // Nothing configured (or everything failed to load) - fall back to the plugin's bundled image
        // so the screen is never blank out of the box.
        if (Config.BackgroundBrushes.Num() == 0)
        {
            const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("LoadingMonitor"));
            const FString FallbackPath = Plugin.IsValid()
                ? FPaths::Combine(Plugin->GetBaseDir(), TEXT("Resources/LoadingBackground.png"))
                : FString();

            if (!FallbackPath.IsEmpty() && FPaths::FileExists(FallbackPath))
            {
                Config.BackgroundBrushes.Add(MakeShared<FSlateImageBrush>(
                    FallbackPath,
                    FVector2D(1920.0f, 1080.0f),
                    FLinearColor::White,
                    ESlateBrushTileType::NoTile,
                    ESlateBrushImageType::FullColor));
            }
        }

        Config.BackgroundImageIntervalSeconds = FMath::Max(1.0f, Settings->BackgroundImageIntervalSeconds);
        Config.BackgroundFadeDurationSeconds = FMath::Max(0.0f, Settings->BackgroundFadeDurationSeconds);
        Config.bRandomizeBackgroundOrder = Settings->bRandomizeBackgroundOrder;
        Config.ScreenDarkenColor = Settings->ScreenDarkenColor;

        Config.GameName = Settings->GameName;
        Config.GameNameFontSize = Settings->GameNameFontSize;
        Config.bGameNameBold = Settings->bGameNameBold;
        Config.GameNameColor = Settings->GameNameColor;
        Config.GameNameHorizontalAlignment = Settings->GameNameHorizontalAlignment;
        Config.GameNameVerticalAlignment = Settings->GameNameVerticalAlignment;
        Config.GameNameJustification = Settings->GameNameJustification;
        Config.GameNameMargin = Settings->GameNameMargin;

        Config.NumVisibleLogLines = FMath::Clamp(Settings->NumVisibleLogLines, 1, 12);
        Config.StatusFontSize = Settings->StatusFontSize;
        Config.StatusTextColor = Settings->StatusTextColor;
        Config.bStatusTextBold = Settings->bStatusTextBold;
        Config.bShowDetailedAssetNames = Settings->bShowDetailedAssetNames;

        Config.bShowCustomMessages = Settings->bShowCustomMessages;
        Config.CustomMessages = Settings->CustomMessages;
        Config.CustomMessageIntervalSeconds = FMath::Max(0.5f, Settings->CustomMessageIntervalSeconds);
        Config.CustomMessageFadeDurationSeconds = FMath::Max(0.0f, Settings->CustomMessageFadeDurationSeconds);
        Config.bRandomizeCustomMessageOrder = Settings->bRandomizeCustomMessageOrder;
        Config.CustomMessageFontSize = Settings->CustomMessageFontSize;
        Config.CustomMessageColor = Settings->CustomMessageColor;
        Config.bCustomMessageItalic = Settings->bCustomMessageItalic;
        Config.CustomMessageJustification = Settings->CustomMessageJustification;

        Config.bShowProgressBar = Settings->bShowProgressBar;
        Config.bShowPercentageText = Settings->bShowPercentageText;
        Config.ProgressBarSmoothingSpeed = Settings->ProgressBarSmoothingSpeed;
        Config.AlmostThereThresholdPercent = FMath::Clamp(Settings->AlmostThereThresholdPercent, 1.0f, 100.0f);
        Config.ProgressBarFillColor = Settings->ProgressBarFillColor;
        Config.ProgressBarBackgroundColor = Settings->ProgressBarBackgroundColor;
        Config.ProgressBarHeight = FMath::Max(2.0f, Settings->ProgressBarHeight);
        Config.ProgressBarCornerRadius = Settings->ProgressBarCornerRadius;
        Config.PercentageTextColor = Settings->PercentageTextColor;
        Config.PercentageFontSize = Settings->PercentageFontSize;
        Config.bPercentageOnLeft = Settings->bPercentageOnLeft;

        Config.bShowThrobber = Settings->bShowThrobber;
        Config.ThrobberColor = Settings->ThrobberColor;
        Config.ThrobberSize = Settings->ThrobberSize;
        Config.bThrobberOnRight = Settings->bThrobberOnRight;
        Config.InfoRowVerticalAlignment = Settings->InfoRowVerticalAlignment;
        Config.bShowAccentStrip = Settings->bShowAccentStrip;
        Config.AccentStripColor = Settings->AccentStripColor;
        Config.AccentStripWidth = Settings->AccentStripWidth;

        Config.bShowStatusPanel = Settings->bShowStatusPanel;
        Config.PanelLayoutMode = Settings->PanelLayoutMode;
        Config.PanelHorizontalAlignment = Settings->PanelHorizontalAlignment;
        Config.PanelVerticalAlignment = Settings->PanelVerticalAlignment;
        Config.PanelWidth = Settings->PanelWidth;
        Config.PanelMargin = Settings->PanelMargin;
        Config.PanelPadding = Settings->PanelPadding;
        Config.PanelRowSpacing = Settings->PanelRowSpacing;
        Config.PanelBackgroundColor = Settings->PanelBackgroundColor;
        Config.PanelCornerRadius = Settings->PanelCornerRadius;
        Config.PanelBorderColor = Settings->PanelBorderColor;
        Config.PanelBorderThickness = Settings->PanelBorderThickness;
        Config.bShowPanelShadow = Settings->bShowPanelShadow;
        Config.PanelShadowColor = Settings->PanelShadowColor;
        Config.PanelShadowOffset = Settings->PanelShadowOffset;

        Config.MessageLabels.SetNum(static_cast<int32>(EStatusCategory::Count));
        Config.MessageLabels[static_cast<int32>(EStatusCategory::LoadingMap)] = Settings->Message_LoadingMap.ToString();
        Config.MessageLabels[static_cast<int32>(EStatusCategory::CompilingShaders)] = Settings->Message_CompilingShaders.ToString();
        Config.MessageLabels[static_cast<int32>(EStatusCategory::PreparingTextures)] = Settings->Message_PreparingTextures.ToString();
        Config.MessageLabels[static_cast<int32>(EStatusCategory::PreparingStaticMeshes)] = Settings->Message_PreparingStaticMeshes.ToString();
        Config.MessageLabels[static_cast<int32>(EStatusCategory::BuildingDistanceFields)] = Settings->Message_BuildingDistanceFields.ToString();
        Config.MessageLabels[static_cast<int32>(EStatusCategory::CompilingParticleSystems)] = Settings->Message_CompilingParticleSystems.ToString();
        Config.MessageLabels[static_cast<int32>(EStatusCategory::StreamingWorldData)] = Settings->Message_StreamingWorldData.ToString();
        Config.MessageLabels[static_cast<int32>(EStatusCategory::FetchingCachedData)] = Settings->Message_FetchingCachedData.ToString();
        Config.MessageLabels[static_cast<int32>(EStatusCategory::FinishingWorldSetup)] = Settings->Message_FinishingWorldSetup.ToString();
        Config.MessageLabels[static_cast<int32>(EStatusCategory::AlmostThere)] = Settings->Message_AlmostThere.ToString();

        return Config;
    }

#if WITH_EDITOR
    /**
     * Drives the "Open Live Preview" button: builds the same widget a real load would use, but feeds
     * it a simulated progress/category cycle via a normal game-thread ticker instead of a real
     * compile - there's nothing blocking here, so no MoviePlayer thread is needed just to look at it.
     */
    class FLoadingMonitorPreviewSession : public TSharedFromThis<FLoadingMonitorPreviewSession>
    {
    public:
        static void Open()
        {
            if (GActiveSession.IsValid())
            {
                if (const TSharedPtr<SWindow> ExistingWindow = GActiveSession->WindowWeak.Pin())
                {
                    FSlateApplication::Get().RequestDestroyWindow(ExistingWindow.ToSharedRef());
                }
                GActiveSession.Reset();
            }

            const TSharedRef<FLoadingMonitorPreviewSession> Session = MakeShared<FLoadingMonitorPreviewSession>();
            Session->Start();
            GActiveSession = Session;
        }

        void Start()
        {
            const ULoadingMonitorSettings* Settings = GetDefault<ULoadingMonitorSettings>();

            if (Settings != nullptr && Settings->bUseCustomWidget)
            {
                StartCustomWidgetPreview(Settings);
                return;
            }

            DemoState = MakeShared<FLoadingState, ESPMode::ThreadSafe>();
            Config = BuildDisplayConfigFromSettings(KeepAliveTextures);

            DemoState->SetMessageLabels(Config.MessageLabels);
            DemoState->SetMaxVisibleLines(Config.NumVisibleLogLines);
            DemoState->SetSmoothingSpeed(Config.ProgressBarSmoothingSpeed);
            DemoState->SetAlmostThereThreshold(Config.AlmostThereThresholdPercent);
            DemoState->SetShowAssetDetail(Config.bShowDetailedAssetNames);
            DemoState->Reset(TEXT("PreviewMap"));

            const TSharedRef<SWindow> NewWindow =
                SNew(SWindow)
                .Title(FText::FromString(TEXT("Loading Screen Preview")))
                .ClientSize(FVector2D(1280.0f, 720.0f))
                .SupportsMaximize(true)
                .SupportsMinimize(true)
                [
                    SNew(SLoadingMonitorWidget)
                    .LoadingState(DemoState)
                    .Config(Config)
                ];

            OpenWindow(NewWindow);

            TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
                FTickerDelegate::CreateSP(this, &FLoadingMonitorPreviewSession::Tick), 0.1f);
        }

        void Stop()
        {
            if (TickerHandle.IsValid())
            {
                FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
                TickerHandle.Reset();
            }

            DemoState.Reset();
            KeepAliveTextures.Reset();
            PreviewCustomWidget.Reset();
        }

    private:
        /**
         * Safe here in a way the real loading screen can't be: a plain SWindow runs on the normal
         * Slate application/game thread, not MoviePlayer's dedicated loading thread, so there's no
         * threading conflict with UMG. No simulated data feed though - this just shows your widget's
         * own initial state, since we don't know what it binds to.
         */
        void StartCustomWidgetPreview(const ULoadingMonitorSettings* Settings)
        {
            UClass* WidgetClass = Settings->CustomWidgetBlueprint.LoadSynchronous();
            UWorld* OwningWorld = ResolveAnyValidWorld();

            const bool bValidClass = WidgetClass != nullptr
                && WidgetClass->IsChildOf(UUserWidget::StaticClass())
                && !WidgetClass->HasAnyClassFlags(CLASS_Abstract);

            if (!bValidClass || OwningWorld == nullptr || !IsValid(OwningWorld))
            {
                UE_LOG(LogTemp, Warning,
                    TEXT("LoadingMonitor: Preview could not create CustomWidgetBlueprint (class invalid/abstract, or no PIE/Game world is running to own it - start Play first)."));
                return;
            }

            UUserWidget* CustomWidget = CreateWidget<UUserWidget>(OwningWorld, WidgetClass);
            if (CustomWidget == nullptr)
            {
                return;
            }

            PreviewCustomWidget = TStrongObjectPtr<UUserWidget>(CustomWidget);

            const TSharedRef<SWindow> NewWindow =
                SNew(SWindow)
                .Title(FText::FromString(TEXT("Loading Screen Preview (Custom Widget)")))
                .ClientSize(FVector2D(1280.0f, 720.0f))
                .SupportsMaximize(true)
                .SupportsMinimize(true)
                [
                    CustomWidget->TakeWidget()
                ];

            OpenWindow(NewWindow);
        }

        void OpenWindow(const TSharedRef<SWindow>& NewWindow)
        {
            WindowWeak = NewWindow;
            FSlateApplication::Get().AddWindow(NewWindow);

            const TSharedRef<FLoadingMonitorPreviewSession> SelfRef = AsShared();
            NewWindow->GetOnWindowClosedEvent().AddLambda([SelfRef](const TSharedRef<SWindow>&)
            {
                SelfRef->Stop();
                if (GActiveSession == SelfRef)
                {
                    GActiveSession.Reset();
                }
            });
        }

        bool Tick(float DeltaTime)
        {
            if (!DemoState.IsValid())
            {
                return false;
            }

            // Steady completion, feeding the same cumulative completed-vs-total-ever-queued math the
            // real progress bar uses - climbs continuously rather than sawtoothing back to 0.
            const float JustCompleted = FMath::Min(DemoRemaining, DemoCompletionRate * DeltaTime);
            DemoRemaining -= JustCompleted;

            static const EStatusCategory DemoCycle[] =
            {
                EStatusCategory::CompilingShaders,
                EStatusCategory::PreparingTextures,
                EStatusCategory::PreparingStaticMeshes,
                EStatusCategory::BuildingDistanceFields,
                EStatusCategory::CompilingParticleSystems,
                EStatusCategory::StreamingWorldData,
                EStatusCategory::FetchingCachedData,
            };

            // Matches DemoCycle index-for-index - lets the preview show what Detailed Asset Names
            // actually looks like too, without a real compile to pull names from.
            static const TCHAR* DemoDetail[] =
            {
                TEXT("M_Character_Master"),
                TEXT("T_Character_Diffuse"),
                TEXT("SM_Rock_01"),
                TEXT("SM_Castle_Wall"),
                TEXT("NS_Torch_Fire"),
                TEXT("World Partition Cell 12"),
                TEXT("DDC: Shader Bytecode"),
            };
            static_assert(UE_ARRAY_COUNT(DemoDetail) == UE_ARRAY_COUNT(DemoCycle), "Keep these two arrays in sync.");

            CategoryTimer += DeltaTime;
            if (CategoryTimer >= 1.6f)
            {
                CategoryTimer = 0.0f;
                CategoryIndex = (CategoryIndex + 1) % UE_ARRAY_COUNT(DemoCycle);
                DemoState->PushCategory(DemoCycle[CategoryIndex], DemoDetail[CategoryIndex]);

                // A new category starting simulates a new phase of work being discovered - a real,
                // bounded dip in percentage (never a reset to 0%), same as a real multi-phase load
                // (e.g. HLODs queuing up right as static meshes finish). Kept smaller than
                // DemoCompletionRate on average so the overall trend still climbs toward 100%.
                DemoRemaining += FMath::RandRange(15.0f, 45.0f);
            }

            DemoState->UpdateProgress(FMath::RoundToInt(DemoRemaining), FMath::RoundToInt(JustCompleted));

            return true;
        }

        FLoadingStatePtr DemoState;
        TArray<TStrongObjectPtr<UTexture2D>> KeepAliveTextures;
        FDisplayConfig Config;
        TWeakPtr<SWindow> WindowWeak;
        FTSTicker::FDelegateHandle TickerHandle;
        static constexpr float DemoCompletionRate = 30.0f; // simulated items/sec
        float DemoRemaining = 90.0f;
        float CategoryTimer = 0.0f;
        int32 CategoryIndex = 0;
        TStrongObjectPtr<UUserWidget> PreviewCustomWidget;

        static TSharedPtr<FLoadingMonitorPreviewSession> GActiveSession;
    };

    TSharedPtr<FLoadingMonitorPreviewSession> FLoadingMonitorPreviewSession::GActiveSession;
#endif // WITH_EDITOR

    /**
     * Prefers an actual PIE/Game world - never the level editor's own preview world. Handing
     * CreateWidget() the editor world instead of the game world it has nothing to do with is a
     * plausible crash source in its own right (wrong local player/viewport context for the widget's
     * internal initialization), and GetWorldContexts() can return the editor world first.
     */
    static UWorld* ResolveAnyValidWorld()
    {
        if (GEngine == nullptr)
        {
            return nullptr;
        }

        UWorld* Fallback = nullptr;

        for (const FWorldContext& Context : GEngine->GetWorldContexts())
        {
            UWorld* CandidateWorld = Context.World();
            if (CandidateWorld == nullptr)
            {
                continue;
            }

            if (Context.WorldType == EWorldType::PIE || Context.WorldType == EWorldType::Game)
            {
                return CandidateWorld;
            }

            if (Fallback == nullptr && Context.WorldType != EWorldType::Editor)
            {
                Fallback = CandidateWorld;
            }
        }

        return Fallback;
    }
}

#if WITH_EDITOR
void OpenLoadingMonitorPreviewWindow()
{
    LoadingMonitor::FLoadingMonitorPreviewSession::Open();
}
#else
void OpenLoadingMonitorPreviewWindow()
{
}
#endif

class FLoadingMonitorModule final : public IModuleInterface
{
public:
    virtual void StartupModule() override
    {
        State = MakeShared<LoadingMonitor::FLoadingState, ESPMode::ThreadSafe>();
        OutputDevice = MakeUnique<LoadingMonitor::FLoadingLogOutputDevice>(State.ToSharedRef());

        if (GLog != nullptr)
        {
            GLog->AddOutputDevice(OutputDevice.Get());
        }

        PreLoadMapHandle = FCoreUObjectDelegates::PreLoadMap.AddRaw(
            this,
            &FLoadingMonitorModule::HandlePreLoadMap);

        PostLoadMapHandle = FCoreUObjectDelegates::PostLoadMapWithWorld.AddRaw(
            this,
            &FLoadingMonitorModule::HandlePostLoadMap);

        // FAssetCompilingManager::OnAssetPostCompileEvent is declared with
        // DECLARE_TS_MULTICAST_DELEGATE - explicitly documented as safe to fire from any thread -
        // so this is the one place allowed to touch FAssetCompilingManager directly.
        AssetCompileHandle = FAssetCompilingManager::Get().OnAssetPostCompileEvent().AddRaw(
            this,
            &FLoadingMonitorModule::HandleAssetPostCompile);
    }

    virtual void ShutdownModule() override
    {
        if (PreLoadMapHandle.IsValid())
        {
            FCoreUObjectDelegates::PreLoadMap.Remove(PreLoadMapHandle);
        }

        if (PostLoadMapHandle.IsValid())
        {
            FCoreUObjectDelegates::PostLoadMapWithWorld.Remove(PostLoadMapHandle);
        }

        if (AssetCompileHandle.IsValid())
        {
            FAssetCompilingManager::Get().OnAssetPostCompileEvent().Remove(AssetCompileHandle);
        }

        if (GLog != nullptr && OutputDevice.IsValid())
        {
            GLog->RemoveOutputDevice(OutputDevice.Get());
        }

        OutputDevice.Reset();
        State.Reset();
        LoadedBackgroundTextures.Reset();
        RemoveCustomWidgetOverlay();
    }

private:
    /** ULoadingMonitorSettings::AllowedLevelNames check - empty list = show for every level (default, unchanged behavior). */
    static bool ShouldShowForMap(const ULoadingMonitorSettings* Settings, const FString& MapName)
    {
        if (Settings == nullptr || Settings->AllowedLevelNames.Num() == 0)
        {
            return true;
        }

        const FString ShortName = FPaths::GetBaseFilename(MapName);

        for (const FString& Allowed : Settings->AllowedLevelNames)
        {
            if (!Allowed.IsEmpty() &&
                (MapName.Equals(Allowed, ESearchCase::IgnoreCase) || ShortName.Equals(Allowed, ESearchCase::IgnoreCase)))
            {
                return true;
            }
        }

        return false;
    }

    void HandlePreLoadMap(const FString& MapName)
    {
        if (!State.IsValid())
        {
            return;
        }

        const ULoadingMonitorSettings* Settings = GetDefault<ULoadingMonitorSettings>();

        if (!ShouldShowForMap(Settings, MapName))
        {
            return;
        }

        LoadingMonitor::FDisplayConfig Config = LoadingMonitor::BuildDisplayConfigFromSettings(LoadedBackgroundTextures);

        State->SetMessageLabels(Config.MessageLabels);
        State->SetMaxVisibleLines(Config.NumVisibleLogLines);
        State->SetSmoothingSpeed(Config.ProgressBarSmoothingSpeed);
        State->SetAlmostThereThreshold(Config.AlmostThereThresholdPercent);
        State->SetShowAssetDetail(Config.bShowDetailedAssetNames);
        State->Reset(MapName);

        // Seed a baseline immediately - if compilation is already in flight when the map load
        // starts (e.g. it was queued just before travel), the bar doesn't sit at "..." for no reason.
        State->UpdateProgress(FAssetCompilingManager::Get().GetNumRemainingAssets());

        if (Settings != nullptr && Settings->bUseCustomWidget)
        {
            // Deliberately does NOT go through MoviePlayer at all - see the long comment on
            // bUseCustomWidget/CustomWidgetBlueprint for why: a UMG widget on MoviePlayer's
            // dedicated loading thread is a confirmed crash, not a theoretical risk.
            ShowCustomWidgetOverlay(Settings);
            return;
        }

        if (!IsMoviePlayerEnabled())
        {
            return;
        }

        IGameMoviePlayer* const MoviePlayer = GetMoviePlayer();
        if (MoviePlayer == nullptr || !MoviePlayer->IsInitialized())
        {
            return;
        }

        FLoadingScreenAttributes Attributes;
        Attributes.bAutoCompleteWhenLoadingCompletes = true;
        Attributes.bMoviesAreSkippable = false;
        Attributes.bWaitForManualStop = false;
        Attributes.bAllowEngineTick = false;
        Attributes.MinimumLoadingScreenDisplayTime = Settings != nullptr ? FMath::Max(0.0f, Settings->MinimumDisplayTimeSeconds) : 0.0f;

        Attributes.WidgetLoadingScreen = SNew(LoadingMonitor::SLoadingMonitorWidget)
            .LoadingState(State)
            .Config(Config);

        MoviePlayer->SetupLoadingScreen(Attributes);
    }

    /**
     * The safe alternative to routing a UMG widget through MoviePlayer: an ordinary AddToViewport()
     * overlay, added right before the freeze starts and removed in HandlePostLoadMap - the same
     * supported way any UMG widget is shown. It won't animate *through* a real freeze (nothing
     * UMG-based safely can - see bUseCustomWidget's tooltip), but it also won't crash.
     */
    void ShowCustomWidgetOverlay(const ULoadingMonitorSettings* Settings)
    {
        RemoveCustomWidgetOverlay();

        if (Settings == nullptr || Settings->CustomWidgetBlueprint.IsNull())
        {
            return;
        }

        UClass* WidgetClass = Settings->CustomWidgetBlueprint.LoadSynchronous();
        UWorld* OwningWorld = LoadingMonitor::ResolveAnyValidWorld();

        const bool bValidClass = WidgetClass != nullptr
            && WidgetClass->IsChildOf(UUserWidget::StaticClass())
            && !WidgetClass->HasAnyClassFlags(CLASS_Abstract);

        if (!bValidClass || OwningWorld == nullptr || !IsValid(OwningWorld))
        {
            UE_LOG(LogTemp, Warning,
                TEXT("LoadingMonitor: CustomWidgetBlueprint could not be created (class invalid/abstract or no usable world) - no loading screen will show for this load."));
            return;
        }

        UUserWidget* CustomWidget = CreateWidget<UUserWidget>(OwningWorld, WidgetClass);
        if (CustomWidget == nullptr)
        {
            return;
        }

        CustomWidgetInstance = TStrongObjectPtr<UUserWidget>(CustomWidget);
        CustomWidget->AddToViewport(10000);
    }

    void RemoveCustomWidgetOverlay()
    {
        if (CustomWidgetInstance.IsValid())
        {
            CustomWidgetInstance->RemoveFromParent();
        }

        CustomWidgetInstance.Reset();
    }

    void HandlePostLoadMap(UWorld* LoadedWorld)
    {
        if (State.IsValid())
        {
            State->PushCategory(LoadingMonitor::EStatusCategory::FinishingWorldSetup);

            // The level is genuinely ready now - finish the bar to 100% (smoothed, not a snap) instead
            // of leaving it wherever the last real asset-compiling sample happened to land. See
            // ForceComplete()'s comment, and pair this with MinimumDisplayTimeSeconds so it's visible.
            State->ForceComplete();
        }

        RemoveCustomWidgetOverlay();
    }

    /**
     * Bound to FAssetCompilingManager::OnAssetPostCompileEvent, which fires whenever a batch of
     * assets (static meshes, textures, shader maps, HLODs, etc) finishes compiling - including
     * mid-flush, so it fires progressively throughout a blocking map load, not just at the very end.
     * Data.Num() is how many finished in this batch, which is what lets UpdateProgress accumulate a
     * true running total instead of only ever seeing the current in-flight count. This is the ONLY
     * place in this plugin that reads FAssetCompilingManager directly; everything the Slate loading
     * thread displays comes from State (our own mutex-guarded snapshot) instead.
     */
    void HandleAssetPostCompile(const TArray<FAssetCompileData>& Data)
    {
        if (State.IsValid())
        {
            State->UpdateProgress(FAssetCompilingManager::Get().GetNumRemainingAssets(), Data.Num());
        }
    }

private:
    TSharedPtr<LoadingMonitor::FLoadingState, ESPMode::ThreadSafe> State;
    TUniquePtr<LoadingMonitor::FLoadingLogOutputDevice> OutputDevice;
    TArray<TStrongObjectPtr<UTexture2D>> LoadedBackgroundTextures;
    TStrongObjectPtr<UUserWidget> CustomWidgetInstance;
    FDelegateHandle PreLoadMapHandle;
    FDelegateHandle PostLoadMapHandle;
    FDelegateHandle AssetCompileHandle;
};

IMPLEMENT_MODULE(FLoadingMonitorModule, LoadingMonitor)
