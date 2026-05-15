package com.runbeat.pro;

import android.content.Context;
import android.content.SharedPreferences;

public class PreferencesManager {

    private static final String PREF_NAME = "runbeat_prefs";

    // KEY_BPM_LEGACY: 旧版以 float 存储的键（只读，用于迁移）
    private static final String KEY_BPM_LEGACY  = "bpm";
    // KEY_BPM_BITS: 新版以 long 存储 double 位模式，零精度损失
    private static final String KEY_BPM_BITS    = "bpm_bits";
    private static final String KEY_TICK_VOLUME = "tick_volume";
    private static final String KEY_ACCENT      = "accent";
    private static final String KEY_TIMBRE      = "timbre";
    private static final String KEY_GAIN_LEVEL  = "gain_level";
    private static final String KEY_ACCENT_INDEX = "accent_index";
    private static final String KEY_LANGUAGE     = "language";

    private static final double  DEFAULT_BPM         = 180.0;
    private static final int     DEFAULT_TICK_VOLUME  = 80;
    private static final boolean DEFAULT_ACCENT       = true;
    private static final int     DEFAULT_TIMBRE       = 0;
    private static final int     DEFAULT_GAIN_LEVEL   = 1;
    private static final int     DEFAULT_ACCENT_INDEX = 0;

    private final SharedPreferences prefs;

    public PreferencesManager(Context context) {
        prefs = context.getSharedPreferences(PREF_NAME, Context.MODE_PRIVATE);
    }

    /**
     * 读取 BPM。优先读取新版 long bits 键；若不存在则从旧版 float 键迁移（一次性）。
     */
    public double getBpm() {
        if (prefs.contains(KEY_BPM_BITS)) {
            return Double.longBitsToDouble(
                    prefs.getLong(KEY_BPM_BITS, Double.doubleToRawLongBits(DEFAULT_BPM)));
        }
        // 旧版迁移：读取旧 float 值并立即写入新格式
        double legacy = prefs.getFloat(KEY_BPM_LEGACY, (float) DEFAULT_BPM);
        setBpm(legacy); // 写入新键，下次直接读新键
        return legacy;
    }

    /**
     * 存储 BPM（以 Double.doubleToRawLongBits 存为 long，保留完整双精度精度）。
     */
    public void setBpm(double bpm) {
        prefs.edit()
                .putLong(KEY_BPM_BITS, Double.doubleToRawLongBits(bpm))
                .apply();
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

    public int getGainLevel() {
        int v = prefs.getInt(KEY_GAIN_LEVEL, DEFAULT_GAIN_LEVEL);
        if (v < 1) return 1;
        if (v > 3) return 3;
        return v;
    }

    public void setGainLevel(int level) {
        prefs.edit().putInt(KEY_GAIN_LEVEL, level).apply();
    }

    public int getAccentIndex() {
        int v = prefs.getInt(KEY_ACCENT_INDEX, DEFAULT_ACCENT_INDEX);
        return (v >= 0 && v < 5) ? v : DEFAULT_ACCENT_INDEX;
    }

    public void setAccentIndex(int index) {
        prefs.edit().putInt(KEY_ACCENT_INDEX, index).apply();
    }

    public String getLanguage() {
        return prefs.getString(KEY_LANGUAGE, "zh");
    }

    public void setLanguage(String lang) {
        prefs.edit().putString(KEY_LANGUAGE, lang).commit();
    }
}
