package com.runbeat.pro;

import android.app.Dialog;
import android.content.Context;
import android.graphics.Color;
import android.graphics.drawable.ColorDrawable;
import android.os.Handler;
import android.os.Looper;
import android.view.Gravity;
import android.view.LayoutInflater;
import android.view.View;
import android.view.Window;
import android.view.WindowManager;
import android.widget.TextView;

import androidx.core.content.ContextCompat;

import com.runbeat.audio.CadenceNative;

import java.util.Locale;

/** 步频统计面板 — 底部抽屉式只读弹窗。 */
final class CadenceStatsDialog {

    private CadenceStatsDialog() {}

    static void show(Context context, int accentColor) {
        final Dialog dialog = new Dialog(context);
        dialog.requestWindowFeature(Window.FEATURE_NO_TITLE);
        final View root = LayoutInflater.from(context).inflate(R.layout.dialog_cadence_stats, null);
        dialog.setContentView(root);
        dialog.setCanceledOnTouchOutside(true);

        final CadenceChartView chart = root.findViewById(R.id.cadenceChart);
        final TextView tvAvg = root.findViewById(R.id.tvStatAvg);
        final TextView tvMax = root.findViewById(R.id.tvStatMax);
        final TextView tvAccuracy = root.findViewById(R.id.tvStatAccuracy);
        final TextView tvElapsed = root.findViewById(R.id.tvStatElapsed);
        final TextView tvSteps = root.findViewById(R.id.tvPanelSteps);
        final TextView tvDelta = root.findViewById(R.id.tvPanelDelta);
        final TextView tvChartTarget = root.findViewById(R.id.tvChartTarget);
        final TextView tvChartEmpty = root.findViewById(R.id.tvChartEmpty);
        final TextView btnClose = root.findViewById(R.id.btnCloseStats);

        for (TextView value : new TextView[]{tvAvg, tvMax, tvAccuracy, tvElapsed}) {
            value.setTextColor(accentColor);
        }
        btnClose.setOnClickListener(v -> dialog.dismiss());

        final Handler handler = new Handler(Looper.getMainLooper());
        final float[] history = new float[60];
        final Runnable refresh = new Runnable() {
            @Override
            public void run() {
                final double[] snapshot = CadenceNative.snapshot();
                final float target = (float) snapshot[CadenceNative.TARGET];
                final double delta = snapshot[CadenceNative.CADENCE] - target;
                final boolean tracking = snapshot[CadenceNative.TRACKING] != 0;

                tvAvg.setText(spmText(snapshot[CadenceNative.AVG]));
                tvMax.setText(spmText(snapshot[CadenceNative.MAX]));
                tvAccuracy.setText(percentText(snapshot[CadenceNative.ACCURACY], snapshot[CadenceNative.ACTIVE_SEC]));
                tvElapsed.setText(durationText(snapshot[CadenceNative.ACTIVE_SEC]));
                tvSteps.setText(context.getString(R.string.label_steps_fmt,
                        String.format(Locale.getDefault(), "%,d", (long) snapshot[CadenceNative.STEPS])));
                tvChartTarget.setText(context.getString(R.string.label_target_fmt, String.format(Locale.US, "%.1f", target)));

                if (tracking) {
                    tvDelta.setText(context.getString(R.string.label_current_delta_fmt, signed(delta)));
                    tvDelta.setTextColor(deltaColor(context, delta, accentColor));
                } else {
                    tvDelta.setText("—");
                    tvDelta.setTextColor(ContextCompat.getColor(context, R.color.textSecondary));
                }

                final int valid = CadenceNative.history(history);
                boolean hasData = false;
                for (float value : history) {
                    if (value > 0f) {
                        hasData = true;
                        break;
                    }
                }
                chart.setVisibility(hasData ? View.VISIBLE : View.INVISIBLE);
                tvChartEmpty.setVisibility(hasData ? View.GONE : View.VISIBLE);
                chart.setData(history, target, accentColor);

                handler.postDelayed(this, 1000);
            }
        };

        dialog.setOnShowListener(d -> {
            final Window window = dialog.getWindow();
            if (window != null) {
                window.setBackgroundDrawable(new ColorDrawable(Color.TRANSPARENT));
                window.setLayout(WindowManager.LayoutParams.MATCH_PARENT, WindowManager.LayoutParams.WRAP_CONTENT);
                window.setGravity(Gravity.BOTTOM);
                window.addFlags(WindowManager.LayoutParams.FLAG_DIM_BEHIND);
                window.setDimAmount(0.65f);
            }
            handler.post(refresh);
        });
        dialog.setOnDismissListener(d -> handler.removeCallbacks(refresh));
        dialog.show();
    }

    private static String spmText(double spm) {
        return spm >= 1.0 ? String.valueOf(Math.round(spm)) : "—";
    }

    private static String percentText(double accuracy, double activeSec) {
        return activeSec >= 0.5 ? String.format(Locale.US, "%.0f%%", accuracy * 100.0) : "—";
    }

    private static String durationText(double seconds) {
        final int total = (int) Math.max(0, seconds);
        return String.format(Locale.US, "%02d:%02d:%02d", total / 3600, (total % 3600) / 60, total % 60);
    }

    private static String signed(double value) {
        final long rounded = Math.round(value);
        return String.format(Locale.US, "%+d", rounded);
    }

    private static int deltaColor(Context context, double delta, int accentColor) {
        final double abs = Math.abs(delta);
        if (abs <= 3.0) return accentColor;
        if (abs <= 6.0) return ContextCompat.getColor(context, R.color.cadence_warn);
        return ContextCompat.getColor(context, R.color.cadence_bad);
    }
}
