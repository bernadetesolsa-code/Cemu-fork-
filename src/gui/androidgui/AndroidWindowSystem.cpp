#include "../interface/WindowSystem.h"
#include "Cafe/HW/Latte/Core/Latte.h"
#include "config/ActiveSettings.h"
#include "config/NetworkSettings.h"
#include "config/CemuConfig.h"
#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "Cafe/CafeSystem.h"

WindowSystem::WindowInfo g_window_info{};

std::shared_mutex g_mutex;

void WindowSystem::Create()
{
}

static WindowSystem::ErrorDialogCallback s_errorDialogCallback;

void WindowSystem::SetErrorDialogCallback(WindowSystem::ErrorDialogCallback callback)
{
	s_errorDialogCallback = std::move(callback);
}

void WindowSystem::ShowErrorDialog(std::string_view message, std::string_view title, std::optional<WindowSystem::ErrorCategory> /*errorId*/)
{
	// Always log, in case no callback is registered yet (e.g. very early startup) or the
	// callback itself fails - this must never be the only place an error goes to die silently.
	cemuLog_log(LogType::Force, "Error dialog: {} - {}", title, message);
	if (s_errorDialogCallback)
		s_errorDialogCallback(message, title);
}

WindowSystem::WindowInfo& WindowSystem::GetWindowInfo()
{
	return g_window_info;
}

void WindowSystem::UpdateWindowTitles(bool isIdle, bool isLoading, double fps)
{
}

void WindowSystem::GetWindowSize(int& w, int& h)
{
	w = g_window_info.width;
	h = g_window_info.height;
}

void WindowSystem::GetPadWindowSize(int& w, int& h)
{
	if (g_window_info.pad_open)
	{
		w = g_window_info.pad_width;
		h = g_window_info.pad_height;
	}
	else
	{
		w = 0;
		h = 0;
	}
}

void WindowSystem::GetWindowPhysSize(int& w, int& h)
{
	w = g_window_info.phys_width;
	h = g_window_info.phys_height;
}

void WindowSystem::GetPadWindowPhysSize(int& w, int& h)
{
	if (g_window_info.pad_open)
	{
		w = g_window_info.phys_pad_width;
		h = g_window_info.phys_pad_height;
	}
	else
	{
		w = 0;
		h = 0;
	}
}

double WindowSystem::GetWindowDPIScale()
{
	return g_window_info.dpi_scale;
}

double WindowSystem::GetPadDPIScale()
{
	return g_window_info.pad_open ? g_window_info.pad_dpi_scale.load() : 1.0;
}

bool WindowSystem::IsPadWindowOpen()
{
	return g_window_info.pad_open;
}

bool WindowSystem::IsKeyDown(uint32 key)
{
	return g_window_info.get_keystate(key);
}

bool WindowSystem::IsKeyDown(PlatformKeyCodes platformKey)
{
	return g_window_info.get_keystate(static_cast<uint32>(platformKey));
}

std::string WindowSystem::GetKeyCodeName(uint32 button)
{
	return fmt::format("key_{}", button);
}

bool WindowSystem::InputConfigWindowHasFocus()
{
	return false;
}

void WindowSystem::NotifyGameLoaded()
{
}

static WindowSystem::GameExitedCallback s_gameExitedCallback;

void WindowSystem::SetGameExitedCallback(WindowSystem::GameExitedCallback callback)
{
	s_gameExitedCallback = std::move(callback);
}

void WindowSystem::NotifyGameExited()
{
	if (s_gameExitedCallback)
		s_gameExitedCallback();
}

void WindowSystem::RefreshGameList()
{
}

void WindowSystem::CaptureInput(const ControllerState& currentState, const ControllerState& lastState)
{
}

bool WindowSystem::IsFullScreen()
{
	return true;
}
