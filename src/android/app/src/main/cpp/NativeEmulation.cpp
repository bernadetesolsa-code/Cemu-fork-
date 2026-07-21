#include "AndroidFilesystemCallbacks.h"
#include "AndroidInputHelpers.h"
#include "Cafe/CafeSystem.h"
#include "Cafe/HW/Latte/Core/LatteOverlay.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanAPI.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanRenderer.h"
#include "Cafe/OS/libs/snd_core/ax.h"
#include "Cafe/TitleList/TitleId.h"
#include "Cafe/TitleList/TitleList.h"
#include "Cemu/ncrypto/ncrypto.h"
#include "config/ActiveSettings.h"
#include "input/InputManager.h"
#include "JNIUtils.h"
#include "WindowSystem.h"

#if HAS_CUBEB
#include "audio/CubebAPI.h"
#endif // HAS_CUBEB

#include <android/native_window_jni.h>

// forward declaration from main.cpp
void CemuCommonInit();

namespace NativeEmulation
{

	void CreateAudioDevice(IAudioAPI::AudioAPI audioApi, AudioChannels channels, sint32 volume, bool isTV)
	{
		constexpr int AX_FRAMES_PER_GROUP = 4;
		std::unique_lock lock(g_audioMutex);
		auto& audioDevice = isTV ? g_tvAudio : g_padAudio;

		switch (audioApi)
		{
#if HAS_CUBEB
		case IAudioAPI::AudioAPI::Cubeb:
		{
			audioDevice.reset();
			auto deviceDescriptionPtr = std::make_shared<CubebAPI::CubebDeviceDescription>(nullptr, std::string(), std::wstring());
			audioDevice = IAudioAPI::CreateDevice(
				IAudioAPI::AudioAPI::Cubeb,
				deviceDescriptionPtr,
				48000,
				CemuConfig::AudioChannelsToNChannels(channels),
				snd_core::AX_SAMPLES_PER_3MS_48KHZ * AX_FRAMES_PER_GROUP,
				16);
			audioDevice->SetVolume(volume);
			break;
		}
#endif // HAS_CUBEB
		default:
			cemuLog_log(LogType::Force, "Invalid audio api: {}", audioApi);
			break;
		}
	}

	void InitializeAudioDevices()
	{
		auto& config = GetConfig();
		if (!config.tv_device.empty())
			CreateAudioDevice(IAudioAPI::AudioAPI::Cubeb, config.tv_channels, config.tv_volume, true);

		if (!config.pad_device.empty())
			CreateAudioDevice(IAudioAPI::AudioAPI::Cubeb, config.pad_channels, config.pad_volume, false);
	}

	void SetDefaultDeviceController()
	{
		for (size_t i = 0; i < InputManager::kMaxController; ++i)
		{
			auto emulatedController = InputManager::instance().get_controller(i);

			if (!emulatedController)
			{
				continue;
			}

			for (const auto& controller : emulatedController->get_controllers())
			{
				if (controller->api() == InputAPI::Device)
				{
					return;
				}
			}
		}

		if (auto emulatedController = InputManager::instance().get_controller(0))
		{
			auto controller = CreateDefaultDeviceController();
			emulatedController->add_controller(controller);
		}
	}

	void CreateCemuDirectories()
	{
		const auto mlc = ActiveSettings::GetMlcPath();

		// create sys/usr folder in mlc01
		const auto sysFolder = mlc / "sys";
		fs::create_directories(sysFolder);

		const auto usrFolder = mlc / "usr";
		fs::create_directories(usrFolder);
		fs::create_directories(usrFolder / "title/00050000"); // base
		fs::create_directories(usrFolder / "title/0005000c"); // dlc
		fs::create_directories(usrFolder / "title/0005000e"); // update

		// Mii Maker save folders {0x500101004A000, 0x500101004A100, 0x500101004A200},
		fs::create_directories(mlc / "usr/save/00050010/1004a100/user/common/db");
		fs::create_directories(mlc / "usr/save/00050010/1004a000/user/common/db");
		fs::create_directories(mlc / "usr/save/00050010/1004a200/user/common/db");

		// lang files
		auto langDir = mlc / "sys/title/0005001b/1005c000/content";
		fs::create_directories(langDir);

		auto langFile = langDir / "language.txt";

		if (!fs::exists(langFile))
		{
			if (std::ofstream file{langFile})
			{
				const auto langStrings = {"ja", "en", "fr", "de", "it", "es", "ko", "nl", "pt", "ru", "zh"};
				for (const char* lang : langStrings)
					fmt::println(file, R"("{}",)", lang);
			}
		}

		auto countryFile = langDir / "country.txt";
		if (!fs::exists(countryFile))
		{
			if (std::ofstream file{countryFile})
			{
				for (sint32 i = 0; i < 201; i++)
				{
					const char* countryCode = NCrypto::GetCountryAsString(i);
					if (boost::iequals(countryCode, "NN"))
						fmt::println(file, "NULL,");
					else
						fmt::println(file, R"("{}",)", countryCode);
				}
			}
		}

		// cemu directories
		fs::create_directories(ActiveSettings::GetConfigPath("controllerProfiles"));
		fs::create_directories(ActiveSettings::GetUserDataPath("memorySearcher"));
	}

	enum PrepareTitleResult : sint32
	{
		SUCCESSFUL = 0,
		ERROR_GAME_BASE_FILES_NOT_FOUND = 1,
		ERROR_NO_DISC_KEY = 2,
		ERROR_NO_TITLE_TIK = 3,
		ERROR_UNKNOWN = 4,
	};

	class TestSurface
	{
	  public:
		TestSurface()
		{
			JNIEnv* env = JNIUtils::GetEnv();

			jclass surfaceTextureClass = env->FindClass("android/graphics/SurfaceTexture");
			jmethodID ctorSurfaceTexture = env->GetMethodID(surfaceTextureClass, "<init>", "(I)V");
			jobject localSurfaceTexture = env->NewObject(surfaceTextureClass, ctorSurfaceTexture, 0);

			jclass surfaceClass = env->FindClass("android/view/Surface");
			jmethodID ctorSurface = env->GetMethodID(surfaceClass, "<init>", "(Landroid/graphics/SurfaceTexture;)V");
			jobject localSurface = env->NewObject(surfaceClass, ctorSurface, localSurfaceTexture);

			m_surfaceTexture = env->NewGlobalRef(localSurfaceTexture);
			m_surface = env->NewGlobalRef(localSurface);

			m_window = ANativeWindow_fromSurface(env, m_surface);
			ANativeWindow_acquire(m_window);

			env->DeleteLocalRef(localSurfaceTexture);
			env->DeleteLocalRef(localSurface);
			env->DeleteLocalRef(surfaceTextureClass);
			env->DeleteLocalRef(surfaceClass);
		}

			~TestSurface()
			{
				JNIEnv* env = JNIUtils::GetEnv();
				if (!env) return;
	
				if (m_window) {
					ANativeWindow_release(m_window);
					m_window = nullptr;
				}
	
				if (m_surface) {
					jclass surfaceClass = env->FindClass("android/view/Surface");
					if (surfaceClass) {
						jmethodID releaseSurface = env->GetMethodID(surfaceClass, "release", "()V");
						env->CallVoidMethod(m_surface, releaseSurface);
						env->DeleteLocalRef(surfaceClass);
					}
					env->DeleteGlobalRef(m_surface);
					m_surface = nullptr;
				}
	
				if (m_surfaceTexture) {
					jclass surfaceTextureClass = env->FindClass("android/graphics/SurfaceTexture");
					if (surfaceTextureClass) {
						jmethodID releaseSurfaceTexture = env->GetMethodID(surfaceTextureClass, "release", "()V");
						env->CallVoidMethod(m_surfaceTexture, releaseSurfaceTexture);
						env->DeleteLocalRef(surfaceTextureClass);
					}
					env->DeleteGlobalRef(m_surfaceTexture);
					m_surfaceTexture = nullptr;
				}
			}

		ANativeWindow* getWindow()
		{
			return m_window;
		}

	  private:
		ANativeWindow* m_window;
		jobject m_surface = nullptr;
		jobject m_surfaceTexture = nullptr;
	};
} // namespace NativeEmulation

extern "C" [[maybe_unused]] JNIEXPORT void JNICALL
Java_info_cemu_cemu_nativeinterface_NativeEmulation_setReplaceTVWithPadView([[maybe_unused]] JNIEnv* env, [[maybe_unused]] jclass clazz, jboolean swapped)
{
	// Emulate pressing the TAB key for showing DRC instead of TV
	WindowSystem::GetWindowInfo().set_keystate(static_cast<uint32>(WindowSystem::PlatformKeyCodes::TAB), swapped);
}

extern "C" [[maybe_unused]] JNIEXPORT void JNICALL
Java_info_cemu_cemu_nativeinterface_NativeEmulation_initializeEmulation([[maybe_unused]] JNIEnv* env, [[maybe_unused]] jclass clazz)
{
	FilesystemAndroid::SetFilesystemCallbacks(std::make_shared<AndroidFilesystemCallbacks>());
	GetConfigHandle().SetFilename(ActiveSettings::GetConfigPath("settings.xml").generic_wstring());
	NativeEmulation::CreateCemuDirectories();
	NetworkConfig::LoadOnce();
	ActiveSettings::Init();
	LatteOverlay_init();
	CemuCommonInit();

	// Wire WindowSystem::ShowErrorDialog() to the Kotlin-side NativeEmulation.FatalErrorState,
	// so fatal errors happening on background native threads (e.g. LatteThread failing during
	// boot, well after launchTitle() already returned "success" to Kotlin) actually reach the
	// user instead of leaving the UI stuck on a frozen game screen.
	// The jclass is looked up and turned into a global ref here, on the JNI thread that is
	// calling us, which has the correct (app) classloader. FindClass called later from a raw
	// native thread (like LatteThread) would use the wrong classloader and silently fail, so we
	// must never look this class up lazily from inside the callback itself.
	static JNIUtils::Scopedjclass s_fatalErrorStateClass("info/cemu/cemu/nativeinterface/NativeEmulation$FatalErrorState");
	jmethodID reportMethodId = env->GetStaticMethodID(*s_fatalErrorStateClass, "report", "(Ljava/lang/String;)V");
	WindowSystem::SetErrorDialogCallback([reportMethodId](std::string_view message, std::string_view title) {
		std::string fullMessage = title.empty() ? std::string(message) : fmt::format("{}: {}", title, message);
		JNIUtils::FiberSafeJNICall([&](JNIEnv* jniEnv) {
			jstring jMessage = JNIUtils::ToJString(jniEnv, fullMessage);
			jniEnv->CallStaticVoidMethod(*s_fatalErrorStateClass, reportMethodId, jMessage);
			jniEnv->DeleteLocalRef(jMessage);
		});
	});

	// Wire WindowSystem::NotifyGameExited() to the Kotlin-side NativeEmulation.GameExitedState,
	// so that when the title-launch thread finishes on its own (e.g. "Return to Wii U Menu", or
	// the PPC scheduler simply returning), the Android UI is told the game is over instead of
	// staying stuck on the last rendered frame with no way back. Same class-lookup caveat as
	// above: this must be resolved here, on the JNI thread, not lazily from the background thread
	// that ends up invoking the callback.
	static JNIUtils::Scopedjclass s_gameExitedStateClass("info/cemu/cemu/nativeinterface/NativeEmulation$GameExitedState");
	jmethodID gameExitedReportMethodId = env->GetStaticMethodID(*s_gameExitedStateClass, "report", "()V");
	WindowSystem::SetGameExitedCallback([gameExitedReportMethodId]() {
		JNIUtils::FiberSafeJNICall([&](JNIEnv* jniEnv) {
			jniEnv->CallStaticVoidMethod(*s_gameExitedStateClass, gameExitedReportMethodId);
		});
	});
}

extern "C" [[maybe_unused]] JNIEXPORT void JNICALL
	Java_info_cemu_cemu_nativeinterface_NativeEmulation_initializeRenderer(JNIEnv* env, [[maybe_unused]] jclass clazz)
	{
		static std::unique_ptr<NativeEmulation::TestSurface> testSurface;
	
		try {
			if (!InitializeGlobalVulkan()) {
				cemuLog_log(LogType::Force, "JNI: Failed to initialize global Vulkan loader");
			}
			
			JNIUtils::HandleNativeException(env, [&]() {
				if (!testSurface) {
					testSurface = std::make_unique<NativeEmulation::TestSurface>();
				}
	
				if (testSurface && testSurface->getWindow()) {
					WindowSystem::GetWindowInfo().window_main.surface = testSurface->getWindow();
				}
	
				if (!g_renderer) {
					g_renderer = std::make_unique<VulkanRenderer>();
				}
			});
		} catch (const std::exception& ex) {
			cemuLog_log(LogType::Force, "JNI: Exception during initializeRenderer: {}", ex.what());
		} catch (...) {
			cemuLog_log(LogType::Force, "JNI: Unknown exception during initializeRenderer");
		}
	}

extern "C" [[maybe_unused]] JNIEXPORT void JNICALL
Java_info_cemu_cemu_nativeinterface_NativeEmulation_setDPI([[maybe_unused]] JNIEnv* env, [[maybe_unused]] jclass clazz, jfloat dpi)
{
	auto& windowInfo = WindowSystem::GetWindowInfo();
	windowInfo.dpi_scale = windowInfo.pad_dpi_scale = dpi;
}

extern "C" [[maybe_unused]] JNIEXPORT void JNICALL
Java_info_cemu_cemu_nativeinterface_NativeEmulation_clearPadSurface([[maybe_unused]] JNIEnv* env, [[maybe_unused]] jclass clazz)
{
	VulkanRenderer::GetInstance()->StopUsingPadAndWait();
	WindowSystem::GetWindowInfo().pad_open = false;
}

extern "C" [[maybe_unused]] JNIEXPORT jboolean JNICALL
Java_info_cemu_cemu_nativeinterface_NativeEmulation_supportsLoadingCustomDriver([[maybe_unused]] JNIEnv* env, [[maybe_unused]] jclass clazz)
{
	return SupportsLoadingCustomDriver();
}

extern "C" [[maybe_unused]] JNIEXPORT void JNICALL
Java_info_cemu_cemu_nativeinterface_NativeEmulation_setSurface(JNIEnv* env, [[maybe_unused]] jclass clazz, jobject surface, jboolean isMainCanvas)
{
	JNIUtils::HandleNativeException(env, [&]() {
		auto& windowHandleInfo = isMainCanvas ? WindowSystem::GetWindowInfo().canvas_main : WindowSystem::GetWindowInfo().canvas_pad;
		auto oldWindow = windowHandleInfo.surface.load();
		if (oldWindow != nullptr)
			ANativeWindow_release(static_cast<ANativeWindow*>(oldWindow));
		if (surface == nullptr)
		{
			// caller is clearing the surface (Kotlin side declares this parameter as Surface?) - nothing to acquire
			windowHandleInfo.surface = nullptr;
			windowHandleInfo.surface.notify_all();
			return;
		}
		auto newSurface = ANativeWindow_fromSurface(env, surface);
		if (newSurface == nullptr)
		{
			// ANativeWindow_fromSurface can fail (e.g. surface already released on the Java side by the time this
			// runs); calling ANativeWindow_acquire(nullptr) would crash, so bail out instead of dereferencing null.
			windowHandleInfo.surface = nullptr;
			windowHandleInfo.surface.notify_all();
			return;
		}
		ANativeWindow_acquire(newSurface);
		windowHandleInfo.surface = newSurface;
		windowHandleInfo.surface.notify_all();
	});
}

extern "C" [[maybe_unused]] JNIEXPORT void JNICALL
Java_info_cemu_cemu_nativeinterface_NativeEmulation_initializeSurface(JNIEnv* env, [[maybe_unused]] jclass clazz, jboolean isMainCanvas)
{
	JNIUtils::HandleNativeException(env, [&]() {
		int width, height;
		if (isMainCanvas)
		{
			WindowSystem::GetWindowPhysSize(width, height);
		}
		else
		{
			WindowSystem::GetPadWindowPhysSize(width, height);
			WindowSystem::GetWindowInfo().pad_open = true;
		}

		VulkanRenderer::GetInstance()->InitializeSurface({width, height}, isMainCanvas);
	});
}

extern "C" [[maybe_unused]] JNIEXPORT void JNICALL
Java_info_cemu_cemu_nativeinterface_NativeEmulation_setSurfaceSize([[maybe_unused]] JNIEnv* env, [[maybe_unused]] jclass clazz, jint width, jint height, jboolean isMainCanvas)
{
	auto& windowInfo = WindowSystem::GetWindowInfo();
	if (isMainCanvas)
	{
		windowInfo.width = windowInfo.phys_width = width;
		windowInfo.height = windowInfo.phys_height = height;
	}
	else
	{
		windowInfo.pad_width = windowInfo.phys_pad_width = width;
		windowInfo.pad_height = windowInfo.phys_pad_height = height;
	}
}

extern "C" [[maybe_unused]] JNIEXPORT void JNICALL
Java_info_cemu_cemu_nativeinterface_NativeEmulation_initializeSystems(JNIEnv* env, [[maybe_unused]] jclass clazz)
{
	JNIUtils::HandleNativeException(env, [&]() {
		NativeEmulation::SetDefaultDeviceController();
		WindowSystem::GetWindowInfo().set_keystatesup();
		NativeEmulation::InitializeAudioDevices();
	});
}

extern "C" [[maybe_unused]] JNIEXPORT jint JNICALL
Java_info_cemu_cemu_nativeinterface_NativeEmulation_prepareTitle([[maybe_unused]] JNIEnv* env, [[maybe_unused]] jclass clazz, jstring launchPathJava)
{
	using enum NativeEmulation::PrepareTitleResult;

	// This is the very first step of booting a title - parsing the RPX/title data and setting
	// up the CPU JIT (PPCRecompiler_init/SetupMemorySpace), all before any GPU/renderer code
	// runs at all. An uncaught C++ exception crossing back out over the JNI boundary here is
	// undefined behavior and crashes the app, same as the uncaught-exception bugs already fixed
	// elsewhere - except this one would explain a crash that happens identically regardless of
	// GPU vendor, since no graphics work has started yet. Unlike the rest of this file, this
	// function was not wrapped in JNIUtils::HandleNativeException at all, so any exception here
	// (corrupted/unexpected title data, JIT init failure, etc.) previously had nothing catching
	// it. We can't use HandleNativeException directly since it discards the return value and
	// this function needs to return a PrepareTitleResult code, so the same conversion is done
	// manually here instead.
	try
	{
		fs::path launchPath = JNIUtils::FromJString(env, launchPathJava);

		TitleInfo launchTitle{launchPath};

		if (launchTitle.IsValid())
		{
			// the title might not be in the TitleList, so we add it as a temporary entry
			CafeTitleList::AddTitleFromPath(launchPath);
			// title is valid, launch from TitleId
			TitleId baseTitleId;
			if (!CafeTitleList::FindBaseTitleId(launchTitle.GetAppTitleId(), baseTitleId))
			{
				return ERROR_GAME_BASE_FILES_NOT_FOUND;
			}
			CafeSystem::PREPARE_STATUS_CODE r = CafeSystem::PrepareForegroundTitle(baseTitleId);
			if (r != CafeSystem::PREPARE_STATUS_CODE::SUCCESS)
			{
				return ERROR_UNKNOWN;
			}
		}
		else // if (launchTitle.GetFormat() == TitleInfo::TitleDataFormat::INVALID_STRUCTURE )
		{
			// title is invalid, if it's an RPX/ELF we can launch it directly
			// otherwise it's an error
			CafeTitleFileType fileType = DetermineCafeSystemFileType(launchPath);
			if (fileType == CafeTitleFileType::RPX || fileType == CafeTitleFileType::ELF)
			{
				CafeSystem::PREPARE_STATUS_CODE r = CafeSystem::PrepareForegroundTitleFromStandaloneRPX(launchPath);
				if (r != CafeSystem::PREPARE_STATUS_CODE::SUCCESS)
				{
					return ERROR_UNKNOWN;
				}
			}
			else if (launchTitle.GetInvalidReason() == TitleInfo::InvalidReason::NO_DISC_KEY)
			{
				return ERROR_NO_DISC_KEY;
			}
			else if (launchTitle.GetInvalidReason() == TitleInfo::InvalidReason::NO_TITLE_TIK)
			{
				return ERROR_NO_TITLE_TIK;
			}
			else
			{
				return ERROR_UNKNOWN;
			}
		}

		return SUCCESSFUL;
	}
	catch (const std::exception& ex)
	{
		cemuLog_log(LogType::Force, "JNI: Exception during prepareTitle: {}", ex.what());
		jclass exceptionClass = env->FindClass("info/cemu/cemu/nativeinterface/NativeException");
		env->ThrowNew(exceptionClass, ex.what());
		return ERROR_UNKNOWN;
	}
	catch (...)
	{
		cemuLog_log(LogType::Force, "JNI: Unknown exception during prepareTitle");
		jclass exceptionClass = env->FindClass("info/cemu/cemu/nativeinterface/NativeException");
		env->ThrowNew(exceptionClass, "Unknown native exception while preparing title");
		return ERROR_UNKNOWN;
	}
}

extern "C" [[maybe_unused]] JNIEXPORT void JNICALL
Java_info_cemu_cemu_nativeinterface_NativeEmulation_launchTitle([[maybe_unused]] JNIEnv* env, [[maybe_unused]] jclass clazz)
{
	CafeSystem::LaunchForegroundTitle();
}

extern "C" [[maybe_unused]] JNIEXPORT void JNICALL
Java_info_cemu_cemu_nativeinterface_NativeEmulation_pauseTitle([[maybe_unused]] JNIEnv* env, [[maybe_unused]] jclass clazz)
{
	CafeSystem::PauseTitle();
}

extern "C" [[maybe_unused]] JNIEXPORT void JNICALL
Java_info_cemu_cemu_nativeinterface_NativeEmulation_resumeTitle([[maybe_unused]] JNIEnv* env, [[maybe_unused]] jclass clazz)
{
	CafeSystem::ResumeTitle();
}
