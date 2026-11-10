/* Compile the real overlay in this test translation unit so synthetic sessions
 * can exercise its private lifecycle without opening visible overlay windows. */
#include "../src/blackhole_overlay.c"

#include <stdio.h>
#include <string.h>

static int failures, notifications, sounds, releases;
static wchar_t messages[3][256];

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
        ++failures; \
    } \
} while (0)

/* No desktop capture or renderer is needed for the synthetic sessions. */
BOOL BlackHoleOverlay_CaptureDesktop(BlackHoleSession* session) {
    (void)session;
    return FALSE;
}
void BlackHoleOverlay_ReleaseDesktopCapture(BlackHoleSession* session) {
    (void)session;
    ++releases;
}
BOOL CALLBACK BlackHoleOverlay_CaptureMonitor(HMONITOR monitor, HDC dc,
                                              LPRECT rect, LPARAM context) {
    (void)monitor; (void)dc; (void)rect; (void)context;
    return FALSE;
}
BOOL BlackHoleOverlay_CreateRenderer(BlackHoleSession* session) {
    (void)session;
    return FALSE;
}
BOOL BlackHoleOverlay_CreateMonitorSurface(BlackHoleMonitor* monitor) {
    (void)monitor;
    return FALSE;
}
void BlackHoleOverlay_Render(BlackHoleMonitor* monitor, ULONGLONG elapsed) {
    (void)monitor; (void)elapsed;
}
void BlackHoleOverlay_ReleaseRenderer(BlackHoleSession* session) {
    (void)session;
}
void ShowNotification(HWND hwnd, const wchar_t* message) {
    (void)hwnd;
    if (notifications < 3) {
        wcsncpy(messages[notifications], message, 255);
        messages[notifications][255] = L'\0';
    }
    ++notifications;
}
BOOL PlayNotificationSound(HWND hwnd) {
    (void)hwnd;
    ++sounds;
    return TRUE;
}

static BlackHoleSession* NewPreview(HWND owner) {
    BlackHoleSession* session = calloc(1, sizeof(*session));
    if (!session) return NULL;
    session->hwndMain = owner;
    session->preview = TRUE;
    session->startedAt = GetTickCount64() - 100;
    g_session = session;
    return session;
}

static void TestRejectedPromotions(HWND owner, HWND other) {
    CHECK(!BlackHoleOverlay_PromotePreview(owner, L"done"));
    BlackHoleSession* session = NewPreview(owner);
    CHECK(session != NULL);
    if (!session) return;
    ULONGLONG originalStart = session->startedAt;
    CHECK(!BlackHoleOverlay_PromotePreview(other, L"done"));
    CHECK(!BlackHoleOverlay_Start(other, L"done"));
    CHECK(!BlackHoleOverlay_PromotePreview(owner, NULL));
    session->initializing = TRUE;
    CHECK(!BlackHoleOverlay_PromotePreview(owner, L"done"));
    session->initializing = FALSE;
    session->finishing = TRUE;
    CHECK(!BlackHoleOverlay_PromotePreview(owner, L"done"));
    session->finishing = FALSE;
    session->finishRequested = TRUE;
    CHECK(!BlackHoleOverlay_PromotePreview(owner, L"done"));
    session->finishRequested = FALSE;
    CHECK(session->preview && session->startedAt == originalStart);
    CHECK(session->completionMessage[0] == L'\0');
    BlackHoleOverlay_CancelPreview();
    CHECK(!g_session && releases == 1 && notifications == 0 && sounds == 0);
}

static void TestPreviewEndsSilently(HWND owner) {
    BlackHoleSession* session = NewPreview(owner);
    CHECK(session != NULL);
    if (!session) return;
    FinishOverlay(session, TRUE);
    CHECK(!g_session && !g_pendingCompletion);
    CHECK(releases == 2 && notifications == 0 && sounds == 0);
}

static void TestStartPromotesAndNotifiesOnce(HWND owner, HWND other) {
    BlackHoleSession* session = NewPreview(owner);
    CHECK(session != NULL);
    if (!session) return;
    ULONGLONG before = GetTickCount64();
    CHECK(BlackHoleOverlay_IsPreviewActive());
    CHECK(BlackHoleOverlay_Start(owner, L"work complete"));
    CHECK(g_session == session && !session->preview);
    CHECK(!BlackHoleOverlay_IsPreviewActive() && BlackHoleOverlay_IsActive());
    CHECK(session->startedAt >= before && session->startedAt <= GetTickCount64());
    CHECK(wcscmp(session->completionMessage, L"work complete") == 0);
    CHECK(!BlackHoleOverlay_PromotePreview(owner, L"again"));
    BlackHoleOverlay_CancelPreview();
    CHECK(g_session == session && releases == 2);

    FinishOverlay(session, TRUE);
    CHECK(!g_session && g_pendingCompletion == session);
    CHECK(notifications == 0 && sounds == 0);
    MSG message;
    BOOL posted = PeekMessageW(&message, owner, WM_APP_BLACK_HOLE_COMPLETE,
                               WM_APP_BLACK_HOLE_COMPLETE, PM_REMOVE);
    CHECK(posted);
    if (!posted) { BlackHoleOverlay_Cancel(); return; }
    BlackHoleOverlay_HandleCompletion(other, message.lParam);
    CHECK(g_pendingCompletion == session && notifications == 0);
    BlackHoleOverlay_HandleCompletion(owner, message.lParam);
    CHECK(!g_pendingCompletion && releases == 3);
    CHECK(notifications == 1 && sounds == 1);
    CHECK(wcscmp(messages[0], L"work complete") == 0);
    BlackHoleOverlay_HandleCompletion(owner, message.lParam);
    CHECK(notifications == 1 && sounds == 1 && releases == 3);
}

static void TestCancelPending(HWND owner) {
    BlackHoleSession* session = NewPreview(owner);
    CHECK(session != NULL);
    if (!session) return;
    CHECK(BlackHoleOverlay_PromotePreview(owner, L"first"));
    FinishOverlay(session, TRUE);
    CHECK(g_pendingCompletion == session);
    MSG message;
    BOOL posted = PeekMessageW(&message, owner, WM_APP_BLACK_HOLE_COMPLETE,
                               WM_APP_BLACK_HOLE_COMPLETE, PM_REMOVE);
    CHECK(posted);
    BlackHoleOverlay_Cancel();
    if (posted) BlackHoleOverlay_HandleCompletion(owner, message.lParam);
    CHECK(!g_pendingCompletion && releases == 4);
    CHECK(notifications == 1 && sounds == 1);
}

static void TestPendingCompletionSurvivesPreviewCancellation(HWND owner) {
    BlackHoleSession* session = NewPreview(owner);
    CHECK(session != NULL);
    if (!session) return;
    CHECK(BlackHoleOverlay_PromotePreview(owner, L"first"));
    FinishOverlay(session, TRUE);
    CHECK(g_pendingCompletion == session);
    MSG message;
    BOOL posted = PeekMessageW(&message, owner, WM_APP_BLACK_HOLE_COMPLETE,
                               WM_APP_BLACK_HOLE_COMPLETE, PM_REMOVE);
    CHECK(posted);
    if (!posted) { BlackHoleOverlay_Cancel(); return; }

    session = NewPreview(owner);
    CHECK(session != NULL);
    if (!session) { BlackHoleOverlay_Cancel(); return; }
    BlackHoleOverlay_CancelPreview();
    CHECK(!g_session && g_pendingCompletion != NULL);
    BlackHoleOverlay_HandleCompletion(owner, message.lParam);
    CHECK(!g_pendingCompletion && releases == 6);
    CHECK(notifications == 2 && sounds == 2);
    CHECK(wcscmp(messages[1], L"first") == 0);
}

int main(void) {
    HWND owner = CreateWindowExW(0, L"STATIC", L"", WS_OVERLAPPED,
                                 0, 0, 0, 0, NULL, NULL, GetModuleHandleW(NULL), NULL);
    HWND other = CreateWindowExW(0, L"STATIC", L"", WS_OVERLAPPED,
                                 0, 0, 0, 0, NULL, NULL, GetModuleHandleW(NULL), NULL);
    if (!owner || !other) {
        fprintf(stderr, "failed to create hidden test windows\n");
        if (owner) DestroyWindow(owner);
        if (other) DestroyWindow(other);
        return 1;
    }
    TestRejectedPromotions(owner, other);
    TestPreviewEndsSilently(owner);
    TestStartPromotesAndNotifiesOnce(owner, other);
    TestCancelPending(owner);
    TestPendingCompletionSurvivesPreviewCancellation(owner);
    BlackHoleOverlay_Cancel();
    DestroyWindow(other);
    DestroyWindow(owner);
    if (!failures) puts("black-hole preview lifecycle tests passed");
    return failures ? 1 : 0;
}