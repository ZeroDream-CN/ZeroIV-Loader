#include "main.h"
#include <cstring>

/*
	List of stuff you can replace with this: https://gist.github.com/martonp96/59f731446c7f17db3f400c2be458c4a4

	How to do it:
	Find something from the list, eg. common:/data/dlclist.xml

	Copy the original dlclist.xml from the files and place it in [your GTA5 folder]/newmods/common/data/dlclist.xml
	Edit the file, add a new entry like <Item>dlcpacks:/mrpd/</Item>
	Place the single player dlc.rpf like [your GTA5 folder]/newmods/dlcpacks/mrpd/dlc.rpf

	Start the game and the modded stuff should be loaded automatically.

	I used this for the example: https://hu.gta5-mods.com/maps/community-mission-row-pd
*/

rage::fiDeviceLocal* platformDevice;
rage::fiDeviceLocal* platformDeviceCRC;
rage::fiDeviceLocal* commonDevice;
rage::fiDeviceLocal* commonDeviceCRC;
rage::fiDeviceLocal* dlcDevice;

bool IsCustomDevice(rage::fiDeviceLocal* dev)
{
	if (dev == platformDevice || dev == platformDeviceCRC || dev == commonDevice || dev == commonDeviceCRC || dev == dlcDevice)
		return true;
	return false;
}

// Mounts one of the newmods overrides. The device is left untouched (and the
// game keeps its stock mount) when the backing directory is missing, because an
// override rooted at a non-existent path makes every lookup through that mount
// point fail.
static bool mount_override(const char* mountPoint, const std::filesystem::path& source,
	const char* virtualPath)
{
	std::error_code ec;
	if (!std::filesystem::is_directory(source, ec))
	{
		logger::write("device", "[%s] %s left stock: %s is missing",
			__FUNCTION__, mountPoint, source.string().c_str());
		return false;
	}

	rage::fiDeviceLocal* device = nullptr;
	if (std::strcmp(mountPoint, "platform:/") == 0)
		device = platformDevice;
	else if (std::strcmp(mountPoint, "platformcrc:/") == 0)
		device = platformDeviceCRC;
	else if (std::strcmp(mountPoint, "common:/") == 0)
		device = commonDevice;
	else if (std::strcmp(mountPoint, "commoncrc:/") == 0)
		device = commonDeviceCRC;
	else
		device = dlcDevice;

	device->SetPath(virtualPath, true, nullptr);

	if (!device->Mount(mountPoint))
	{
		logger::write("device", "[%s] %s left stock: mount failed", __FUNCTION__, mountPoint);
		return false;
	}

	logger::write("device", "[%s] %s mounted from %s", __FUNCTION__, mountPoint, source.string().c_str());
	return true;
}

void(*InitialMountOrig)();
void InitialMountHook()
{
	InitialMountOrig();

	// Mounting an override device on a directory that does not exist leaves the
	// game with a mount point that resolves to nothing, and GTA Enhanced then
	// retries forever instead of booting. Only mount what is actually there.
	if (!config::get_value<bool>("config", "modsOverride", true))
	{
		logger::write("device", "[%s] newmods overrides disabled by config", __FUNCTION__);
		return;
	}

	std::filesystem::path cwd = std::filesystem::current_path() / "newmods";
	cwd.make_preferred();

	std::error_code ec;
	if (!std::filesystem::is_directory(cwd, ec))
	{
		logger::write("device", "[%s] %s is missing, using stock devices",
			__FUNCTION__, cwd.string().c_str());
		return;
	}

	logger::write("device", "[%s] Mods path: %s", __FUNCTION__, cwd.string().c_str());

	rage::fiDeviceLocal* rootDevice = new rage::fiDeviceLocal();
	rootDevice->SetPath(cwd.string().c_str(), true, nullptr);

	if (!rootDevice->Mount("mods:/"))
	{
		logger::write("device", "[%s] mods:/ could not be mounted, using stock devices", __FUNCTION__);
		return;
	}

	logger::write("device", "[%s] Root device mounted!", __FUNCTION__);

	platformDevice = new rage::fiDeviceLocal();
	platformDeviceCRC = new rage::fiDeviceLocal();
	commonDevice = new rage::fiDeviceLocal();
	commonDeviceCRC = new rage::fiDeviceLocal();
	dlcDevice = new rage::fiDeviceLocal();

	mount_override("platform:/", cwd / "platform", "mods:/platform/");
	mount_override("platformcrc:/", cwd / "platform", "mods:/platform/");
	mount_override("common:/", cwd / "common", "mods:/common/");
	mount_override("commoncrc:/", cwd / "common", "mods:/common/");
	mount_override("dlcpacks:/", cwd / "dlcpacks", "mods:/dlcpacks/");
}

static memory::func<rage::fiDevice*, const char*, bool> GetDeviceHook(IsEnhanced() ? "41 57 41 56 41 55 41 54 56 57 55 53 48 81 ec ? ? ? ? 89 d6 49 89 ce" :
	"48 89 5C 24 ? 88 54 24 10 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 20 48 8D 15 ? ? ? ? 41 B8");

char (*OpenArchiveOrig)(rage::fiPackfile* a1, const char* path, char smth, int32_t type, __int64 a5);
char OpenArchiveHook(rage::fiPackfile* a1, const char* path, char smth, int32_t type, __int64 a5)
{
	auto device = GetDeviceHook(path, true);
	if (device && IsCustomDevice((rage::fiDeviceLocal*)device))
	{
		if (device->GetAttributes(path) & FILE_ATTRIBUTE_DIRECTORY)
		{
			logger::log("device", "[%s] Archive %s is a directory!", __FUNCTION__, path);
			type = 2;
		}
	}

	logger::log("device", "[%s] Opening archive %s %d %d", __FUNCTION__, path, smth, type);

	// A mod archive is only useful once the game mounts it. Records are cheap
	// and rare, and this is the one line that tells a mounted pack apart from a
	// pack that was redirected to disk but never parsed: without it there is no
	// way to tell whether a car failed to appear because the engine rejected it
	// or because it never read it at all.
	if (path)
	{
		const char* hit = nullptr;
		for (const char* p = path; *p; ++p)
		{
			if ((p[0] == 'n' || p[0] == 'N') &&
				(p[1] == 'i' || p[1] == 'I') &&
				(p[2] == 's' || p[2] == 'S') &&
				(p[3] == 'a' || p[3] == 'A'))
			{
				hit = p;
				break;
			}
		}
		if (hit)
		{
			const char result = OpenArchiveOrig(a1, path, smth, type, a5);
			logger::write("pack", "[%s] mounted %s -> %d (type %d)", __FUNCTION__, path, (int)result, (int)type);
			return result;
		}
	}

	return OpenArchiveOrig(a1, path, smth, type, a5);
}

static memory::InitFuncs CustomDevice([] {

	if (IsEnhanced()) {
		auto mem = memory::scan("e8 ? ? ? ? 48 8d 05 ? ? ? ? 66 c7 44 58 02 00 00 66 89 3d").add(26);
		InitialMountOrig = mem.add(1).rip().as<decltype(InitialMountOrig)>();
		mem.set_call(InitialMountHook);

		memory::scan("41 57 41 56 41 55 41 54 56 57 55 53 48 81 ec ? ? ? ? 45 89 cd 45 89 c6 49 89 d7 48 89 ce 8b 05")
			.hook(OpenArchiveHook, &OpenArchiveOrig);
	}
	else {

		auto mem = memory::scan("0F B7 05 ? ? ? ? 48 03 C3 44 88 34 38").add(21);
		InitialMountOrig = mem.add(1).rip().as<decltype(InitialMountOrig)>();
		mem.set_call(InitialMountHook);

		memory::scan("48 8B C4 48 89 58 10 48 89 70 18 48 89 78 20 55 41 54 41 55 41 56 41 57 48 8D 68 98 48 81 EC ? ? ? ? 41 8B F9 4C 8B E2 48 8B D9 4C 8B CA 48 8D 05")
			.hook(OpenArchiveHook, &OpenArchiveOrig);
	}
});