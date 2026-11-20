#include "main.h"
#include "i18n.h"

#include <cctype>
#include <cstdio>
#include <string>

namespace
{
	enum class Lang
	{
		English,
		Chinese,
	};

	struct Text
	{
		const wchar_t* title;
		const wchar_t* body;
	};

	// One entry per language; the order matches the Lang enum.
	const Text kConversionFailed[] = {
		{
			L"ZeroIV - update.rpf could not be prepared",
			L"ZeroIV could not register the generated DLC packs in update.rpf, so they "
			L"were not loaded this session.\n\n"
			L"The game itself is unaffected. See ZeroIV.log for the reason.",
		},
		{
			L"ZeroIV - update.rpf 处理失败",
			L"ZeroIV 未能在 update.rpf 中注册动态生成的 DLC，本次启动没有加载它们。\n\n"
			L"游戏本身不受影响。具体原因见 ZeroIV.log。",
		},
	};

	Lang g_language = Lang::English;
	bool g_inited = false;

	Lang detect()
	{
		const std::string setting = config::get_value<std::string>("ui", "language", "auto");

		std::string lowered = setting;
		for (auto& c : lowered)
			c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));

		if (lowered == "zh" || lowered == "chinese")
			return Lang::Chinese;
		if (lowered == "en" || lowered == "english")
			return Lang::English;

		// A Chinese UI language is everything in the 0x04 primary language group.
		const LANGID id = ::GetUserDefaultUILanguage();
		if ((id & 0x3FF) == 0x04)
			return Lang::Chinese;
		return Lang::English;
	}
}

namespace i18n
{
	void init()
	{
		if (g_inited)
			return;
		g_inited = true;
		g_language = detect();
		logger::write("info", "ui: language %s ([ui] language=auto|en|zh)",
			g_language == Lang::Chinese ? "zh" : "en");
	}

	void show(Message message)
	{
		init();

		const Text* table = nullptr;
		switch (message)
		{
		case Message::ConversionFailed: table = kConversionFailed; break;
		default: return;
		}

		const Text& text = table[g_language == Lang::Chinese ? 1 : 0];
		// TOPMOST because the box is raised from the loading path, which on a
		// fullscreen boot would otherwise hide it behind the game window.
		::MessageBoxW(nullptr, text.body, text.title,
			MB_OK | MB_ICONWARNING | MB_SETFOREGROUND | MB_TOPMOST);
	}

	void showIncompatibleResources(const std::vector<std::string>& packs, int fileCount)
	{
		init();
		if (packs.empty() || fileCount <= 0)
			return;

		const bool zh = g_language == Lang::Chinese;
		const bool enhanced = IsEnhanced();

		auto widen = [](const std::string& utf8) -> std::wstring {
			if (utf8.empty())
				return {};
			const int need = ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
				static_cast<int>(utf8.size()), nullptr, 0);
			if (need <= 0)
				return std::wstring(utf8.begin(), utf8.end());
			std::wstring out(static_cast<size_t>(need), L'\0');
			::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()),
				out.data(), need);
			return out;
		};

		std::wstring list;
		const size_t shown = packs.size() < 12 ? packs.size() : 12;
		for (size_t i = 0; i < shown; ++i)
		{
			if (i)
				list += L", ";
			list += widen(packs[i]);
		}
		if (packs.size() > shown)
			list += zh ? L" 等" : L", ...";

		wchar_t body[2048];
		if (zh)
		{
			const wchar_t* need = enhanced
				? L"请先用 Gen8→Gen9 转换工具处理后再进游戏，否则游戏可能会崩溃。"
				: L"这些资源是 Enhanced (Gen9) 格式，在 Legacy 上可能无法正常使用。";
			std::swprintf(body, sizeof(body) / sizeof(body[0]),
				L"检测到 %d 个资源文件与当前游戏版本不兼容（涉及 %zu 个模组）：\n\n%ls\n\n%ls\n\n"
				L"完整列表见 ZeroIV.log 中的 [compat] 行。",
				fileCount, packs.size(), list.c_str(), need);
			::MessageBoxW(nullptr, body, L"ZeroIV - 资源版本不兼容",
				MB_OK | MB_ICONWARNING | MB_SETFOREGROUND | MB_TOPMOST);
		}
		else
		{
			const wchar_t* need = enhanced
				? L"Convert them Gen8→Gen9 before playing; spawning them can crash the game."
				: L"These assets are Enhanced (Gen9) and may not work on Legacy.";
			std::swprintf(body, sizeof(body) / sizeof(body[0]),
				L"%d resource file(s) do not match this game build (%zu pack(s)):\n\n%ls\n\n%ls\n\n"
				L"See [compat] lines in ZeroIV.log for the full list.",
				fileCount, packs.size(), list.c_str(), need);
			::MessageBoxW(nullptr, body, L"ZeroIV - incompatible resources",
				MB_OK | MB_ICONWARNING | MB_SETFOREGROUND | MB_TOPMOST);
		}
	}
}
