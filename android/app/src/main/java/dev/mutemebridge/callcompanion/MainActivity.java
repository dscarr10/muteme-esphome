package dev.mutemebridge.callcompanion;

import android.app.Activity;
import android.app.AppOpsManager;
import android.app.role.RoleManager;
import android.content.Intent;
import android.graphics.Typeface;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.Process;
import android.text.InputType;
import android.util.TypedValue;
import android.view.View;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

/** Status and setup screen. The real work happens in CallCompanionService. */
public class MainActivity extends Activity {
    private static final int REQ_ROLE = 1;
    // Not exposed in the public SDK (it's a @SystemApi constant), so spell it out.
    private static final String ROLE_CALL_COMPANION = "android.app.role.CALL_COMPANION";
    private static final String OP_MANAGE_ONGOING_CALLS = "android:manage_ongoing_calls";

    private final Handler handler = new Handler(Looper.getMainLooper());
    private final Runnable ticker = new Runnable() {
        @Override
        public void run() {
            refresh();
            handler.postDelayed(this, 1000);
        }
    };

    private TextView roleText;
    private TextView callText;
    private TextView lastSentText;
    private EditText webhookInput;
    private TextView adbText;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        int pad = dp(16);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(pad, pad, pad, pad);

        root.addView(heading("MuteMe Call Companion"));
        root.addView(body("Lets Home Assistant see and change the mute state of calls on this phone "
                + "(Teams and other calling apps that share their calls with Android)."));

        root.addView(heading("1. Call access"));
        roleText = body("");
        root.addView(roleText);
        Button roleButton = button("Grant call companion role", v -> requestRole());
        root.addView(roleButton);
        adbText = body("If the button does nothing (many phones don't offer it), grant call access once "
                + "from a PC with USB debugging on:\n"
                + "adb shell appops set " + getPackageName() + " MANAGE_ONGOING_CALLS allow");
        adbText.setTextIsSelectable(true);
        adbText.setTypeface(Typeface.MONOSPACE);
        root.addView(adbText);

        root.addView(heading("2. Home Assistant webhook"));
        webhookInput = new EditText(this);
        webhookInput.setHint("https://…/api/webhook/muteme_call_companion");
        webhookInput.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI);
        webhookInput.setSingleLine(true);
        webhookInput.setText(Prefs.webhookUrl(this));
        root.addView(webhookInput);
        root.addView(button("Save and send a test", v -> {
            Prefs.setWebhookUrl(this, webhookInput.getText().toString());
            CallCompanionService s = CallCompanionService.instance;
            if (s != null) {
                s.publish(true);
            } else {
                HaReporter.report(this, false, false, "", true);
            }
            Toast.makeText(this, "Saved, sending…", Toast.LENGTH_SHORT).show();
        }));
        lastSentText = body("");
        root.addView(lastSentText);

        root.addView(heading("3. Live status"));
        callText = body("");
        callText.setTextIsSelectable(true);
        root.addView(callText);
        root.addView(button("Toggle mute (test)", v -> {
            if (!CallCompanionService.toggleMute()) {
                Toast.makeText(this, "No active call visible", Toast.LENGTH_SHORT).show();
            }
        }));

        ScrollView scroll = new ScrollView(this);
        scroll.addView(root);
        setContentView(scroll);
    }

    @Override
    protected void onResume() {
        super.onResume();
        handler.post(ticker);
    }

    @Override
    protected void onPause() {
        handler.removeCallbacks(ticker);
        super.onPause();
    }

    private void refresh() {
        boolean held = hasCallAccess();
        roleText.setText(held
                ? "Call access: granted"
                : "Call access: NOT granted. The app can't see calls until it is.");
        adbText.setVisibility(held ? View.GONE : View.VISIBLE);

        String calls = CallCompanionService.describeCalls();
        boolean bound = CallCompanionService.instance != null;
        callText.setText("Calls seen:\n" + calls
                + (bound ? "\nMuted: " + (CallCompanionService.isMutedNow() ? "yes" : "no") : ""));

        lastSentText.setText("Last send: " + Prefs.lastResult(this));
    }

    /** Either route gives Android a reason to bind CallCompanionService during calls. */
    private boolean hasCallAccess() {
        return roleHeld() || manageOngoingCallsAllowed();
    }

    private boolean roleHeld() {
        RoleManager rm = getSystemService(RoleManager.class);
        return rm != null && rm.isRoleAvailable(ROLE_CALL_COMPANION) && rm.isRoleHeld(ROLE_CALL_COMPANION);
    }

    private boolean manageOngoingCallsAllowed() {
        AppOpsManager ops = getSystemService(AppOpsManager.class);
        if (ops == null) {
            return false;
        }
        try {
            return ops.unsafeCheckOpNoThrow(OP_MANAGE_ONGOING_CALLS, Process.myUid(), getPackageName())
                    == AppOpsManager.MODE_ALLOWED;
        } catch (Exception e) {
            return false;
        }
    }

    private void requestRole() {
        RoleManager rm = getSystemService(RoleManager.class);
        if (rm == null || !rm.isRoleAvailable(ROLE_CALL_COMPANION)) {
            Toast.makeText(this, "This phone doesn't offer the role here — use the adb command.",
                    Toast.LENGTH_LONG).show();
            return;
        }
        if (rm.isRoleHeld(ROLE_CALL_COMPANION)) {
            Toast.makeText(this, "Already granted", Toast.LENGTH_SHORT).show();
            return;
        }
        try {
            startActivityForResult(rm.createRequestRoleIntent(ROLE_CALL_COMPANION), REQ_ROLE);
        } catch (Exception e) {
            Toast.makeText(this, "Couldn't ask for the role — use the adb command.", Toast.LENGTH_LONG).show();
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == REQ_ROLE) {
            refresh();
            if (!hasCallAccess()) {
                Toast.makeText(this, "Not granted — use the adb command.", Toast.LENGTH_LONG).show();
            }
        }
    }

    // ---- tiny UI helpers ----

    private TextView heading(String text) {
        TextView t = new TextView(this);
        t.setText(text);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, 18);
        t.setTypeface(Typeface.DEFAULT_BOLD);
        t.setPadding(0, dp(16), 0, dp(4));
        return t;
    }

    private TextView body(String text) {
        TextView t = new TextView(this);
        t.setText(text);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15);
        t.setPadding(0, dp(4), 0, dp(4));
        return t;
    }

    private Button button(String text, View.OnClickListener listener) {
        Button b = new Button(this);
        b.setText(text);
        b.setAllCaps(false);
        b.setOnClickListener(listener);
        return b;
    }

    private int dp(int value) {
        return Math.round(TypedValue.applyDimension(
                TypedValue.COMPLEX_UNIT_DIP, value, getResources().getDisplayMetrics()));
    }
}
