#pragma once
// ---------------------------------------------------------------------------
// The small part of SFSE's plugin API that this mod needs.
//
// Copied (trimmed) from sfse/PluginAPI.h, which is MIT-licensed:
//   Copyright (c) 2023 Ian Patterson
//   https://github.com/ianpatt/sfse/blob/master/sfse/PluginAPI.h
//
// SFSE talks to plugins through these exact memory layouts, so field order
// and types must stay identical to the original header. Do not reorder.
// ---------------------------------------------------------------------------
#include <cstdint>

using PluginHandle = std::uint32_t;

enum : std::uint32_t
{
	kInterface_Invalid   = 0,
	kInterface_Messaging = 1,
	kInterface_Trampoline,
	kInterface_Menu,
	kInterface_Task,
};

// Passed to SFSEPlugin_Load. Gives access to the other SFSE services.
struct SFSEInterface
{
	std::uint32_t sfseVersion;
	std::uint32_t runtimeVersion;    // game version, packed (see DecodeVersion in main.cpp)
	std::uint32_t interfaceVersion;
	void* (*QueryInterface)(std::uint32_t id);
	PluginHandle (*GetPluginHandle)();
	const void* (*GetPluginInfo)(const char* name);
};

// Lets a plugin receive notifications such as "all game data-load callbacks have finished".
struct SFSEMessagingInterface
{
	struct Message
	{
		const char*   sender;
		std::uint32_t type;
		std::uint32_t dataLen;
		void*         data;
	};

	using EventCallback = void (*)(Message* msg);

	enum : std::uint32_t
	{
		kMessage_PostLoad,          // all SFSE plugins have loaded
		kMessage_PostPostLoad,
		kMessage_PostDataLoad,      // all game data (ESM/ESP files) has loaded
		kMessage_PostPostDataLoad,
		kMessage_PreSaveGame,
		kMessage_PostSaveGame,
		kMessage_PreLoadGame,
		kMessage_PostLoadGame,
	};

	std::uint32_t interfaceVersion;
	bool (*RegisterListener)(PluginHandle listener, const char* sender, EventCallback handler);
	bool (*Dispatch)(PluginHandle sender, std::uint32_t messageType, void* data, std::uint32_t dataLen, const char* receiver);
};

// Every SFSE plugin exports one of these, named SFSEPlugin_Version.
// SFSE reads it *before* running any of your code to decide whether to load you.
struct SFSEPluginVersionData
{
	enum : std::uint32_t { kVersion = 1 };

	enum : std::uint32_t
	{
		kAddressIndependence_Signatures       = 1 << 0,  // no hard-coded game addresses
		kAddressIndependence_AddressLibrary   = 1 << 1,
		kAddressIndependence_AddressLibraryV2 = 1 << 2,
	};

	enum : std::uint32_t
	{
		kStructureIndependence_NoStructs         = 1 << 0,  // never touches game data structures
		kStructureIndependence_InitialLayout     = 1 << 1,
		kStructureIndependence_1_8_86_Layout     = 1 << 2,
		kStructureIndependence_1_14_70_Layout    = 1 << 3,
	};

	std::uint32_t dataVersion;             // always kVersion
	std::uint32_t pluginVersion;           // your own version number
	char          name[256];
	char          author[256];
	std::uint32_t addressIndependence;
	std::uint32_t structureIndependence;
	std::uint32_t compatibleVersions[16];  // ignored when the two fields above say "independent"
	std::uint32_t seVersionRequired;       // 0 = any SFSE
	std::uint32_t reservedNonBreaking;     // must be 0
	std::uint32_t reservedBreaking;        // must be 0
};

// If this ever fails, the struct no longer matches what SFSE expects.
static_assert(sizeof(SFSEPluginVersionData) == 604, "SFSEPluginVersionData layout changed");
