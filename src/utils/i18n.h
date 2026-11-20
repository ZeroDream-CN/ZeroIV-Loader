#pragma once

#include <string>
#include <vector>

// The loader's only user-facing surface. Everything else goes to the log.
//
// With the offline conversion there is normally nothing to say: mods are applied
// before the engine starts, so there is no restart and no prompt. The one case
// worth interrupting a launch for is a failure, where the player would otherwise
// only see "the mod did not load" and have no idea why.
namespace i18n
{
	enum class Message
	{
		ConversionFailed, // update.rpf could not be prepared
	};

	void init();

	// Shows `message` as a modal information box. Safe to call from any thread.
	void show(Message message);

	// Warns that add-on resources are the wrong generation for this game build
	// (Gen8 assets on Enhanced, or Gen9 on Legacy). `packs` are folder names;
	// `fileCount` is the number of incompatible files logged.
	void showIncompatibleResources(const std::vector<std::string>& packs, int fileCount);
}
