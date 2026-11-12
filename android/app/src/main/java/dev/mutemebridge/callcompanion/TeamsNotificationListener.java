package dev.mutemebridge.callcompanion;

import android.app.Notification;
import android.app.PendingIntent;
import android.service.notification.NotificationListenerService;
import android.service.notification.StatusBarNotification;
import android.util.Log;

/**
 * Reads Teams' ongoing-call notification. Its first button says "Mute" while you're unmuted and
 * "Unmute" while you're muted, so it tells us Teams' real mute state, and pressing it changes
 * Teams' own mute (the Teams screen updates too). Teams doesn't share its mute with Android's
 * call system, so this is the only reliable hook.
 */
public class TeamsNotificationListener extends NotificationListenerService {
    private static final String TAG = "MuteMeCompanion";
    static final String TEAMS_PACKAGE = "com.microsoft.teams";

    static volatile TeamsNotificationListener instance;

    /** The current Teams call notification's mute/unmute button, or null when there's no call. */
    private static volatile Notification.Action muteAction;
    private static volatile boolean muted;
    private static volatile String callKey;

    @Override
    public void onListenerConnected() {
        instance = this;
        rescan();
    }

    @Override
    public void onListenerDisconnected() {
        instance = null;
        clear();
        CallState.publish(this, false);
    }

    @Override
    public void onNotificationPosted(StatusBarNotification sbn) {
        if (TEAMS_PACKAGE.equals(sbn.getPackageName())) {
            rescan();
        }
    }

    @Override
    public void onNotificationRemoved(StatusBarNotification sbn) {
        if (TEAMS_PACKAGE.equals(sbn.getPackageName())) {
            rescan();
        }
    }

    /** Looks through Teams' active notifications for the ongoing-call one. */
    private void rescan() {
        StatusBarNotification[] active;
        try {
            active = getActiveNotifications();
        } catch (Exception e) {
            Log.w(TAG, "Couldn't read notifications", e);
            return;
        }
        Notification.Action found = null;
        String key = null;
        if (active != null) {
            for (StatusBarNotification sbn : active) {
                if (!TEAMS_PACKAGE.equals(sbn.getPackageName())) {
                    continue;
                }
                Notification n = sbn.getNotification();
                boolean ongoing = (n.flags & Notification.FLAG_ONGOING_EVENT) != 0;
                if (!ongoing || !Notification.CATEGORY_CALL.equals(n.category)) {
                    continue;
                }
                Notification.Action action = findMuteAction(n);
                if (action != null) {
                    found = action;
                    key = sbn.getKey();
                    break;
                }
            }
        }
        if (found == null) {
            clear();
        } else {
            muteAction = found;
            callKey = key;
            muted = isUnmuteAction(found);
        }
        CallState.publish(this, false);
    }

    private static Notification.Action findMuteAction(Notification n) {
        if (n.actions == null) {
            return null;
        }
        for (Notification.Action a : n.actions) {
            int semantic = a.getSemanticAction();
            if (semantic == Notification.Action.SEMANTIC_ACTION_MUTE
                    || semantic == Notification.Action.SEMANTIC_ACTION_UNMUTE) {
                return a;
            }
            String title = a.title == null ? "" : a.title.toString().trim();
            if (title.equalsIgnoreCase("Mute") || title.equalsIgnoreCase("Unmute")) {
                return a;
            }
        }
        return null;
    }

    /** True when the button would unmute, i.e. Teams is currently muted. */
    private static boolean isUnmuteAction(Notification.Action a) {
        if (a.getSemanticAction() == Notification.Action.SEMANTIC_ACTION_UNMUTE) {
            return true;
        }
        if (a.getSemanticAction() == Notification.Action.SEMANTIC_ACTION_MUTE) {
            return false;
        }
        return a.title != null && a.title.toString().trim().equalsIgnoreCase("Unmute");
    }

    private static void clear() {
        muteAction = null;
        callKey = null;
        muted = false;
    }

    // ---- Used by CallState / CommandReceiver / status screen ----

    static boolean hasTeamsCall() {
        return instance != null && muteAction != null;
    }

    static boolean isMuted() {
        return hasTeamsCall() && muted;
    }

    /** Presses Teams' mute/unmute button. Returns false if there's no Teams call. */
    static boolean toggle() {
        Notification.Action a = muteAction;
        if (instance == null || a == null || a.actionIntent == null) {
            return false;
        }
        try {
            a.actionIntent.send();
            return true;
        } catch (PendingIntent.CanceledException e) {
            Log.w(TAG, "Teams' mute button is no longer valid", e);
            return false;
        }
    }

    /** Mutes or unmutes Teams, pressing its button only if the state needs to change. */
    static boolean setMuted(boolean mute) {
        if (!hasTeamsCall()) {
            return false;
        }
        return muted == mute || toggle();
    }

    static String describe() {
        if (instance == null) {
            return "Notification access not connected";
        }
        if (muteAction == null) {
            return "No Teams call notification";
        }
        return "Teams call — " + (muted ? "muted" : "unmuted") + " (button: \"" + muteAction.title + "\")";
    }
}
