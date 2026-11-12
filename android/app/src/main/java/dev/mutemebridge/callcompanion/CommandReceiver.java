package dev.mutemebridge.callcompanion;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.util.Log;

/**
 * Receives commands from Home Assistant, sent with the companion app's
 * "command_broadcast_intent" notification.
 */
public class CommandReceiver extends BroadcastReceiver {
    private static final String TAG = "MuteMeCompanion";
    private static final String PREFIX = "dev.mutemebridge.callcompanion.";

    @Override
    public void onReceive(Context context, Intent intent) {
        String action = intent.getAction();
        if (action == null) {
            return;
        }
        boolean handled;
        switch (action) {
            case PREFIX + "TOGGLE_MUTE":
                handled = CallState.toggleMute();
                break;
            case PREFIX + "MUTE":
                handled = CallState.setMute(true);
                break;
            case PREFIX + "UNMUTE":
                handled = CallState.setMute(false);
                break;
            case PREFIX + "REPORT":
                handled = false; // fall through to the resend below
                break;
            default:
                return;
        }
        Log.i(TAG, action + (handled ? " applied" : " — no active call"));

        // No call (or a REPORT request): resend the current state so HA can resync.
        if (!handled) {
            CallState.publish(context, true);
        }
    }
}
