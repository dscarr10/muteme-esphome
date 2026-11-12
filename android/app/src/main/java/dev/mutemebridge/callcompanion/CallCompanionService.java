package dev.mutemebridge.callcompanion;

import android.telecom.Call;
import android.telecom.CallAudioState;
import android.telecom.InCallService;
import android.telecom.PhoneAccountHandle;
import android.util.Log;

import java.util.List;

/**
 * Non-UI in-call service. Android binds it while calls are active (once the app holds the
 * call companion role). It mirrors the shared call mute state to Home Assistant and lets
 * Home Assistant change it, the same mechanism Android Auto uses for its mute button.
 */
public class CallCompanionService extends InCallService {
    private static final String TAG = "MuteMeCompanion";

    static volatile CallCompanionService instance;

    private volatile boolean muted = false;

    private final Call.Callback callCallback = new Call.Callback() {
        @Override
        public void onStateChanged(Call call, int state) {
            publish(false);
        }
    };

    @Override
    public void onCreate() {
        super.onCreate();
        instance = this;
        Log.i(TAG, "In-call service started");
    }

    @Override
    public void onDestroy() {
        instance = null;
        // The service is unbound when the last call ends; make sure HA sees "not in a call".
        HaReporter.report(this, false, false, "", false);
        Log.i(TAG, "In-call service stopped");
        super.onDestroy();
    }

    @Override
    public void onCallAdded(Call call) {
        call.registerCallback(callCallback);
        refreshMuteFromAudioState();
        publish(false);
    }

    @Override
    public void onCallRemoved(Call call) {
        call.unregisterCallback(callCallback);
        publish(false);
    }

    @SuppressWarnings("deprecation")
    @Override
    public void onCallAudioStateChanged(CallAudioState audioState) {
        if (audioState != null) {
            muted = audioState.isMuted();
            publish(false);
        }
    }

    /** Android 14+ reports mute changes here as well. */
    @Override
    public void onMuteStateChanged(boolean isMuted) {
        muted = isMuted;
        publish(false);
    }

    @SuppressWarnings("deprecation")
    private void refreshMuteFromAudioState() {
        CallAudioState s = getCallAudioState();
        if (s != null) {
            muted = s.isMuted();
        }
    }

    /** First call that is still active (not ending or ended). */
    Call activeCall() {
        List<Call> calls = getCalls();
        for (Call c : calls) {
            int state = c.getDetails().getState();
            if (state != Call.STATE_DISCONNECTED && state != Call.STATE_DISCONNECTING) {
                return c;
            }
        }
        return null;
    }

    static String appOf(Call call) {
        if (call == null) {
            return "";
        }
        PhoneAccountHandle handle = call.getDetails().getAccountHandle();
        if (handle == null || handle.getComponentName() == null) {
            return "";
        }
        return handle.getComponentName().getPackageName();
    }

    void publish(boolean force) {
        Call call = activeCall();
        HaReporter.report(this, call != null, muted, appOf(call), force);
    }

    // ---- Used by CommandReceiver and the status screen ----

    /** Toggle mute on the active call. Returns false if there is no call. */
    static boolean toggleMute() {
        CallCompanionService s = instance;
        if (s == null || s.activeCall() == null) {
            return false;
        }
        s.setMuted(!s.muted);
        return true;
    }

    /** Mute or unmute the active call. Returns false if there is no call. */
    static boolean setMute(boolean mute) {
        CallCompanionService s = instance;
        if (s == null || s.activeCall() == null) {
            return false;
        }
        s.setMuted(mute);
        return true;
    }

    static boolean isMutedNow() {
        CallCompanionService s = instance;
        return s != null && s.muted;
    }

    /** One line per call, for the status screen. */
    static String describeCalls() {
        CallCompanionService s = instance;
        if (s == null) {
            return "Not bound (no call right now, or the role isn't granted)";
        }
        List<Call> calls = s.getCalls();
        if (calls.isEmpty()) {
            return "Bound, but no calls visible";
        }
        StringBuilder sb = new StringBuilder();
        for (Call c : calls) {
            if (sb.length() > 0) {
                sb.append('\n');
            }
            String app = appOf(c);
            sb.append(app.isEmpty() ? "(unknown app)" : app)
                    .append(" — ")
                    .append(stateName(c.getDetails().getState()));
        }
        return sb.toString();
    }

    private static String stateName(int state) {
        switch (state) {
            case Call.STATE_NEW: return "new";
            case Call.STATE_DIALING: return "dialing";
            case Call.STATE_RINGING: return "ringing";
            case Call.STATE_HOLDING: return "on hold";
            case Call.STATE_ACTIVE: return "active";
            case Call.STATE_DISCONNECTED: return "ended";
            case Call.STATE_CONNECTING: return "connecting";
            case Call.STATE_DISCONNECTING: return "ending";
            case Call.STATE_SELECT_PHONE_ACCOUNT: return "choosing account";
            case Call.STATE_PULLING_CALL: return "pulling";
            case Call.STATE_AUDIO_PROCESSING: return "audio processing";
            case Call.STATE_SIMULATED_RINGING: return "simulated ringing";
            default: return "state " + state;
        }
    }
}
