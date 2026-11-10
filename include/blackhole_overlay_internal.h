#ifndef CATIME_BLACKHOLE_OVERLAY_INTERNAL_H
#define CATIME_BLACKHOLE_OVERLAY_INTERNAL_H

#include <windows.h>

#define BLACK_HOLE_MAX_MONITORS 16
#define BLACK_HOLE_DURATION_MS 30000ULL

typedef struct BlackHoleSession BlackHoleSession;

typedef struct {
    RECT rect;
    HWND hwnd;
    BlackHoleSession* session;
    void* surface;
} BlackHoleMonitor;

struct BlackHoleSession {
    HWND hwndMain;
    wchar_t completionMessage[256];
    ULONGLONG startedAt;
    int monitorCount;
    BOOL finishing;
    BOOL preview;
    BOOL initializing;
    BOOL initializationCancelled;
    unsigned int rendering;
    BOOL finishRequested;
    BOOL finishNotify;
    UINT_PTR completionToken;
    BlackHoleSession* nextCompletion;
    RECT desktopRect;
    HDC snapshotDc;
    HBITMAP snapshot;
    HBITMAP previousSnapshot;
    void* snapshotPixels;
    int desktopWidth;
    int desktopHeight;
    void* renderer;
    BlackHoleMonitor monitors[BLACK_HOLE_MAX_MONITORS];
};

BOOL BlackHoleOverlay_CaptureDesktop(BlackHoleSession* session);
void BlackHoleOverlay_ReleaseDesktopCapture(BlackHoleSession* session);
BOOL CALLBACK BlackHoleOverlay_CaptureMonitor(HMONITOR hMonitor, HDC hdc,
                                              LPRECT unused, LPARAM context);
BOOL BlackHoleOverlay_CreateRenderer(BlackHoleSession* session);
BOOL BlackHoleOverlay_CreateMonitorSurface(BlackHoleMonitor* monitor);
void BlackHoleOverlay_Render(BlackHoleMonitor* monitor, ULONGLONG elapsedMs);
void BlackHoleOverlay_ReleaseRenderer(BlackHoleSession* session);

#endif /* CATIME_BLACKHOLE_OVERLAY_INTERNAL_H */