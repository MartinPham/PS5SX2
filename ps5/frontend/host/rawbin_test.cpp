// PS5 port frontend, PC test (2026-10-08, AI-assisted; testers: ".bin images don't load"): the shelf lists raw CD images
// (.bin, .img) and reads their serials whatever their sector layout (2352 bytes with the data 24 or 16 in, 2448 with
// subcode, plain 2048), and leaves out a .bin under 16 MB (a BIOS dump, a memory card) and one with no ISO 9660 volume
// (the audio tracks of a .cue/.bin set).
//   ps5/frontend/host/test-rawbin.sh
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later
#include "fe_games.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include <cstdlib>
static int fails = 0;
static void Check(bool ok, const std::string& w) { std::printf("%s  %s\n", ok ? "PASS" : "FAIL", w.c_str()); fails += !ok; }
// A 2048-byte ISO image (as settings_test.cpp), then written as raw sectors of `block` bytes with the data `offset` in.
static std::vector<uint8_t> Iso(const std::string& boot_elf, size_t sectors)
{
	std::vector<uint8_t> img(sectors * 2048, 0);
	auto le32 = [&](size_t at, uint32_t v) { for (int i = 0; i < 4; i++) img[at + i] = static_cast<uint8_t>(v >> (8 * i)); };
	uint8_t* pvd = &img[16 * 2048];
	pvd[0] = 1; std::memcpy(pvd + 1, "CD001", 5); pvd[6] = 1;
	const size_t root = 16 * 2048 + 156;
	img[root] = 34; le32(root + 2, 18); le32(root + 10, 2048); img[root + 25] = 2; img[root + 32] = 1;
	uint8_t* term = &img[17 * 2048]; term[0] = 255; std::memcpy(term + 1, "CD001", 5);
	const std::string cnf = "BOOT2 = cdrom0:\\" + boot_elf + ";1\r\nVER = 1.00\r\nVMODE = NTSC\r\n";
	const std::string name = "SYSTEM.CNF;1";
	const size_t rec = 18 * 2048;
	img[rec] = static_cast<uint8_t>(33 + name.size() + ((33 + name.size()) & 1));
	le32(rec + 2, 19); le32(rec + 10, static_cast<uint32_t>(cnf.size()));
	img[rec + 32] = static_cast<uint8_t>(name.size());
	std::memcpy(&img[rec + 33], name.data(), name.size());
	std::memcpy(&img[19 * 2048], cnf.data(), cnf.size());
	return img;
}
static void WriteRaw(const std::string& path, const std::vector<uint8_t>& iso, size_t block, size_t offset)
{
	std::vector<uint8_t> out(iso.size() / 2048 * block, 0xAB);
	for (size_t s = 0; s < iso.size() / 2048; s++)
		std::memcpy(&out[s * block + offset], &iso[s * 2048], 2048);
	std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
}
int main(int argc, char** argv)
{
	const std::string dir = argc > 1 ? argv[1] : "/tmp/rawbin-test";
	std::system(("rm -rf '" + dir + "' && mkdir -p '" + dir + "'").c_str());
	const size_t big = (17u << 20) / 2048; // over 16 MB as 2048-byte sectors
	WriteRaw(dir + "/Raw Mode2 (USA).bin", Iso("SLUS_203.70", big), 2352, 24);
	WriteRaw(dir + "/Raw Mode1 (Europe).bin", Iso("SLES_123.45", big), 2352, 16);
	WriteRaw(dir + "/Plain (Japan).img", Iso("SLPS_251.98", big), 2048, 0);
	WriteRaw(dir + "/Sub 2448 (USA).bin", Iso("SLUS_210.05", big), 2448, 24);
	WriteRaw(dir + "/Small (USA).bin", Iso("SLUS_200.01", 64), 2352, 24);
	{
		std::vector<uint8_t> audio((20u << 20), 0);
		for (size_t i = 0; i < audio.size(); i++) audio[i] = static_cast<uint8_t>(i * 2654435761u >> 13);
		std::ofstream(dir + "/Raw Mode2 (USA) (Track 2).bin", std::ios::binary).write(reinterpret_cast<const char*>(audio.data()), static_cast<std::streamsize>(audio.size()));
	}
	{
		// 2026-10-08: an .elf with an ELF header is listed (no serial); one without isn't.
		std::string elf = std::string("\x7f" "ELF") + std::string(4092, '\0');
		std::ofstream(dir + "/Homebrew.elf", std::ios::binary).write(elf.data(), static_cast<std::streamsize>(elf.size()));
		std::ofstream(dir + "/Notes.elf", std::ios::binary) << "not an executable";
	}
	const auto games = fe::ScanGames({dir});
	std::string seen;
	for (const auto& g : games) seen += g.file + "=" + fe::ReadSerial(g.path) + "; ";
	std::printf("%s\n", seen.c_str());
	Check(seen.find("Raw Mode2 (USA).bin=SLUS-20370") != std::string::npos, "a mode 2 raw .bin (data 24 in) lists with its serial");
	Check(seen.find("Raw Mode1 (Europe).bin=SLES-12345") != std::string::npos, "a mode 1 raw .bin (data 16 in)");
	Check(seen.find("Plain (Japan).img=SLPS-25198") != std::string::npos, "a plain .img");
	Check(seen.find("Sub 2448 (USA).bin=SLUS-21005") != std::string::npos, "a .bin with subcode (2448)");
	Check(seen.find("Small") == std::string::npos, "a .bin under 16 MB isn't listed");
	Check(seen.find("Track 2") == std::string::npos, "an audio track isn't listed");
	Check(seen.find("Homebrew.elf=;") != std::string::npos, "an ELF is listed, without a serial");
	Check(seen.find("Notes.elf") == std::string::npos, "a file named .elf that isn't one isn't listed");
	std::printf(fails ? "%d FAILED\n" : "all passed\n", fails);
	return fails ? 1 : 0;
}
