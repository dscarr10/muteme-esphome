package dev.mutemebridge.callcompanion;

import android.content.Context;
import android.content.SharedPreferences;

/** Small wrapper around the app's saved settings. */
final class Prefs {
    private static final String FILE = "settings";
    private static final String KEY_WEBHOOK = "webhook_url";
    private static final String KEY_LAST_RESULT = "last_result";

    private Prefs() {}

    private static SharedPreferences sp(Context ctx) {
        return ctx.getApplicationContext().getSharedPreferences(FILE, Context.MODE_PRIVATE);
    }

    static String webhookUrl(Context ctx) {
        return sp(ctx).getString(KEY_WEBHOOK, "").trim();
    }

    static void setWebhookUrl(Context ctx, String url) {
        sp(ctx).edit().putString(KEY_WEBHOOK, url == null ? "" : url.trim()).apply();
    }

    /** Human-readable result of the last webhook send, shown on the status screen. */
    static String lastResult(Context ctx) {
        return sp(ctx).getString(KEY_LAST_RESULT, "nothing sent yet");
    }

    static void setLastResult(Context ctx, String result) {
        sp(ctx).edit().putString(KEY_LAST_RESULT, result).apply();
    }
}
