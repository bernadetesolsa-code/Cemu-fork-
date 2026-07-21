package info.cemu.cemu.nativeinterface

import android.view.Surface
import androidx.annotation.Keep
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asStateFlow

object NativeEmulation {
    // Some native failures (e.g. the GPU/LatteThread failing to initialize the renderer or a
    // built-in shader) happen on a background native thread *after* launchTitle() has already
    // returned "success" to Kotlin - there is no JNI call still on the stack to throw a
    // NativeException through. Without this, that thread previously either crashed the whole
    // process silently, or (after the native-side try/catch fixes) just logged an error nobody
    // ever saw, leaving the UI stuck on the game screen with nothing rendering. This lets native
    // code push a fatal error to the UI at any time, the same way NativeSwkbd.SwkbdState pushes
    // keyboard visibility changes.
    object FatalErrorState {
        private val _message = MutableStateFlow<String?>(null)
        val message = _message.asStateFlow()

        fun consume() {
            _message.value = null
        }

        @Keep
        @JvmStatic
        @Suppress("unused")
        private fun report(message: String?) {
            _message.value = message ?: "Unknown fatal error"
        }
    }

    // Native title-launch runs on its own background thread (CafeSystem::_LaunchTitleThread)
    // which, when the title exits on its own (e.g. "Return to Wii U Menu", or the PPC scheduler
    // simply returning), finishes with nothing left on the JNI call stack to report that back
    // through a normal function return. Without this, the Kotlin side never learns the game is
    // over and EmulationActivity is stuck on the last rendered frame with no way back. Same
    // push-based approach as FatalErrorState above.
    object GameExitedState {
        private val _exited = MutableStateFlow(false)
        val exited = _exited.asStateFlow()

        fun consume() {
            _exited.value = false
        }

        @Keep
        @JvmStatic
        @Suppress("unused")
        private fun report() {
            _exited.value = true
        }
    }

    @JvmStatic
    external fun initializeEmulation()

    @JvmStatic
    external fun setDPI(dpi: Float)


    @JvmStatic
    external fun setSurface(surface: Surface?, isMainCanvas: Boolean)

    @JvmStatic
    external fun initializeSurface(isMainCanvas: Boolean)

    @JvmStatic
    external fun clearPadSurface()

    @JvmStatic
    external fun setSurfaceSize(width: Int, height: Int, isMainCanvas: Boolean)

    @JvmStatic
    external fun initializeRenderer()

    object PrepareTitleResult {
        const val SUCCESSFUL: Int = 0
        const val ERROR_GAME_BASE_FILES_NOT_FOUND: Int = 1
        const val ERROR_NO_DISC_KEY: Int = 2
        const val ERROR_NO_TITLE_TIK: Int = 3
        const val ERROR_UNKNOWN: Int = 4
    }

    @JvmStatic
    external fun prepareTitle(launchPath: String?): Int

    @JvmStatic
    external fun launchTitle()

    @JvmStatic
    external fun pauseTitle()

    @JvmStatic
    external fun resumeTitle()

    @JvmStatic
    external fun initializeSystems()

    @JvmStatic
    external fun setReplaceTVWithPadView(swapped: Boolean)

    @JvmStatic
    external fun supportsLoadingCustomDriver(): Boolean
}
