package com.runbeat.pro;

import android.content.Context;

import com.runbeat.audio.AudioEngine;

/** Shared native-engine restoration and sound-pack configuration for Activity and Service. */
final class AudioEngineRestorer {

    static final String[][] SOUND_PACKS = {
        {"sounds/default/tick_hi.wav", "sounds/default/tick_lo.wav"},
        {"sounds/audios/BassDrum1.wav", "sounds/audios/BassDrum2.wav"},
        {"sounds/audios/Clap1.wav", "sounds/audios/Clap2.wav"},
        {"sounds/audios/Claves1.wav", "sounds/audios/Claves2.wav"},
        {"sounds/audios/Rimshot1.wav", "sounds/audios/Rimshot2.wav"},
        {"sounds/audios/downbeat.wav", "sounds/audios/upbeat.wav"},
    };

    private AudioEngineRestorer() {}

    static boolean loadDefaultSamples(Context context) {
        return AudioEngine.nativeLoadWavAssets(
                context.getAssets(),
                "sounds/default/tick_hi.wav",
                "sounds/default/tick_lo.wav",
                "chime.wav");
    }

    static boolean loadSoundPack(Context context, int timbreIndex) {
        if (timbreIndex < 0 || timbreIndex >= SOUND_PACKS.length) return false;
        return AudioEngine.nativeLoadSoundPack(
                context.getAssets(),
                SOUND_PACKS[timbreIndex][0],
                SOUND_PACKS[timbreIndex][1]);
    }

    /** Bring the native engine to the requested running/paused state. */
    static boolean restorePlaybackState(double bpm, boolean paused) {
        int state = AudioEngine.nativeGetState();
        if (state == 0) {
            if (!AudioEngine.nativeStart(bpm)) return false;
            if (paused) AudioEngine.nativePause();
        } else if (paused && state == 1) {
            AudioEngine.nativePause();
        } else if (!paused && state == 2) {
            return AudioEngine.nativeResume();
        }
        return true;
    }
}
