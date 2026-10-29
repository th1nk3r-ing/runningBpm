package com.runbeat.pro;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.DashPathEffect;
import android.graphics.LinearGradient;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.graphics.Shader;
import android.util.AttributeSet;
import android.view.View;

import androidx.annotation.Nullable;

import java.util.Locale;

/** 近 60 秒步频趋势 — 0 表示该秒无数据，断开折线。 */
public final class CadenceChartView extends View {

    private static final int POINTS = 60;
    private static final float BASE_MIN = 150f;
    private static final float BASE_MAX = 210f;

    private final float[] values = new float[POINTS];
    private final Paint linePaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint fillPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint targetPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint textPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Path linePath = new Path();
    private final Path fillPath = new Path();
    private final RectF plot = new RectF();
    private final float density;

    private float targetSpm = 180f;
    private int accentColor = 0xFFFF6B35;
    private boolean hasData;

    public CadenceChartView(Context context, @Nullable AttributeSet attrs) {
        super(context, attrs);
        density = getResources().getDisplayMetrics().density;

        linePaint.setStyle(Paint.Style.STROKE);
        linePaint.setStrokeWidth(2f * density);
        linePaint.setStrokeCap(Paint.Cap.ROUND);
        linePaint.setStrokeJoin(Paint.Join.ROUND);

        targetPaint.setStyle(Paint.Style.STROKE);
        targetPaint.setStrokeWidth(1f * density);
        targetPaint.setColor(0xFF666666);
        targetPaint.setPathEffect(new DashPathEffect(new float[]{4f * density, 4f * density}, 0f));

        textPaint.setColor(0xFF8A8A8A);
        textPaint.setTextSize(10f * density);
    }

    public void setData(float[] history, float targetSpm, int accentColor) {
        if (history != null) {
            System.arraycopy(history, 0, values, 0, Math.min(history.length, POINTS));
        }
        this.targetSpm = targetSpm;
        this.accentColor = accentColor;
        hasData = false;
        for (float value : values) {
            if (value > 0f) {
                hasData = true;
                break;
            }
        }
        invalidate();
    }

    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        if (!hasData) return;

        float min = BASE_MIN;
        float max = BASE_MAX;
        for (float value : values) {
            if (value > 0f) {
                min = Math.min(min, value);
                max = Math.max(max, value);
            }
        }
        min = Math.min(min, targetSpm);
        max = Math.max(max, targetSpm);
        min -= 5f;
        max += 5f;

        final float left = 30f * density;
        final float top = 6f * density;
        final float right = getWidth() - 6f * density;
        final float bottom = getHeight() - 6f * density;
        plot.set(left, top, right, bottom);

        final float targetY = yFor(targetSpm, min, max, bottom);
        canvas.drawLine(left, targetY, right, targetY, targetPaint);

        linePaint.setColor(accentColor);
        fillPaint.setShader(new LinearGradient(0f, top, 0f, bottom,
                adjustAlpha(accentColor, 0.18f), Color.TRANSPARENT, Shader.TileMode.CLAMP));

        int index = 0;
        final float stepX = (right - left) / (POINTS - 1);
        while (index < POINTS) {
            if (values[index] <= 0f) {
                ++index;
                continue;
            }
            linePath.reset();
            fillPath.reset();
            final int segmentStart = index;
            boolean first = true;
            while (index < POINTS && values[index] > 0f) {
                final float x = left + stepX * index;
                final float y = yFor(values[index], min, max, bottom);
                if (first) {
                    linePath.moveTo(x, y);
                    fillPath.moveTo(x, bottom);
                    fillPath.lineTo(x, y);
                    first = false;
                } else {
                    linePath.lineTo(x, y);
                    fillPath.lineTo(x, y);
                }
                ++index;
            }
            fillPath.lineTo(left + stepX * (index - 1), bottom);
            fillPath.close();
            canvas.drawPath(fillPath, fillPaint);
            canvas.drawPath(linePath, linePaint);
            if (index == segmentStart) ++index;
        }

        textPaint.setTextAlign(Paint.Align.RIGHT);
        canvas.drawText(format(max), left - 4f * density, top + 9f * density, textPaint);
        canvas.drawText(format((min + max) / 2f), left - 4f * density, (top + bottom) / 2f + 3f * density, textPaint);
        canvas.drawText(format(min), left - 4f * density, bottom + 2f * density, textPaint);
    }

    private float yFor(float value, float min, float max, float bottom) {
        final float ratio = (value - min) / Math.max(1f, max - min);
        return bottom - ratio * (bottom - plot.top);
    }

    private static String format(float value) {
        return String.format(Locale.US, "%.0f", value);
    }

    private static int adjustAlpha(int color, float factor) {
        final int alpha = Math.round(Color.alpha(color) * factor);
        return Color.argb(alpha, Color.red(color), Color.green(color), Color.blue(color));
    }
}
