/**
 * @file blackhole_overlay.c
 * @brief In-memory multi-monitor black-hole overlay.
 */

#include "blackhole_overlay.h"
#include "blackhole_overlay_internal.h"

#include <stdlib.h>
#include <wchar.h>

#include "audio_player.h"
#include "notification.h"
#include "window_procedure/window_procedure.h"

#define BLACK_HOLE_CLASS_NAME L"CatimeBlackHoleOverlay"
#define BLACK_HOLE_FRAME_TIMER 48151

static BlackHoleSession* g_session;
static BlackHoleSession* g_pendingCompletion;
static ATOM g_classAtom;
static UINT_PTR g_nextCompletionToken;
static ULONGLONG g_completionGeneration;

static void ReleaseSession(BlackHoleSession* session) {
    if (!session) return;
    BlackHoleOverlay_ReleaseRenderer(session);
    BlackHoleOverlay_ReleaseDesktopCapture(session);
    free(session);
}

static UINT_PTR NextCompletionToken(void) {
    BOOL used;
    do {
        if (++g_nextCompletionToken == 0) ++g_nextCompletionToken;
        used = FALSE;
        for (BlackHoleSession* item = g_pendingCompletion; item; item = item->nextCompletion) {
            if (item->completionToken == g_nextCompletionToken) used = TRUE;
        }
    } while (used);
    return g_nextCompletionToken;
}

static void NotifyCompletion(BlackHoleSession* session) {
    if (IsWindow(session->hwndMain)) {
        ShowNotification(session->hwndMain, session->completionMessage);
        PlayNotificationSound(session->hwndMain);
    }
    ReleaseSession(session);
}

static void FinishOverlay(BlackHoleSession* session, BOOL notify) {
    if (!session || session->finishing) return;
    if (session->initializing) {
        session->initializationCancelled = TRUE;
        return;
    }
    if (session->rendering) {
        if (!session->finishRequested) session->finishNotify = notify;
        else if (!notify) session->finishNotify = FALSE;
        session->finishRequested = TRUE;
        return;
    }
    session->finishing = TRUE;
    ULONGLONG completionGeneration = g_completionGeneration;

    for (int i = 0; i < session->monitorCount; ++i) {
        HWND hwnd = session->monitors[i].hwnd;
        session->monitors[i].hwnd = NULL;
        if (hwnd && IsWindow(hwnd)) {
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            DestroyWindow(hwnd);
        }
    }
    if (g_session == session) g_session = NULL;
    if (notify && !session->preview && IsWindow(session->hwndMain) &&
        completionGeneration == g_completionGeneration) {
        session->completionToken = NextCompletionToken();
        if (PostMessageW(session->hwndMain, WM_APP_BLACK_HOLE_COMPLETE, 0,
                         (LPARAM)session->completionToken)) {
            session->nextCompletion = g_pendingCompletion;
            g_pendingCompletion = session;
        } else {
            NotifyCompletion(session);
        }
        return;
    }
    ReleaseSession(session);
}

static LRESULT CALLBACK BlackHoleWindowProc(HWND hwnd, UINT message,
                                             WPARAM wParam, LPARAM lParam) {
    BlackHoleMonitor* monitor = (BlackHoleMonitor*)GetWindowLongPtrW(
        hwnd, GWLP_USERDATA);
    BlackHoleSession* session = monitor ? monitor->session : NULL;

    switch (message) {
        case WM_NCCREATE: {
            CREATESTRUCTW* create = (CREATESTRUCTW*)lParam;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              (LONG_PTR)create->lpCreateParams);
            return TRUE;
        }
        case WM_NCHITTEST:
            return HTCLIENT;
        case WM_MOUSEACTIVATE:
            return MA_ACTIVATE;
        case WM_CLOSE:
            return 0;
        case WM_SYSCOMMAND:
            if ((wParam & 0xFFF0) == SC_CLOSE) return 0;
            break;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (wParam == VK_ESCAPE && session) FinishOverlay(session, TRUE);
            return 0;
        case WM_DISPLAYCHANGE:
        case WM_DPICHANGED:
            if (session) FinishOverlay(session, TRUE);
            return 0;
        case WM_TIMER:
            if (wParam == BLACK_HOLE_FRAME_TIMER && session) {
                ULONGLONG elapsed = GetTickCount64() - session->startedAt;
                if (elapsed >= BLACK_HOLE_DURATION_MS) FinishOverlay(session, TRUE);
                else InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
            break;
        case WM_PAINT:
            if (monitor && session) {
                PAINTSTRUCT paint;
                ++session->rendering;
                HDC paintDc = BeginPaint(hwnd, &paint);
                if (!session->finishRequested && !session->initializationCancelled) {
                    BlackHoleOverlay_Render(monitor, GetTickCount64() - session->startedAt);
                }
                if (paintDc) EndPaint(hwnd, &paint);
                --session->rendering;
                if (!session->rendering && session->finishRequested) {
                    FinishOverlay(session, session->finishNotify);
                }
                return 0;
            }
            break;
        case WM_ERASEBKGND:
            return 1;
        case WM_DESTROY:
            KillTimer(hwnd, BLACK_HOLE_FRAME_TIMER);
            if (monitor && monitor->hwnd == hwnd) monitor->hwnd = NULL;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            if (session && !session->finishing) FinishOverlay(session, TRUE);
            return 0;
        case WM_NCDESTROY:
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

static BOOL RegisterOverlayClass(void) {
    if (g_classAtom) return TRUE;
    WNDCLASSW windowClass = {0};
    windowClass.lpfnWndProc = BlackHoleWindowProc;
    windowClass.hInstance = GetModuleHandleW(NULL);
    windowClass.hCursor = LoadCursor(NULL, IDC_ARROW);
    windowClass.lpszClassName = BLACK_HOLE_CLASS_NAME;
    g_classAtom = RegisterClassW(&windowClass);
    return g_classAtom != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

BOOL BlackHoleOverlay_PromotePreview(HWND hwndMain, const wchar_t* completionMessage) {
    BlackHoleSession* session = g_session;
    if (!session || !session->preview || session->initializing || session->finishing ||
        session->finishRequested ||
        session->hwndMain != hwndMain || !completionMessage || !IsWindow(hwndMain)) return FALSE;
    wcsncpy(session->completionMessage, completionMessage,
            _countof(session->completionMessage) - 1);
    session->preview = FALSE;
    session->startedAt = GetTickCount64();
    for (int i = 0; i < session->monitorCount; ++i) {
        InvalidateRect(session->monitors[i].hwnd, NULL, FALSE);
    }
    return TRUE;
}

static BOOL StartSession(HWND hwndMain, const wchar_t* completionMessage, BOOL preview) {
    if (!IsWindow(hwndMain) || (!preview && !completionMessage) || g_session ||
        !RegisterOverlayClass()) return FALSE;

    BlackHoleSession* session = (BlackHoleSession*)calloc(1, sizeof(*session));
    if (!session) return FALSE;
    session->hwndMain = hwndMain;
    session->preview = preview;
    if (completionMessage) {
        wcsncpy(session->completionMessage, completionMessage,
                _countof(session->completionMessage) - 1);
    }

    if (!BlackHoleOverlay_CaptureDesktop(session) ||
        !EnumDisplayMonitors(NULL, NULL, BlackHoleOverlay_CaptureMonitor,
                             (LPARAM)session) ||
        session->monitorCount == 0) {
        ReleaseSession(session);
        return FALSE;
    }

    if (!BlackHoleOverlay_CreateRenderer(session)) {
        ReleaseSession(session);
        return FALSE;
    }

    g_session = session;
    session->initializing = TRUE;
    session->startedAt = GetTickCount64();
    for (int i = 0; i < session->monitorCount; ++i) {
        BlackHoleMonitor* monitor = &session->monitors[i];
        int width = monitor->rect.right - monitor->rect.left;
        int height = monitor->rect.bottom - monitor->rect.top;
        monitor->hwnd = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW, BLACK_HOLE_CLASS_NAME, L"",
            WS_POPUP | WS_VISIBLE, monitor->rect.left, monitor->rect.top,
            width, height, NULL, NULL, GetModuleHandleW(NULL), monitor);
        if (!monitor->hwnd || !IsWindow(monitor->hwnd) || session->initializationCancelled ||
            !BlackHoleOverlay_CreateMonitorSurface(monitor) ||
            session->initializationCancelled ||
            !SetTimer(monitor->hwnd, BLACK_HOLE_FRAME_TIMER, 16, NULL)) goto fail;
        if (!SetWindowPos(monitor->hwnd, HWND_TOPMOST, monitor->rect.left,
                          monitor->rect.top, width, height,
                          SWP_SHOWWINDOW | SWP_NOOWNERZORDER) ||
            session->initializationCancelled) goto fail;
    }
    SetForegroundWindow(session->monitors[0].hwnd);
    if (session->initializationCancelled) goto fail;
    SetFocus(session->monitors[0].hwnd);
    if (session->initializationCancelled) goto fail;
    session->startedAt = GetTickCount64();
    session->initializing = FALSE;
    return TRUE;

fail:
    session->initializing = FALSE;
    FinishOverlay(session, FALSE);
    return FALSE;
}

BOOL BlackHoleOverlay_Start(HWND hwndMain, const wchar_t* completionMessage) {
    if (BlackHoleOverlay_IsPreviewActive()) {
        return BlackHoleOverlay_PromotePreview(hwndMain, completionMessage);
    }
    return StartSession(hwndMain, completionMessage, FALSE);
}

BOOL BlackHoleOverlay_StartPreview(HWND hwndMain) {
    return StartSession(hwndMain, NULL, TRUE);
}

BOOL BlackHoleOverlay_IsPreviewActive(void) {
        return g_session && g_session->preview && !g_session->initializing &&
            !g_session->finishing && !g_session->finishRequested;
}

void BlackHoleOverlay_CancelPreview(void) {
    if (g_session && g_session->preview) FinishOverlay(g_session, FALSE);
}

void BlackHoleOverlay_Cancel(void) {
    ++g_completionGeneration;
    FinishOverlay(g_session, FALSE);
    while (g_pendingCompletion) {
        BlackHoleSession* session = g_pendingCompletion;
        g_pendingCompletion = session->nextCompletion;
        ReleaseSession(session);
    }
}

BOOL BlackHoleOverlay_IsActive(void) {
    return g_session != NULL;
}

void BlackHoleOverlay_HandleCompletion(HWND hwndMain, LPARAM completionData) {
    BlackHoleSession** link = &g_pendingCompletion;
    while (*link && ((*link)->completionToken != (UINT_PTR)completionData ||
                     (*link)->hwndMain != hwndMain)) {
        link = &(*link)->nextCompletion;
    }
    if (!*link) return;
    BlackHoleSession* session = *link;
    *link = session->nextCompletion;
    NotifyCompletion(session);
}