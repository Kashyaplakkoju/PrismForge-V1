package com.prismforge.studio

import android.app.Activity
import android.content.Context
import android.util.TypedValue
import androidx.appcompat.app.AppCompatDelegate
import androidx.core.content.ContextCompat
import com.google.android.material.dialog.MaterialAlertDialogBuilder

object StudioAppearance {
    private const val PREFS = "studio_appearance"
    private val modes = intArrayOf(AppCompatDelegate.MODE_NIGHT_FOLLOW_SYSTEM, AppCompatDelegate.MODE_NIGHT_YES, AppCompatDelegate.MODE_NIGHT_NO, AppCompatDelegate.MODE_NIGHT_YES)
    private fun selected(context: Context) = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getInt("mode", 0).coerceIn(0, 3)
    fun initialize(context: Context) { AppCompatDelegate.setDefaultNightMode(modes[selected(context)]) }
    fun applyTheme(context: Activity) { context.setTheme(if (selected(context) == 3) R.style.Theme_PrismForgeStudio_Glass else R.style.Theme_PrismForgeStudio) }
    fun color(context: Context, resource: Int): Int {
        val attr = when (resource) {
            R.color.studio_bg -> R.attr.studioBg
            R.color.studio_surface -> R.attr.studioSurface
            R.color.studio_raised -> R.attr.studioRaised
            R.color.studio_text -> R.attr.studioText
            R.color.studio_muted -> R.attr.studioMuted
            R.color.studio_accent -> R.attr.studioAccent
            R.color.studio_green -> R.attr.studioGreen
            else -> return ContextCompat.getColor(context, resource)
        }
        val value = TypedValue()
        return if (context.theme.resolveAttribute(attr, value, true)) value.data else ContextCompat.getColor(context, resource)
    }
    fun showPicker(context: Activity) {
        val previous = selected(context)
        MaterialAlertDialogBuilder(context).setTitle("Appearance")
            .setSingleChoiceItems(arrayOf("Follow system", "Dark", "Light", "Glass"), previous) { dialog, selected ->
                context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit().putInt("mode", selected).apply()
                dialog.dismiss()
                applyTheme(context)
                AppCompatDelegate.setDefaultNightMode(modes[selected])
                if (context is StreamActivity) context.refreshAppearance()
                else if (previous != selected && (previous == 3 || selected == 3 || modes[previous] == modes[selected])) context.recreate()
            }.setNegativeButton("Cancel", null).show()
    }
}
