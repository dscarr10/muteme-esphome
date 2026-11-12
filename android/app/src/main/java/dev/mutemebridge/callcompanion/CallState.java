package dev.mutemebridge.callcompanion;

import android.content.Context;
import android.telecom.Call;

/**
 * Combines the two call sources into one state for Home Assistant.
 * Teams' notification wins when present (it has Teams' real mute state); otherwise Android's
 * call system is used (other calling apps, phone calls).
 */
final class CallState {
    private CallState() {}

    static void publish(Context ctx, boolean force) {
        if (TeamsNotificationListener.hasTeamsCall()) {
            HaReporter.report(ctx, true, TeamsNotificationListener.isMuted(),
                    TeamsNotificationListener.TEAMS_PACKAGE, force);
            return;
        }
        CallCompanionService s = CallCompanionService.instance;
        Call call = s == null ? null : s.activeCall();
        if (call != null) {
            HaReporter.report(ctx, true, CallCompanionService.isMutedNow(),
                    CallCompanionService.appOf(call), force);
        } else {
            HaReporter.report(ctx, false, false, "", force);
        }
    }

    static boolean toggleMute() {
        if (TeamsNotificationListener.hasTeamsCall()) {
            return TeamsNotificationListener.toggle();
        }
        return CallCompanionService.toggleMute();
    }

    static boolean setMute(boolean mute) {
        if (TeamsNotificationListener.hasTeamsCall()) {
            return TeamsNotificationListener.setMuted(mute);
        }
        return CallCompanionService.setMute(mute);
    }
}
