/**
 * @file config_notification_apply.c
 * @brief Apply notification settings from a configuration snapshot.
 */

#include "config/config_applier.h"

#include "config.h"
#include "config/config_core_api.h"

#include <string.h>

void ApplyNotificationSettings(const ConfigSnapshot* snapshot) {
    if (!snapshot) return;
    strncpy(g_AppConfig.notification.messages.timeout_message, snapshot->timeoutMessage,
           sizeof(g_AppConfig.notification.messages.timeout_message) - 1);
    g_AppConfig.notification.messages.timeout_message[
        sizeof(g_AppConfig.notification.messages.timeout_message) - 1] = '\0';
    g_AppConfig.notification.display.timeout_ms = snapshot->notificationTimeoutMs;
    g_AppConfig.notification.display.max_opacity = snapshot->notificationMaxOpacity;
    g_AppConfig.notification.display.corner_radius = snapshot->notificationCornerRadius;
    g_AppConfig.notification.display.font_size = snapshot->notificationFontSize;
    g_AppConfig.notification.display.type = snapshot->notificationType;
    g_AppConfig.notification.display.disabled = snapshot->notificationDisabled;
    g_AppConfig.notification.display.black_hole_enabled =
        snapshot->notificationBlackHoleEnabled;
    g_AppConfig.notification.display.window_x = snapshot->notificationWindowX;
    g_AppConfig.notification.display.window_y = snapshot->notificationWindowY;
    g_AppConfig.notification.display.window_width = snapshot->notificationWindowWidth;
    g_AppConfig.notification.display.window_height = snapshot->notificationWindowHeight;

    char resolvedSoundPath[MAX_PATH] = {0};
    const char* soundPathToApply = snapshot->notificationSoundFile;
    if (ExpandEffectiveLocalAppDataPath(snapshot->notificationSoundFile,
                                        resolvedSoundPath,
                                        sizeof(resolvedSoundPath))) {
        soundPathToApply = resolvedSoundPath;
    }
    strncpy(g_AppConfig.notification.sound.sound_file, soundPathToApply, MAX_PATH - 1);
    g_AppConfig.notification.sound.sound_file[MAX_PATH - 1] = '\0';
    g_AppConfig.notification.sound.volume = snapshot->notificationSoundVolume;
}