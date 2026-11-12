package dev.mutemebridge.callcompanion;

import android.content.Context;
import android.util.Log;

import org.json.JSONException;
import org.json.JSONObject;

import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.text.DateFormat;
import java.util.Date;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/** Sends the call / mute state to a Home Assistant webhook. */
final class HaReporter {
    private static final String TAG = "MuteMeCompanion";
    private static final ExecutorService EXEC = Executors.newSingleThreadExecutor();
    private static String lastBody = null;

    private HaReporter() {}

    /**
     * Report the current state. Unchanged states are skipped unless {@code force} is true.
     */
    static synchronized void report(Context ctx, boolean inCall, boolean muted, String app, boolean force) {
        final Context appCtx = ctx.getApplicationContext();
        final String url = Prefs.webhookUrl(appCtx);
        if (url.isEmpty()) {
            Prefs.setLastResult(appCtx, "no webhook URL set");
            return;
        }

        final String body;
        try {
            JSONObject json = new JSONObject();
            json.put("in_call", inCall);
            json.put("muted", inCall && muted);
            json.put("app", app == null ? "" : app);
            body = json.toString();
        } catch (JSONException e) {
            Log.e(TAG, "Couldn't build JSON", e);
            return;
        }

        if (!force && body.equals(lastBody)) {
            return;
        }
        lastBody = body;

        EXEC.execute(() -> {
            String result = post(url, body);
            String time = DateFormat.getTimeInstance(DateFormat.MEDIUM).format(new Date());
            Prefs.setLastResult(appCtx, time + ": " + result + "  " + body);
        });
    }

    private static String post(String url, String body) {
        HttpURLConnection conn = null;
        try {
            conn = (HttpURLConnection) new URL(url).openConnection();
            conn.setRequestMethod("POST");
            conn.setConnectTimeout(5000);
            conn.setReadTimeout(5000);
            conn.setDoOutput(true);
            conn.setRequestProperty("Content-Type", "application/json");
            byte[] bytes = body.getBytes(StandardCharsets.UTF_8);
            conn.setFixedLengthStreamingMode(bytes.length);
            try (OutputStream out = conn.getOutputStream()) {
                out.write(bytes);
            }
            int code = conn.getResponseCode();
            return code >= 200 && code < 300 ? "sent (HTTP " + code + ")" : "HA answered HTTP " + code;
        } catch (Exception e) {
            Log.w(TAG, "Webhook failed", e);
            return "failed: " + e.getClass().getSimpleName() + (e.getMessage() == null ? "" : " " + e.getMessage());
        } finally {
            if (conn != null) {
                conn.disconnect();
            }
        }
    }
}
