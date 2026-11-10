/**
 * @file blackhole_overlay.h
 * @brief Short-lived, input-blocking desktop snapshot effect for Pomodoro breaks.
 */

#ifndef CATIME_BLACKHOLE_OVERLAY_H
#define CATIME_BLACKHOLE_OVERLAY_H

#include <windows.h>

/* All overlay APIs must be called on the main window's UI thread. */
BOOL BlackHoleOverlay_Start(HWND hwndMain, const wchar_t* completionMessage);
/* Start a 30-second preview without consulting settings or changing timer state.
 * Returns FALSE if another overlay is active or initialization fails. Esc,
 * timeout, display changes and cancellation end it without notification/sound.
 * A subsequent formal Start promotes the preview using its existing snapshot. */
BOOL BlackHoleOverlay_StartPreview(HWND hwndMain);
BOOL BlackHoleOverlay_IsPreviewActive(void);
/* Promote only an existing preview to a formal effect. */
BOOL BlackHoleOverlay_PromotePreview(HWND hwndMain, const wchar_t* completionMessage);
/* Dismiss a preview without cancelling pending formal completion notifications. */
void BlackHoleOverlay_CancelPreview(void);
void BlackHoleOverlay_Cancel(void);
BOOL BlackHoleOverlay_IsActive(void);
void BlackHoleOverlay_HandleCompletion(HWND hwndMain, LPARAM completionData);

#endif /* CATIME_BLACKHOLE_OVERLAY_H */