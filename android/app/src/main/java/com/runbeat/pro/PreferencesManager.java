package com.runbeat.pro;

import android.content.Context;
import android.content.SharedPreferences;

public class PreferencesManager {

    private static final String PREF_NAME = "runbeat_prefs";
    private static final String KEY_BPM = "bpm";
    private static final String KEY_TICK_VOLUME = "tick_volume";
    private static final String KEY_ACCENT = "accent";
    private static final String KEY_TIMBRE = "timbre";

    private static final float DEFAULT_BPM = 180.0f;
    private static final int DEFAULT_TICK_VOLUME = 80;
    private static final boolean DEFAULT_ACCENT = true;
    private static final int DEFAULT_TIMBRE = 0;

    private final SharedPreferences prefs;

    public PreferencesManager(Context context) {
        prefs = context.getSharedPreferences(PREF_NAME, Context.MODE_PRIVATE);
    }

    public double getBpm() {
        return prefs.getFloat(KEY_BPM, DEFAULT_BPM);
    }

    public void setBpm(double bpm) {
        prefs.edit().putFloat(KEY_BPM, (float) bpm).apply();
    }

    public int getTickVolume() {
        return prefs.getInt(KEY_TICK_VOLUME, DEFAULT_TICK_VOLUME);
    }

    public void setTickVolume(int volume) {
        prefs.edit().putInt(KEY_TICK_VOLUME, volume).apply();
    }

    public boolean isAccentEnabled() {
        return prefs.getBoolean(KEY_ACCENT, DEFAULT_ACCENT);
    }

    public void setAccentEnabled(boolean enabled) {
        prefs.edit().putBoolean(KEY_ACCENT, enabled).apply();
    }

    public int getTimbre() {
        return prefs.getInt(KEY_TIMBRE, DEFAULT_TIMBRE);
    }

    public void setTimbre(int index) {
        prefs.edit().putInt(KEY_TIMBRE, index).apply();
    }
}
