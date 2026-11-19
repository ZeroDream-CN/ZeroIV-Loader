#pragma once

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
}
