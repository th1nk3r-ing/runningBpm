package com.runbeat.pro;

import android.Manifest;
import android.content.Context;
import android.content.pm.PackageManager;
import android.os.Build;
import android.telephony.PhoneStateListener;
import android.telephony.TelephonyCallback;
import android.telephony.TelephonyManager;

import androidx.core.content.ContextCompat;

/**
 * 通话状态监听 —— 无音频焦点后，来电时自动暂停节拍器。
 *
 * API 31+ 使用 {@link TelephonyCallback}（需要运行时权限 READ_PHONE_STATE）；
 * 低版本回退到已废弃的 {@link PhoneStateListener}，不做硬性权限拦截、仅吞掉
 * {@link SecurityException} 静默降级：不监听、不崩溃。
 */
final class CallStateMonitor {

    /** 通话状态回调（均在主线程触发）。 */
    interface Callback {
        void onCallStarted();
        void onCallEnded();
    }

    private final Context context;
    private final Callback callback;
    private TelephonyManager telephonyManager;
    private TelephonyCallback telephonyCallback;   // API 31+
    private PhoneStateListener phoneStateListener; // API < 31
    private boolean registered;

    CallStateMonitor(Context context, Callback callback) {
        this.context = context.getApplicationContext();
        this.callback = callback;
    }

    /** 注册监听；无权限或设备异常时静默返回，不抛异常。 */
    @SuppressWarnings("deprecation")
    void start() {
        if (registered) return;
        TelephonyManager tm = (TelephonyManager) context.getSystemService(Context.TELEPHONY_SERVICE);
        if (tm == null) return;
        // API 31+ 的 TelephonyCallback 必须持有 READ_PHONE_STATE，未授予则静默跳过；
        // API < 31 的 PhoneStateListener 不在此处硬性拦截，交由下方 catch(SecurityException) 兜底。
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S
                && ContextCompat.checkSelfPermission(context, Manifest.permission.READ_PHONE_STATE)
                != PackageManager.PERMISSION_GRANTED) {
            return;
        }
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                telephonyCallback = new CallStateCallback();
                tm.registerTelephonyCallback(context.getMainExecutor(), telephonyCallback);
            } else {
                phoneStateListener = new PhoneStateListener() {
                    @Override
                    public void onCallStateChanged(int state, String phoneNumber) {
                        handleCallState(state);
                    }
                };
                tm.listen(phoneStateListener, PhoneStateListener.LISTEN_CALL_STATE);
            }
            telephonyManager = tm;
            registered = true;
        } catch (SecurityException e) {
            // 权限被 ROM 限制：静默降级
            telephonyCallback = null;
            phoneStateListener = null;
        }
    }

    /** 注销监听；未注册时为空操作。 */
    @SuppressWarnings("deprecation")
    void stop() {
        if (!registered) return;
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S && telephonyCallback != null) {
                telephonyManager.unregisterTelephonyCallback(telephonyCallback);
            } else if (phoneStateListener != null) {
                telephonyManager.listen(phoneStateListener, PhoneStateListener.LISTEN_NONE);
            }
        } catch (Exception e) {
            // 注销失败不应影响生命周期收尾
        }
        telephonyCallback = null;
        phoneStateListener = null;
        registered = false;
    }

    private void handleCallState(int state) {
        if (state == TelephonyManager.CALL_STATE_IDLE) {
            callback.onCallEnded();
        } else {
            // RINGING / OFFHOOK 均视为通话开始
            callback.onCallStarted();
        }
    }

    /**
     * API 31+ 回调实现。
     * {@link TelephonyCallback} 本身不实现监听接口，必须显式实现 {@link TelephonyCallback.CallStateListener}
     * 才能收到 onCallStateChanged。
     */
    private final class CallStateCallback extends TelephonyCallback
            implements TelephonyCallback.CallStateListener {
        @Override
        public void onCallStateChanged(int state) {
            handleCallState(state);
        }
    }
}
