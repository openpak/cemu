#include "Cemu/OpenPak/DeviceIdentity.h"

#include "config/ActiveSettings.h"
#include "config/CemuConfig.h"
#include "config/NetworkSettings.h"
#include "Cemu/Logging/CemuLogging.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <random>

namespace
{
	std::mt19937& Rng()
	{
		static std::mt19937 s_rng(std::random_device{}());
		return s_rng;
	}

	void FillRandom(uint8* out, size_t len)
	{
		std::uniform_int_distribution<int> dist(0, 255);
		std::generate(out, out + len, [&dist]() { return (uint8)dist(Rng()); });
	}

	bool FileWithSizeExists(const fs::path& path, std::uintmax_t expectedSize)
	{
		std::error_code ec;
		if (!fs::is_regular_file(path, ec))
			return false;
		return fs::file_size(path, ec) == expectedSize;
	}

	// otp.bin (1024 bytes), the fields iosu_crypto reads. Unlisted words are
	// zero: nothing else consumes them on this service.
	//  0x87 device id (also the certificate's NG name)
	//  0x88..0x9F device certificate private key (30 bytes)
	//  0xA0 MS value, 0xA1 CA value (certificate subject), 0xA2 NG key id
	//  0xA3..0xDE device certificate signature (60 bytes)
	//  0x120 AES key for MLC RSA keys (unused on this service, filled anyway)
	bool WriteSyntheticOtp(const fs::path& path)
	{
		std::array<uint8, 1024> otp{};
		FillRandom(otp.data() + 0x87 * 4, 4);
		FillRandom(otp.data() + 0x88 * 4, 0x1E);
		FillRandom(otp.data() + 0xA0 * 4, 4);
		FillRandom(otp.data() + 0xA1 * 4, 4);
		FillRandom(otp.data() + 0xA2 * 4, 4);
		FillRandom(otp.data() + 0xA3 * 4, 0x3C);
		FillRandom(otp.data() + 0x120, 16);

		std::ofstream file(path, std::ios::binary | std::ios::trunc);
		if (!file.is_open())
			return false;
		file.write((const char*)otp.data(), otp.size());
		return (bool)file;
	}

	// seeprom.bin (512 bytes): the region word and the serial.
	//  0xA4 region word (the top byte, per SEEPROM_GetRegion)
	//  0xAC(8) + 0xB0(16) serial, read back as one "code+serial" string
	bool WriteSyntheticSeeprom(const fs::path& path)
	{
		std::array<uint8, 512> seeprom{};
		// USA; the account's country drives the ACT country header and games
		// only branch on the region.
		seeprom[0xA4 * 2 + 3] = (uint8)CafeConsoleRegion::USA;

		// Serial in the console's shape: a three-letter code followed by nine
		// digits, NUL-padded across the two reads.
		char serial[13] = "OPK000000000";
		for (size_t i = 3; i < 12; ++i)
			serial[i] = (char)('0' + (std::uniform_int_distribution<int>(0, 9)(Rng())));
		memcpy(seeprom.data() + 0xAC * 2, serial, 8);
		memcpy(seeprom.data() + 0xB0 * 2, serial + 8, 5);

		std::ofstream file(path, std::ios::binary | std::ios::trunc);
		if (!file.is_open())
			return false;
		file.write((const char*)seeprom.data(), seeprom.size());
		return (bool)file;
	}
} // namespace

bool OpenPakDeviceIdentity::EnsureFiles()
{
	if (ActiveSettings::GetNetworkService() != NetworkService::OpenPak)
		return false;

	const auto otpPath = ActiveSettings::GetUserDataPath("otp.bin");
	const auto seepromPath = ActiveSettings::GetUserDataPath("seeprom.bin");
	if (FileWithSizeExists(otpPath, 1024) && FileWithSizeExists(seepromPath, 512))
		return true;

	cemuLog_log(LogType::Force, "OpenPak: synthesising a device identity (otp.bin, seeprom.bin) for this service");
	if (!FileWithSizeExists(otpPath, 1024) && !WriteSyntheticOtp(otpPath))
	{
		cemuLog_log(LogType::Force, "OpenPak: failed to write otp.bin");
		return false;
	}
	if (!FileWithSizeExists(seepromPath, 512) && !WriteSyntheticSeeprom(seepromPath))
	{
		cemuLog_log(LogType::Force, "OpenPak: failed to write seeprom.bin");
		return false;
	}
	return true;
}
