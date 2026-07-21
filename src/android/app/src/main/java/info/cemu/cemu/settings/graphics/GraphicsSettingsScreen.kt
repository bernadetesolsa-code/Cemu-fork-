package info.cemu.cemu.settings.graphics

import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import info.cemu.cemu.common.ui.components.Button
import info.cemu.cemu.common.ui.components.ScreenContent
import info.cemu.cemu.common.ui.components.SingleSelection
import info.cemu.cemu.common.ui.components.Slider
import info.cemu.cemu.common.ui.components.Toggle
import info.cemu.cemu.common.ui.localization.tr
import info.cemu.cemu.nativeinterface.NativeEmulation
import info.cemu.cemu.nativeinterface.NativeSettings

private val ScalingFilterChoices = listOf(
    NativeSettings.ScalingFilter.BILINEAR_FILTER,
    NativeSettings.ScalingFilter.BICUBIC_FILTER,
    NativeSettings.ScalingFilter.BICUBIC_HERMITE_FILTER,
    NativeSettings.ScalingFilter.NEAREST_NEIGHBOR_FILTER
)

private const val EMULATION_SPEED_MIN = 100
private const val EMULATION_SPEED_MAX = 200
private const val EMULATION_SPEED_STEP = 25
private const val EMULATION_SPEED_STEPS =
    (EMULATION_SPEED_MAX - EMULATION_SPEED_MIN) / EMULATION_SPEED_STEP - 1

@Composable
fun GraphicsSettingsScreen(navigateBack: () -> Unit, goToCustomDriversSettings: () -> Unit) {
    val supportsLoadingCustomDrivers = remember { NativeEmulation.supportsLoadingCustomDriver() }

    ScreenContent(
        appBarText = tr("Graphics settings"),
        navigateBack = navigateBack,
    ) {
        if (supportsLoadingCustomDrivers) {
            Button(
                label = tr("Custom drivers"),
                onClick = goToCustomDriversSettings
            )
        }
        Toggle(
            label = tr("Async shader compile"),
            description = tr("Enables async shader and pipeline compilation. Reduces stutter at the cost of objects not rendering for a short time.\nVulkan only"),
            initialCheckedState = NativeSettings::getAsyncShaderCompile,
            onCheckedChanged = NativeSettings::setAsyncShaderCompile,
        )
        SingleSelection(
            label = tr("VSync"),
            initialChoice = NativeSettings::getVsyncMode,
            onChoiceChanged = NativeSettings::setVsyncMode,
            choiceToString = { vsyncModeToString(it) },
            choices = listOf(
                NativeSettings.VSyncMode.OFF,
                NativeSettings.VSyncMode.DOUBLE_BUFFERING,
                NativeSettings.VSyncMode.TRIPLE_BUFFERING
            ),
        )
        Toggle(
            label = tr("Accurate barriers"),
            description = tr("Disabling the accurate barriers option will lead to flickering graphics but may improve performance. It is highly recommended to leave it turned on"),
            initialCheckedState = NativeSettings::getAccurateBarriers,
            onCheckedChanged = NativeSettings::setAccurateBarriers,
        )
        var limitToHalfFps by rememberSaveable { mutableStateOf(NativeSettings.getLimitToHalfFps()) }
        Toggle(
            label = tr("Limit to 30 FPS"),
            description = tr("Locks games that target 60 FPS to a stable 30 FPS instead. Useful on slower devices where a game can't consistently reach 60 FPS - trades peak framerate for a smoother, more consistent one. Has no effect on games that already have their own graphic pack timing fix active. Requires restarting the game."),
            checked = limitToHalfFps,
            onCheckedChanged = {
                limitToHalfFps = it
                NativeSettings.setLimitToHalfFps(it)
            },
        )
        Slider(
            label = tr("Emulation speed"),
            initialValue = NativeSettings::getEmulationSpeedPercent,
            valueFrom = EMULATION_SPEED_MIN,
            valueTo = EMULATION_SPEED_MAX,
            steps = EMULATION_SPEED_STEPS,
            enabled = !limitToHalfFps,
            onValueChange = NativeSettings::setEmulationSpeedPercent,
            labelFormatter = {
                if (limitToHalfFps)
                    tr("Disabled while \"Limit to 30 FPS\" is on")
                else
                    tr("{0}% - the game genuinely plays at {1}x speed, not just the display", it, "%.2f".format(it / 100f))
            },
        )
        SingleSelection(
            label = tr("Fullscreen scaling"),
            initialChoice = NativeSettings::getFullscreenScaling,
            onChoiceChanged = NativeSettings::setFullscreenScaling,
            choiceToString = { fullscreenScalingModeToString(it) },
            choices = listOf(
                NativeSettings.FullscreenScaling.KEEP_ASPECT_RATIO,
                NativeSettings.FullscreenScaling.STRETCH
            ),
        )
        SingleSelection(
            label = tr("Upscale filter"),
            initialChoice = NativeSettings::getUpscalingFilter,
            onChoiceChanged = NativeSettings::setUpscalingFilter,
            choiceToString = { scalingFilterToString(it) },
            choices = ScalingFilterChoices,
        )
        SingleSelection(
            label = tr("Downscale filter"),
            initialChoice = NativeSettings::getDownscalingFilter,
            onChoiceChanged = NativeSettings::setDownscalingFilter,
            choiceToString = { scalingFilterToString(it) },
            choices = ScalingFilterChoices,
        )
    }
}

private fun scalingFilterToString(scalingFilter: Int) = when (scalingFilter) {
    NativeSettings.ScalingFilter.BILINEAR_FILTER -> tr("Bilinear")
    NativeSettings.ScalingFilter.BICUBIC_FILTER -> tr("Bicubic")
    NativeSettings.ScalingFilter.BICUBIC_HERMITE_FILTER -> tr("Hermite")
    NativeSettings.ScalingFilter.NEAREST_NEIGHBOR_FILTER -> tr("Nearest neighbor")
    else -> throw IllegalArgumentException("Invalid scaling filter:  $scalingFilter")
}

private fun vsyncModeToString(vsyncMode: Int) = when (vsyncMode) {
    NativeSettings.VSyncMode.OFF -> tr("Off")
    NativeSettings.VSyncMode.DOUBLE_BUFFERING -> tr("Double buffering")
    NativeSettings.VSyncMode.TRIPLE_BUFFERING -> tr("Triple buffering")
    else -> throw IllegalArgumentException("Invalid vsync mode: $vsyncMode")
}

private fun fullscreenScalingModeToString(fullscreenScaling: Int) = when (fullscreenScaling) {
    NativeSettings.FullscreenScaling.KEEP_ASPECT_RATIO -> tr("Keep aspect ratio")
    NativeSettings.FullscreenScaling.STRETCH -> tr("Stretch")
    else -> throw IllegalArgumentException("Invalid fullscreen scaling mode:  $fullscreenScaling")
}