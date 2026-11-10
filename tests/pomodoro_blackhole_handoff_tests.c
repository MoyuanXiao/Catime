#include "timer_events_internal.h"
#include "blackhole_overlay.h"

#include <stdio.h>
#include <string.h>
#include <wchar.h>

AppConfig g_AppConfig;
POMODORO_PHASE current_pomodoro_phase;
int current_pomodoro_time_index, complete_pomodoro_cycles;
int pomodoro_initial_times_count, pomodoro_initial_loop_count;
int pomodoro_initial_times[MAX_POMODORO_TIMES];
int32_t CLOCK_TOTAL_TIME, countdown_elapsed_time, message_shown;
int64_t g_target_end_time;
bool CLOCK_COUNT_UP, CLOCK_SHOW_CURRENT_TIME, countdown_message_shown;

static int failures, start_calls, cancel_calls, sound_calls, notification_calls;
static int promotion_calls, reset_duration, timer_stops, timer_initializations;
static BOOL preview_active, formal_active, start_succeeds;
static wchar_t start_message[256], notifications[2][256];
static char actions[32];
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

/* The overlay is a fake: it records the timer's requests, not real rendering. */
BOOL BlackHoleOverlay_Start(HWND hwnd, const wchar_t* message) {
    (void)hwnd;
    ++start_calls;
    Record('S');
    wcsncpy(start_message, message, 255);
    start_message[255] = L'\0';
    if (!start_succeeds) return FALSE;
    if (preview_active) ++promotion_calls;
    preview_active = FALSE;
    formal_active = TRUE;
    return TRUE;
}

void BlackHoleOverlay_CancelPreview(void) {
    ++cancel_calls;
    Record('C');
    preview_active = FALSE;
}

void ShowNotification(HWND hwnd, const wchar_t* message) {
    (void)hwnd;
    if (notification_calls < 2) {
        wcsncpy(notifications[notification_calls], message, 255);
        notifications[notification_calls][255] = L'\0';
    }
    ++notification_calls;
    Record('N');
}

BOOL PlayNotificationSound(HWND hwnd) {
    (void)hwnd;
    ++sound_calls;
    Record('P');
    return TRUE;
}

const wchar_t* GetLocalizedString(const wchar_t* chinese,
                                  const wchar_t* english) {
    (void)chinese;
    return english;
}

void TimerEvents_ResetTimerState(int seconds) {
    reset_duration = seconds;
    CLOCK_TOTAL_TIME = seconds;
    countdown_elapsed_time = 0;
    Record('R');
}

void TimerEvents_ResetMillisecondAccumulator(void) {}
void InitializeHighPrecisionTimer(void) { ++timer_initializations; }
void MainTimer_Stop(void) { ++timer_stops; Record('T'); }

static void SetUp(int index, int count, int loops, int cycle,
                  BOOL enabled, BOOL preview, BOOL succeeds) {
    memset(&g_AppConfig, 0, sizeof(g_AppConfig));
    memset(pomodoro_initial_times, 0, sizeof(pomodoro_initial_times));
    memset(notifications, 0, sizeof(notifications));
    start_message[0] = L'\0';
    actions[0] = '\0';
    action_count = 0;
    start_calls = cancel_calls = sound_calls = notification_calls = 0;
    promotion_calls = timer_stops = timer_initializations = 0;
    reset_duration = -1;
    preview_active = preview;
    formal_active = FALSE;
    start_succeeds = succeeds;
    g_AppConfig.notification.display.black_hole_enabled = enabled;
    pomodoro_initial_times_count = count;
    pomodoro_initial_loop_count = loops;
    pomodoro_initial_times[0] = 1500;
    pomodoro_initial_times[1] = 300;
    pomodoro_initial_times[2] = 1200;
    current_pomodoro_time_index = index;
    current_pomodoro_phase = (index % 2) ? POMODORO_PHASE_BREAK : POMODORO_PHASE_WORK;
    complete_pomodoro_cycles = cycle;
    CLOCK_TOTAL_TIME = pomodoro_initial_times[index];
    g_target_end_time = 100000;
    CLOCK_COUNT_UP = CLOCK_SHOW_CURRENT_TIME = true;
    countdown_message_shown = true;
    message_shown = FALSE;
}

static void CheckNext(int expected_index, int seconds) {
    CHECK(current_pomodoro_time_index == expected_index);
    CHECK(reset_duration == seconds);
    CHECK(g_target_end_time == 100000 + (int64_t)seconds * 1000);
    CHECK(!countdown_message_shown);
    CHECK(timer_initializations == 1);
    CHECK(timer_stops == 0);
}

static void TestDisabledCancelsPreview(void) {
    SetUp(0, 2, 1, 0, FALSE, TRUE, TRUE);
    CHECK(TimerEvents_HandlePomodoroCompletion(NULL));
    CHECK(strcmp(actions, "CNPR") == 0);
    CHECK(start_calls == 0 && cancel_calls == 1 && !preview_active);
    CHECK(notification_calls == 1 && sound_calls == 1);
    CHECK(wcscmp(notifications[0], L"25m Pomodoro completed (Cycle1/1Round 1/2)") == 0);
    CHECK(current_pomodoro_phase == POMODORO_PHASE_BREAK);
    CheckNext(1, 300);
}

static void TestWorkToBreakStartsFormal(void) {
    SetUp(0, 2, 1, 0, TRUE, FALSE, TRUE);
    CHECK(TimerEvents_HandlePomodoroCompletion(NULL));
    CHECK(strcmp(actions, "SR") == 0);
    CHECK(start_calls == 1 && formal_active && promotion_calls == 0);
    CHECK(cancel_calls == 0 && notification_calls == 0 && sound_calls == 0);
    CHECK(wcscmp(start_message, L"25m Pomodoro completed (Cycle1/1Round 1/2)") == 0);
    CHECK(current_pomodoro_phase == POMODORO_PHASE_BREAK);
    CheckNext(1, 300);
}

static void TestPreviewHandoff(void) {
    SetUp(0, 2, 1, 0, TRUE, TRUE, TRUE);
    CHECK(TimerEvents_HandlePomodoroCompletion(NULL));
    CHECK(strcmp(actions, "SR") == 0);
    CHECK(start_calls == 1 && promotion_calls == 1);
    CHECK(!preview_active && formal_active && cancel_calls == 0);
    CHECK(notification_calls == 0 && sound_calls == 0);
    CHECK(wcscmp(start_message, L"25m Pomodoro completed (Cycle1/1Round 1/2)") == 0);
    CheckNext(1, 300);
}

static void TestFailedStartFallsBack(void) {
    for (int preview = 0; preview <= 1; ++preview) {
        SetUp(0, 2, 1, 0, TRUE, preview, FALSE);
        CHECK(TimerEvents_HandlePomodoroCompletion(NULL));
        CHECK(strcmp(actions, "SCNPR") == 0);
        CHECK(start_calls == 1 && cancel_calls == 1 && !preview_active);
        CHECK(!formal_active && promotion_calls == 0);
        CHECK(notification_calls == 1 && sound_calls == 1);
        CHECK(wcscmp(start_message, notifications[0]) == 0);
        CheckNext(1, 300);
    }
}

static void TestBreakToWorkCancelsPreview(void) {
    SetUp(1, 2, 2, 0, TRUE, TRUE, TRUE);
    CHECK(TimerEvents_HandlePomodoroCompletion(NULL));
    CHECK(strcmp(actions, "CNPR") == 0);
    CHECK(start_calls == 0 && cancel_calls == 1 && !preview_active);
    CHECK(notification_calls == 1 && sound_calls == 1);
    CHECK(current_pomodoro_phase == POMODORO_PHASE_WORK);
    CHECK(complete_pomodoro_cycles == 1);
    CheckNext(0, 1500);
}

static void TestLastWorkWrapsToWork(void) {
    SetUp(2, 3, 2, 0, TRUE, TRUE, TRUE);
    CHECK(TimerEvents_HandlePomodoroCompletion(NULL));
    CHECK(strcmp(actions, "CNPR") == 0);
    CHECK(start_calls == 0 && cancel_calls == 1 && !preview_active);
    CHECK(complete_pomodoro_cycles == 1);
    CHECK(current_pomodoro_phase == POMODORO_PHASE_WORK);
    CheckNext(0, 1500);
}

static void TestFinalCompletion(void) {
    for (int index = 0; index <= 1; ++index) {
        SetUp(index, index + 1, 1, 0, TRUE, TRUE, TRUE);
        CHECK(!TimerEvents_HandlePomodoroCompletion(NULL));
        CHECK(strcmp(actions, "CNRNPT") == 0);
        CHECK(start_calls == 0 && cancel_calls == 1 && !preview_active);
        CHECK(notification_calls == 2 && sound_calls == 1);
        CHECK(wcscmp(notifications[1], L"All Pomodoro cycles completed!") == 0);
        CHECK(reset_duration == 0 && timer_stops == 1);
        CHECK(current_pomodoro_phase == POMODORO_PHASE_IDLE);
        CHECK(pomodoro_initial_times_count == 0 && complete_pomodoro_cycles == 0);
        CHECK(!CLOCK_COUNT_UP && !CLOCK_SHOW_CURRENT_TIME && message_shown);
        CHECK(g_target_end_time == 100000 && timer_initializations == 0);
    }
}

int main(void) {
    TestDisabledCancelsPreview();
    TestWorkToBreakStartsFormal();
    TestPreviewHandoff();
    TestFailedStartFallsBack();
    TestBreakToWorkCancelsPreview();
    TestLastWorkWrapsToWork();
    TestFinalCompletion();
    if (!failures) puts("pomodoro black-hole handoff tests passed");
    return failures ? 1 : 0;
}