// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"

/**
 * Opens a standalone Slate window showing the loading screen exactly as it would appear for a real
 * map load - same widget, same background rotation/crossfade, same fonts and colors from
 * ULoadingMonitorSettings - but driven by a simulated progress feed instead of a real compile, and
 * running on the normal editor Slate loop (there's nothing to work around here: unlike a real load,
 * nothing is blocking the game thread, so no MoviePlayer/dedicated thread is needed just to look at it).
 *
 * Editor-only. Safe to call repeatedly - reopens/refreshes a single preview window rather than
 * stacking up duplicates.
 */
LOADINGMONITOR_API void OpenLoadingMonitorPreviewWindow();
