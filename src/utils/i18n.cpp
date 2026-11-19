#include "main.h"
#include "i18n.h"

#include <cctype>

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
}
