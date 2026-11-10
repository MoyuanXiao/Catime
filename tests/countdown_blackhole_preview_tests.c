#include "timer_events_internal.h"
#include "blackhole_overlay.h"

#include <stdio.h>
#include <string.h>

AppConfig g_AppConfig;
bool CLOCK_IS_PAUSED, CLOCK_SHOW_CURRENT_TIME, CLOCK_COUNT_UP;
bool countdown_message_shown;
BOOL CLOCK_IS_DRAGGING;
int32_t CLOCK_TOTAL_TIME, countdown_elapsed_time, countup_elapsed_time;
int32_t last_displayed_second;
int64_t g_target_end_time, g_start_time;
TimeoutActionType CLOCK_TIMEOUT_ACTION;
wchar_t g_visibleTimerCurrentText[TIME_TEXT_MAX_LEN];
wchar_t g_lastPaintedTimerText[TIME_TEXT_MAX_LEN];
BOOL g_hasLastPaintedTimerText;

static int failures;
static BOOL preview_active, formal_active, pending_completion, pomodoro_active;
static BOOL system_action_handled;
static int64_t now_ms;
static char actions[64];
static size_t action_count;

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
        ++failures; \
    } \
} while (0)

static void Record(char action) {
    if (action_count + 1 < sizeof(actions)) {
        actions[action_count++] = action;
        actions[action_count] = '\0';
    }
}

/* Model preview cancellation independently from a formal/pending effect. */
void BlackHoleOverlay_CancelPreview(void) {
    Record('C');
    preview_active = FALSE;
}
void TimerEvents_ShowTimeoutNotification(HWND hwnd, const char* message, BOOL playSound) {
    (void)hwnd;
    CHECK(strcmp(message, "timeout") == 0 && playSound);
    CHECK(!preview_active);
    Record('N');
}
BOOL TimerEvents_IsActivePomodoroTimer(void) { return pomodoro_active; }
BOOL TimerEvents_HandlePomodoroCompletion(HWND hwnd) {
    (void)hwnd;
    Record('P');
    return TRUE;
}
void ResetPomodoroState(void) { Record('R'); }
BOOL TimerEvents_ExecuteSystemAction(HWND hwnd, TimeoutActionType action) {
    (void)hwnd; (void)action;
    CHECK(!preview_active);
    Record('S');
    return system_action_handled;
}
void TimerEvents_HandleTimeoutActions(HWND hwnd) {
    (void)hwnd;
    CHECK(!preview_active);
    Record('A');
    if (CLOCK_TIMEOUT_ACTION == TIMEOUT_ACTION_SHOW_TIME) CLOCK_SHOW_CURRENT_TIME = true;
    if (CLOCK_TIMEOUT_ACTION == TIMEOUT_ACTION_COUNT_UP) CLOCK_COUNT_UP = true;
}
void TimerEvents_ResetTimerState(int seconds) {
    Record('Z');
    CLOCK_TOTAL_TIME = seconds;
    countdown_elapsed_time = 0;
}
void TimerEvents_ResetMillisecondAccumulator(void) { Record('M'); }
void TrayAnimation_RecomputeTimerDelay(void) { Record('D'); }
void TimerEvents_RequestWindowRepaint(HWND hwnd) { (void)hwnd; }
void GetTimeText(wchar_t* buffer, size_t size) { if (size) buffer[0] = L'\0'; }
BOOL GetActiveShowMilliseconds(void) { return FALSE; }
UINT GetTimerInterval(void) { return 100; }
void MainTimer_SetInterval(UINT interval) { (void)interval; }
void TryRestorePendingWindowPosition(HWND hwnd) { (void)hwnd; }
BOOL EnforceTopmostOverTaskbar(HWND hwnd) { (void)hwnd; return TRUE; }
int64_t GetAbsoluteTimeMs(void) { return now_ms; }

static void SetUp(TimeoutActionType action) {
    memset(&g_AppConfig, 0, sizeof(g_AppConfig));
    strcpy(g_AppConfig.notification.messages.timeout_message, "timeout");
    action_count = 0;
    actions[0] = '\0';
    preview_active = TRUE;
    formal_active = pending_completion = pomodoro_active = FALSE;
    system_action_handled = FALSE;
    CLOCK_IS_DRAGGING = TRUE;
    CLOCK_IS_PAUSED = CLOCK_SHOW_CURRENT_TIME = CLOCK_COUNT_UP = false;
    countdown_message_shown = false;
    CLOCK_TOTAL_TIME = 10;
    countdown_elapsed_time = countup_elapsed_time = 0;
    g_start_time = 0;
    g_target_end_time = 10000;
    now_ms = 10000;
    CLOCK_TIMEOUT_ACTION = action;
}

static void TestMessageAndNoPreview(void) {
    for (int preview = 0; preview <= 1; ++preview) {
        SetUp(TIMEOUT_ACTION_MESSAGE);
        preview_active = preview;
        formal_active = !preview;
        pending_completion = TRUE;
        CHECK(TimerEvents_HandleMainTimer(NULL));
        CHECK(strcmp(actions, "DCNRSAZM") == 0);
        CHECK(!preview_active && formal_active == !preview && pending_completion);
        CHECK(countdown_message_shown && CLOCK_TOTAL_TIME == 0);
        CHECK(countdown_elapsed_time == 0);
        CHECK(TimerEvents_HandleMainTimer(NULL));
        CHECK(strcmp(actions, "DCNRSAZM") == 0);
    }
}

static void TestOtherTimeoutActions(void) {
    const TimeoutActionType actions_to_test[] = {
        TIMEOUT_ACTION_LOCK, TIMEOUT_ACTION_OPEN_FILE, TIMEOUT_ACTION_OPEN_WEBSITE,
        TIMEOUT_ACTION_SHOW_TIME, TIMEOUT_ACTION_COUNT_UP
    };
    for (size_t i = 0; i < _countof(actions_to_test); ++i) {
        SetUp(actions_to_test[i]);
        CHECK(TimerEvents_HandleMainTimer(NULL));
        CHECK(!preview_active && countdown_message_shown);
        if (actions_to_test[i] == TIMEOUT_ACTION_SHOW_TIME ||
            actions_to_test[i] == TIMEOUT_ACTION_COUNT_UP) {
            CHECK(strcmp(actions, "DCRSA") == 0);
            CHECK(CLOCK_TOTAL_TIME == 10 && countdown_elapsed_time == 10);
            CHECK(CLOCK_SHOW_CURRENT_TIME == (actions_to_test[i] == TIMEOUT_ACTION_SHOW_TIME));
            CHECK(CLOCK_COUNT_UP == (actions_to_test[i] == TIMEOUT_ACTION_COUNT_UP));
        } else {
            CHECK(strcmp(actions, "DCRSAZM") == 0);
            CHECK(CLOCK_TOTAL_TIME == 0);
        }
    }
    SetUp(TIMEOUT_ACTION_SHUTDOWN);
    system_action_handled = TRUE;
    CHECK(TimerEvents_HandleMainTimer(NULL));
    CHECK(strcmp(actions, "DCRS") == 0);
    CHECK(!preview_active && CLOCK_TOTAL_TIME == 10 && countdown_elapsed_time == 10);
}

static void TestNonTimeoutTicks(void) {
    SetUp(TIMEOUT_ACTION_MESSAGE);
    now_ms = 9999;
    CHECK(TimerEvents_HandleMainTimer(NULL));
    CHECK(actions[0] == '\0' && preview_active && countdown_elapsed_time == 9);

    SetUp(TIMEOUT_ACTION_MESSAGE);
    CLOCK_IS_PAUSED = true;
    CHECK(TimerEvents_HandleMainTimer(NULL));
    CHECK(actions[0] == '\0' && preview_active && countdown_elapsed_time == 0);

    SetUp(TIMEOUT_ACTION_MESSAGE);
    CLOCK_SHOW_CURRENT_TIME = true;
    CHECK(TimerEvents_HandleMainTimer(NULL));
    CHECK(actions[0] == '\0' && preview_active);

    SetUp(TIMEOUT_ACTION_MESSAGE);
    CLOCK_COUNT_UP = true;
    CHECK(TimerEvents_HandleMainTimer(NULL));
    CHECK(actions[0] == '\0' && preview_active && countup_elapsed_time == 10);

    SetUp(TIMEOUT_ACTION_MESSAGE);
    CLOCK_TOTAL_TIME = 0;
    CHECK(TimerEvents_HandleMainTimer(NULL));
    CHECK(actions[0] == '\0' && preview_active);

    SetUp(TIMEOUT_ACTION_MESSAGE);
    countdown_message_shown = true;
    CHECK(TimerEvents_HandleMainTimer(NULL));
    CHECK(actions[0] == '\0' && preview_active && countdown_elapsed_time == 10);
}

static void TestPomodoroDispatchIsUnchanged(void) {
    SetUp(TIMEOUT_ACTION_MESSAGE);
    pomodoro_active = TRUE;
    CHECK(TimerEvents_HandleMainTimer(NULL));
    CHECK(strcmp(actions, "DP") == 0 && preview_active);
    CHECK(countdown_message_shown && CLOCK_TOTAL_TIME == 10);
}

int main(void) {
    TestMessageAndNoPreview();
    TestOtherTimeoutActions();
    TestNonTimeoutTicks();
    TestPomodoroDispatchIsUnchanged();
    if (!failures) puts("countdown black-hole preview tests passed");
    return failures ? 1 : 0;
}