package com.prismforge.studio

import android.view.View
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat

/** Keep controls clear of status/navigation bars, camera cutouts. */
fun View.applyStudioInsets() {
    val left = paddingLeft; val top = paddingTop; val right = paddingRight; val bottom = paddingBottom
    ViewCompat.setOnApplyWindowInsetsListener(this) { view, insets ->
        val safe = insets.getInsets(WindowInsetsCompat.Type.systemBars() or WindowInsetsCompat.Type.displayCutout())
        view.setPadding(left + safe.left, top + safe.top, right + safe.right, bottom + safe.bottom)
        insets
    }
    ViewCompat.requestApplyInsets(this)
}
