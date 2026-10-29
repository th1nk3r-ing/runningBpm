package com.runbeat.pro;

import android.animation.ValueAnimator;
import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.RectF;
import android.util.AttributeSet;
import android.view.View;
import android.view.animation.DecelerateInterpolator;

import androidx.annotation.Nullable;

/** 步频环形仪表 — 只绘制弧与目标刻度，文字由叠放的 TextView 承载。 */
public final class CadenceRingView extends View {

    public enum Mode { IDLE, WAITING, TRACKING, PAUSED }

    private static final float START_ANGLE = 135f;
    private static final float SWEEP_ANGLE = 270f;
    private static final float MIN_SPM = 150f;
    private static final float MAX_SPM = 210f;
    private static final int TRACK_COLOR = 0xFF2A2A2A;
    private static final int INACTIVE_COLOR = 0xFF4A4A4A;

    private final Paint trackPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint progressPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint tickPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final RectF oval = new RectF();
    private final float density;

    private Mode mode = Mode.IDLE;
    private float cadenceSpm;
    private float targetSpm = 180f;
    private float displayedTarget = 180f;
    private boolean targetInitialized;
    private ValueAnimator targetAnimator;
    private int accentColor = 0xFFFF6B35;
    private int targetColor = 0xFF8E8E93;

    public CadenceRingView(Context context, @Nullable AttributeSet attrs) {
        super(context, attrs);
        density = getResources().getDisplayMetrics().density;
        float stroke = 10f * density;

        trackPaint.setStyle(Paint.Style.STROKE);
        trackPaint.setStrokeWidth(stroke);
        trackPaint.setStrokeCap(Paint.Cap.ROUND);
        trackPaint.setColor(TRACK_COLOR);

        progressPaint.setStyle(Paint.Style.STROKE);
        progressPaint.setStrokeWidth(stroke);
        progressPaint.setStrokeCap(Paint.Cap.ROUND);

        tickPaint.setStyle(Paint.Style.STROKE);
        tickPaint.setStrokeWidth(3f * density);
        tickPaint.setStrokeCap(Paint.Cap.ROUND);
    }

    public void setAccent(int color) {
        accentColor = color;
        invalidate();
    }

    /** 目标刻度色，与环内目标数字保持一致。 */
    public void setTargetColor(int color) {
        targetColor = color;
        invalidate();
    }

    public void setData(Mode mode, float cadenceSpm, float targetSpm) {
        this.mode = mode;
        this.cadenceSpm = cadenceSpm;
        if (!targetInitialized) {
            this.targetSpm = targetSpm;
            displayedTarget = targetSpm;
            targetInitialized = true;
        } else if (Math.abs(targetSpm - this.targetSpm) > 0.05f) {
            animateTargetTo(targetSpm);
            this.targetSpm = targetSpm;
        }
        invalidate();
    }

    /** 目标刻度滑动到新角度，避免调 BPM 时刻度瞬移。 */
    private void animateTargetTo(float target) {
        if (targetAnimator != null) targetAnimator.cancel();
        targetAnimator = ValueAnimator.ofFloat(displayedTarget, target);
        targetAnimator.setDuration(200);
        targetAnimator.setInterpolator(new DecelerateInterpolator());
        targetAnimator.addUpdateListener(animator -> {
            displayedTarget = (float) animator.getAnimatedValue();
            invalidate();
        });
        targetAnimator.start();
    }

    @Override
    protected void onDetachedFromWindow() {
        if (targetAnimator != null) {
            targetAnimator.cancel();
            targetAnimator = null;
        }
        super.onDetachedFromWindow();
    }

    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);

        final float stroke = 10f * density;
        final float size = Math.min(getWidth() - getPaddingLeft() - getPaddingRight(),
                getHeight() - getPaddingTop() - getPaddingBottom());
        if (size <= stroke) return;

        final float radius = (size - stroke) / 2f;
        final float centerX = getWidth() / 2f;
        final float centerY = getHeight() / 2f;
        oval.set(centerX - radius, centerY - radius, centerX + radius, centerY + radius);

        canvas.drawArc(oval, START_ANGLE, SWEEP_ANGLE, false, trackPaint);

        if (mode == Mode.TRACKING || mode == Mode.PAUSED) {
            final float progress = clamp01((cadenceSpm - MIN_SPM) / (MAX_SPM - MIN_SPM));
            if (progress > 0.001f) {
                progressPaint.setColor(mode == Mode.PAUSED ? INACTIVE_COLOR : accentColor);
                canvas.drawArc(oval, START_ANGLE, SWEEP_ANGLE * progress, false, progressPaint);
            }
        }

        final float targetProgress = clamp01((displayedTarget - MIN_SPM) / (MAX_SPM - MIN_SPM));
        final double angle = Math.toRadians(START_ANGLE + SWEEP_ANGLE * targetProgress);
        final float cos = (float) Math.cos(angle);
        final float sin = (float) Math.sin(angle);
        tickPaint.setColor(targetColor);
        tickPaint.setAlpha(mode == Mode.IDLE || mode == Mode.WAITING ? 170 : 235);
        canvas.drawLine(centerX + cos * (radius - 16f * density), centerY + sin * (radius - 16f * density),
                centerX + cos * (radius - 5f * density), centerY + sin * (radius - 5f * density), tickPaint);
    }

    private static float clamp01(float value) {
        if (value < 0f) return 0f;
        if (value > 1f) return 1f;
        return value;
    }
}
