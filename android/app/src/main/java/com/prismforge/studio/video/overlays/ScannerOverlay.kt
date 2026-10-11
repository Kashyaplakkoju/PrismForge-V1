package com.prismforge.studio.video.overlays

import android.content.Context
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.Rect
import android.util.AttributeSet
import android.view.View
import kotlin.math.min

class ScannerOverlay(context: Context, attrs: AttributeSet) : View(context, attrs) {
    val rect = Rect()
    private val paint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = 0xFFA7B3FF.toInt()
        strokeWidth = 3f * resources.displayMetrics.density
        strokeCap = Paint.Cap.ROUND
        style = Paint.Style.STROKE
    }
    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)
        val side = min(width * 0.72f, height * 0.65f)
        val left = (width - side) / 2; val top = (height - side) / 2
        val right = left + side; val bottom = top + side; val arm = side * 0.13f
        rect.set(left.toInt(), top.toInt(), right.toInt(), bottom.toInt())
        for ((x, dx) in listOf(left to arm, right to -arm)) {
            for ((y, dy) in listOf(top to arm, bottom to -arm)) {
                canvas.drawLine(x, y, x + dx, y, paint)
                canvas.drawLine(x, y, x, y + dy, paint)
            }
        }
    }
}
