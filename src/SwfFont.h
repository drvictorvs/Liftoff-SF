#pragma once
// Reads a font from the game's own interface files (fonts_<language>.swf), so the overlay can use
// the menu typeface without shipping it, and picks up font replacers the same way the game does.
//
// The files are found the way the game finds them: a loose file under Data\Interface first, then
// mod BA2 archives, then the vanilla "Starfield - Interface.ba2". fontconfig_<language>.txt maps
// the UI's font names (like "$MAIN_Font_Bold") to the fonts inside the SWF.
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace SwfFont
{
	// One outline command, in font units (the em square is 20480 units; y grows downwards and
	// the baseline is y = 0, so letters sit at negative y).
	struct Command
	{
		enum Type : std::uint8_t { kMove, kLine, kQuad } type;
		float x, y;    // end point
		float cx, cy;  // control point (kQuad only)
	};

	struct Glyph
	{
		std::vector<Command> commands;
		float                advance = 0.0f;  // font units
	};

	struct Font
	{
		static constexpr float kEm = 20480.0f;

		std::string                           name;
		std::string                           source;  // where it was read from, for the log
		float                                 ascent = 0.0f;
		float                                 descent = 0.0f;
		float                                 capHeight = 0.0f;  // top of 'H', for centring capitals
		std::vector<Glyph>                    glyphs;
		std::unordered_map<char32_t, std::size_t> index;  // character -> glyph

		const Glyph* Find(char32_t a_char) const
		{
			const auto it = index.find(a_char);
			return it == index.end() ? nullptr : &glyphs[it->second];
		}
	};

	// a_wanted: a fontconfig name such as "$MAIN_Font_Bold", or a font name inside the SWF such as
	// "NB Architekt". a_language: "en", "de", ... (falls back to "en" if that language is missing).
	// Returns false and sets a_error when the font can't be read.
	bool Load(const std::filesystem::path& a_dataDir, const std::string& a_language, const std::string& a_wanted,
		Font& a_font, std::string& a_error);
}
