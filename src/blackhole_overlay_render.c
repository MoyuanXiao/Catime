/**
 * @file blackhole_overlay_render.c
 * @brief In-memory desktop capture support for the black-hole overlay.
 */

#include "blackhole_overlay_internal.h"

BOOL BlackHoleOverlay_CaptureDesktop(BlackHoleSession* session) {
    if (!session) return FALSE;
    RECT desktop = {
        GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
        0, 0
    };
    desktop.right = desktop.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    desktop.bottom = desktop.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    int width = desktop.right - desktop.left;
    int height = desktop.bottom - desktop.top;
    if (width <= 0 || height <= 0) return FALSE;

    HDC screenDc = GetDC(NULL);
    HDC memoryDc = screenDc ? CreateCompatibleDC(screenDc) : NULL;
    BITMAPINFO info;
    ZeroMemory(&info, sizeof(info));
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = NULL;
    HBITMAP bitmap = memoryDc
        ? CreateDIBSection(screenDc, &info, DIB_RGB_COLORS, &bits, NULL, 0) : NULL;
    HBITMAP previous = bitmap ? (HBITMAP)SelectObject(memoryDc, bitmap) : NULL;
    BOOL captured = previous && previous != (HBITMAP)HGDI_ERROR &&
        BitBlt(memoryDc, 0, 0, width, height, screenDc,
               desktop.left, desktop.top, SRCCOPY | CAPTUREBLT);
    if (screenDc) ReleaseDC(NULL, screenDc);
    if (!captured) {
        if (previous && previous != (HBITMAP)HGDI_ERROR && memoryDc) {
            SelectObject(memoryDc, previous);
        }
        if (bitmap) DeleteObject(bitmap);
        if (memoryDc) DeleteDC(memoryDc);
        return FALSE;
    }
    session->desktopRect = desktop;
    session->snapshotDc = memoryDc;
    session->snapshot = bitmap;
    session->previousSnapshot = previous;
    session->snapshotPixels = bits;
    session->desktopWidth = width;
    session->desktopHeight = height;
    return TRUE;
}

void BlackHoleOverlay_ReleaseDesktopCapture(BlackHoleSession* session) {
    if (!session) return;
    if (session->snapshotDc && session->previousSnapshot) {
        SelectObject(session->snapshotDc, session->previousSnapshot);
    }
    if (session->snapshot) DeleteObject(session->snapshot);
    if (session->snapshotDc) DeleteDC(session->snapshotDc);
    session->snapshotDc = NULL;
    session->snapshot = NULL;
    session->previousSnapshot = NULL;
    session->snapshotPixels = NULL;
    session->desktopWidth = 0;
    session->desktopHeight = 0;
}

BOOL CALLBACK BlackHoleOverlay_CaptureMonitor(HMONITOR hMonitor, HDC hdc,
                                              LPRECT unused, LPARAM context) {
    (void)hdc;
    (void)unused;
    BlackHoleSession* session = (BlackHoleSession*)context;
    if (!session || session->monitorCount >= BLACK_HOLE_MAX_MONITORS) return FALSE;

    MONITORINFO info;
    ZeroMemory(&info, sizeof(info));
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(hMonitor, &info)) return FALSE;

    RECT rect = info.rcMonitor;
    int width = rect.right - rect.left;
    int height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0) return FALSE;

    BlackHoleMonitor* monitor = &session->monitors[session->monitorCount++];
    monitor->rect = rect;
    monitor->session = session;
    return TRUE;
}
