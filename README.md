# Loading Monitor (UE 5.6)

A UE 5.6 runtime Unreal Engine plugin that shows a Slate/MoviePlayer loading overlay while a blocking map load is in progress. It captures useful engine log activity on any thread, shows a live compiling progress bar/percentage, and cycles through a configurable set of background images - all fully configurable from **Project Settings > Plugins > Loading Monitor**, nothing hardcoded.

## Captured activity

- Shader compiler messages
- Waiting/building static meshes
- Waiting/building textures
- Mesh distance-field builds
- Niagara compilation
- World/level streaming messages
- Derived Data Cache messages
- LoadMap status

## Configuring it

Everything below lives in **Edit > Project Settings > Plugins > Loading Monitor** (backed by `Config/DefaultGame.ini`, section `[/Script/LoadingMonitor.LoadingMonitorSettings]`) - no code changes needed, and changes apply on the next map load without an editor restart.

**Preview & Presets**
- **Open Live Preview** - opens a real window showing the loading screen exactly as configured: same widget, same rotating/crossfading images, same fonts and colors, driven by a simulated progress feed so you can see it animate without triggering a real freeze. If **Use Custom Widget** is on, this instead opens your assigned Widget Blueprint directly (statically - no simulated data, since we don't know what it binds to). Editor-only.
- **Apply: Cyan Ops / Ember Alert / Toxic Signal / Violet Core / Solar Gold / Signal Red** - one-click color schemes. Each sets `ProgressBarFillColor` / `ThrobberColor` / `AccentStripColor` / `PanelBackgroundColor` / `StatusTextColor` together; `GameNameColor` and `PercentageTextColor` are deliberately left pure white, since that's what keeps every scheme readable.

**General**
- `AllowedLevelNames` - which levels show this loading screen at all. Empty (default) = every level load shows it. Add short map names (`MainMap`) or full paths (`/Game/Maps/MainMap`) to restrict it to just those.
- `MinimumDisplayTimeSeconds` (default 0.6s) - keeps the screen up for at least this long after loading actually finishes. The bar finishes climbing to 100% right as loading completes rather than jumping straight to the level opening (see Progress Bar below) - this is the window that finish is actually visible in, so 0 means you likely won't see it. No effect when using a custom widget.

**Background**
- `BackgroundImages` - pick any `UTexture2D` straight from the Content Browser (asset picker, not a filename). A fresh one shows at the start of the loading screen and rotates every `BackgroundImageIntervalSeconds`, crossfading smoothly over `BackgroundFadeDurationSeconds` (0 = hard cut). Empty = falls back to the plugin's bundled `Resources/LoadingBackground.png`.
- `bRandomizeBackgroundOrder` - shuffled order (default) vs. list order.
- `ScreenDarkenColor` - overlay tint so text stays readable over any image.

**Game Title**
- `GameName` - your game's title/logo text. Empty hides it.
- `GameNameFontSize`, `bGameNameBold`, `GameNameColor`.
- `GameNameHorizontalAlignment` / `GameNameVerticalAlignment` - placed independently of the status panel below (e.g. big title top-center while the panel stays bottom-left).
- `GameNameJustification`, `GameNameMargin`.

**Status Log**
- `NumVisibleLogLines` - defaults to **1**: only the latest message is shown in place, nothing stacks.
- `StatusFontSize`, `StatusTextColor`, `bStatusTextBold`.
- `bShowDetailedAssetNames` - off (default): status lines stay generic, e.g. "Preparing Textures". On: the specific asset name is appended, e.g. "Preparing Textures: T_Character_Diffuse", "Building Distance Fields: SM_Rock_01". More informative, but the line changes as fast as the engine actually names assets internally - expect it to feel busy during a heavy compile burst. The Preview window demonstrates this with placeholder asset names.

**Status Messages**
- `Message_CompilingShaders`, `Message_PreparingTextures`, `Message_PreparingStaticMeshes`, `Message_BuildingDistanceFields`, `Message_CompilingParticleSystems`, `Message_StreamingWorldData`, `Message_FetchingCachedData`, `Message_LoadingMap`, `Message_FinishingWorldSetup` - the plugin classifies raw, technical engine log lines into these buckets and only ever displays the fixed label you set here (e.g. "Compiling Shaders"), never the raw line with asset paths/verbosity tags. Reword or localize freely.
- `Message_AlmostThere` - shown once progress crosses `AlmostThereThresholdPercent` (Progress Bar category), a reassuring final beat instead of the last real status line just sitting there for however long the tail end takes.

**Custom Messages**
- `bShowCustomMessages` - off by default. When on, a separate line of your own flavor text/tips crossfades above the technical status line - not tied to any real engine activity.
- `CustomMessages` - the pool to pick from (pre-populated with ~20 example lines - reword or replace freely).
- `CustomMessageIntervalSeconds` / `CustomMessageFadeDurationSeconds` - timing, same idea as the background image rotation.
- `bRandomizeCustomMessageOrder`, `CustomMessageFontSize`, `CustomMessageColor`, `bCustomMessageItalic`.

**Progress Bar**
- `bShowProgressBar`, `bShowPercentageText`.
- The bar is one global, cumulative percentage across every asset type combined (textures, static meshes, HLODs, shaders, ...) - completed-so-far divided by everything ever queued so far, clamped so it can only ever climb. A new phase starting (e.g. HLODs right as static meshes finish) can pause it for a moment if that phase adds a lot of new work, but it never visibly moves backward or resets to 0% - same principle as any download/install progress bar.
- When loading genuinely finishes, the bar is forced to 100% (still eased in smoothly, not snapped) instead of stopping wherever the last real sample happened to land - see `MinimumDisplayTimeSeconds` above for making that visible.
- `ProgressBarSmoothingSpeed` - the real underlying data arrives in chunks (a batch of assets finishes compiling and the bar would otherwise jump), so the *displayed* value eases toward the real one instead of snapping. Higher = snappier, lower = smoother.
- `AlmostThereThresholdPercent` - once the (smoothed) percentage crosses this, `Message_AlmostThere` takes over the status line.
- `ProgressBarFillColor`, `ProgressBarBackgroundColor` (the unfilled track), `ProgressBarHeight`, `ProgressBarCornerRadius` (0 = square, Height/2 = full pill/capsule) - a real custom-drawn bar, not the default engine style.
- `PercentageTextColor`, `PercentageFontSize`.

**Throbber**
- `bShowThrobber`, `ThrobberColor`, `ThrobberSize`.
- `bShowAccentStrip`, `AccentStripColor`, `AccentStripWidth` - the colored strip along the panel's left edge.

**Panel Layout**
- `bShowStatusPanel` - turn off to show just the background/title with no panel/bar at all.
- `PanelLayoutMode` - **Full Width Bottom Bar** (default): a bar spanning the entire screen width, pinned to the bottom, with the progress bar running flush edge-to-edge along the very bottom underneath a throbber/status/percentage row - the classic AAA loading-bar look. **Floating Card**: the original rounded, bordered, shadowed card, positioned by `PanelHorizontalAlignment` / `PanelVerticalAlignment` / `PanelWidth` (that trio, plus corner radius and the drop shadow, only apply in this mode - Full Width Bottom Bar ignores them since it always spans edge-to-edge and sits flush, with nowhere for a shadow to fall).
- `PanelMargin` - Floating Card: gap on all 4 sides. Full Width Bottom Bar: gap from the bottom edge only (0 = flush).
- `PanelPadding` (internal spacing on all 4 sides of the content), `PanelRowSpacing` (space between the info row and whatever's below it).
- `PanelBackgroundColor`, `PanelCornerRadius` (Floating Card only).
- `PanelBorderColor`, `PanelBorderThickness` - a thin outline so a translucent panel still reads clearly over any image (set alpha or thickness to 0 to turn it off).
- `bShowPanelShadow` (Floating Card only), `PanelShadowColor`, `PanelShadowOffset`.

**Custom Widget (Advanced)**
- `bUseCustomWidget` - master switch. Turning it on greys out every other setting above (none of it applies once you're providing your own widget) and enables `CustomWidgetBlueprint` below.
- `CustomWidgetBlueprint` - your own Widget Blueprint, replacing the entire built-in layout.

**Why this is shown differently than the built-in panel** (read this before relying on it): the built-in panel is plain native Slate with no UObject/Blueprint content, which is what lets it safely render on MoviePlayer's dedicated loading thread during a real blocking load. A Widget Blueprint is a UObject running actual Blueprint graphs, UMG bindings, and animations - none of which are safe to execute outside the game thread, and MoviePlayer's separate thread is exactly that. Earlier builds of this plugin routed a custom widget through MoviePlayer directly, which was a **confirmed crash**, not a theoretical one.

So instead, your widget is shown as a normal `AddToViewport()` overlay - added right before the freeze starts, removed once the level finishes loading, the same supported way any UMG widget is shown. It displays safely, but like the rest of the UI, it visibly freezes for the duration of the actual blocking load and resumes (animations, ticks, bindings) the moment the freeze ends - it does not animate live *through* the freeze. If you need a readout that keeps moving during the freeze itself, that's what the built-in panel is for.

## Install

1. Close Unreal Editor.
2. Copy the `LoadingMonitor` folder into your project:
   `YourProject/Plugins/LoadingMonitor/`
3. Right-click your `.uproject` and choose **Generate Visual Studio project files** if needed.
4. Open the project and allow Unreal to build the plugin, or build your Editor target from Rider/Visual Studio.
5. Make sure **Loading Monitor** is enabled under **Edit > Plugins** (plugins in a project's own `Plugins/` folder are enabled by default, but check if in doubt).

## Testing

For the actual blocked-game-thread behavior, test using **Standalone Game** or a packaged Development build. MoviePlayer loading screens are not a reliable way to judge this behavior in PIE.

## Change the background images

Drop your own PNGs (ideally 1920x1080) into `LoadingMonitor/Resources/`, then list their filenames under `BackgroundImageFilenames` in Project Settings. Rebuild/package afterward so the new resources are staged (see `RuntimeDependencies` in `LoadingMonitor.Build.cs` if you add files - they need to be listed there to ship in a packaged build).

## Important limitation

The loading UI can continue while the Game Thread is blocked because it uses Unreal's MoviePlayer/Slate loading path, which runs on its own dedicated thread. It cannot guarantee animation during a total process, render-thread, RHI, driver, or GPU hang, and it does not animate inside Editor-embedded PIE - the Editor's own window is on the same blocked message pump there, same as any other Slate UI (including the Editor's own "Compiling Shaders" notification).

## UE 5.6 compatibility notes

- Uses `#include "UObject/UObjectGlobals.h"` for `FCoreUObjectDelegates`, the correct CoreUObject public header for UE 5.6.
- The progress percentage is driven by `FAssetCompilingManager::OnAssetPostCompileEvent()`, which is explicitly declared thread-safe (`DECLARE_TS_MULTICAST_DELEGATE`) - the only place in this plugin that touches `FAssetCompilingManager` directly. Everything the Slate loading thread displays (log lines, percentage, which background image is showing) reads from the plugin's own mutex-guarded snapshot instead, never engine/UObject state directly.
