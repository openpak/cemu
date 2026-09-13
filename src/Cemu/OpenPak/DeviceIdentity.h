#pragma once

// OpenPak: the OTP-less path for the OpenPak network service (PRD
// emulator-integration-prd.md NA-1a). A real Wii U derives its device
// identity — device id, serial, device certificate and its key — from the
// console's otp.bin and seeprom.bin, which only console owners can dump.
// The OpenPak adapter does not verify certificate chains (structural checks
// only, and TLS verification is off for this service), so when the active
// account uses the OpenPak service and the dumps are absent, Cemu synthesises
// a device identity once and persists it as otp.bin/seeprom.bin in the user
// data path. Everything downstream — the device certificate generator, the
// device id, the serial, the ACT login headers — then works unmodified.
//
// Files that already exist are never touched: a user with real dumps keeps
// them, whatever service they select.

namespace OpenPakDeviceIdentity
{
	// When the active account's network service is OpenPak, make sure both
	// files exist: synthesise and persist what is missing. Returns true when
	// otp.bin and seeprom.bin are present afterwards.
	bool EnsureFiles();
}
