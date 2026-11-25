#include "SwfFont.h"

#include <miniz.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <optional>
#include <string_view>

namespace fs = std::filesystem;

namespace SwfFont
{
	namespace
	{
		using Bytes = std::vector<std::uint8_t>;

		std::string Lower(std::string a_text)
		{
			for (auto& c : a_text) {
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
				if (c == '\\')
					c = '/';
			}
			return a_text;
		}

		bool ReadWhole(const fs::path& a_path, Bytes& a_out)
		{
			std::ifstream in(a_path, std::ios::binary);
			if (!in)
				return false;
			in.seekg(0, std::ios::end);
			const auto size = static_cast<std::streamoff>(in.tellg());
			if (size <= 0 || size > (256ll << 20))
				return false;
			a_out.resize(static_cast<std::size_t>(size));
			in.seekg(0);
			return static_cast<bool>(in.read(reinterpret_cast<char*>(a_out.data()), size));
		}

		template <class T>
		bool Get(const Bytes& a_data, std::size_t a_offset, T& a_value)
		{
			if (a_offset + sizeof(T) > a_data.size())
				return false;
			std::memcpy(&a_value, a_data.data() + a_offset, sizeof(T));
			return true;
		}

		bool Inflate(const std::uint8_t* a_src, std::size_t a_srcSize, std::size_t a_size, Bytes& a_out)
		{
			a_out.resize(a_size);
			mz_ulong outSize = static_cast<mz_ulong>(a_size);
			return mz_uncompress(a_out.data(), &outSize, a_src, static_cast<mz_ulong>(a_srcSize)) == MZ_OK &&
			       outSize == a_size;
		}

		// ------------------------------------------------------------------ BA2 (general archives)

		// Reads one file from a "GNRL" BA2 archive (versions 1, 2 and 3; zlib or uncompressed).
		// a_entry is lower case with forward slashes, e.g. "interface/fonts_en.swf".
		bool ReadFromBa2(const fs::path& a_archive, const std::string& a_entry, Bytes& a_out)
		{
			std::ifstream in(a_archive, std::ios::binary);
			if (!in)
				return false;
			struct Header
			{
				char          magic[4];
				std::uint32_t version;
				char          type[4];
				std::uint32_t count;
				std::uint64_t nameTable;
			} header{};
			if (!in.read(reinterpret_cast<char*>(&header), sizeof(header)) || std::memcmp(header.magic, "BTDX", 4) != 0 ||
				std::memcmp(header.type, "GNRL", 4) != 0 || header.count == 0 || header.count > 1'000'000) {
				return false;
			}
			std::uint32_t compression = 0;  // version 3: 0 = zlib, 3 = LZ4 (not supported here)
			if (header.version == 3) {
				in.seekg(32);
				in.read(reinterpret_cast<char*>(&compression), sizeof(compression));
			}
			const std::streamoff records = header.version == 2 ? 32 : header.version == 3 ? 36 : 24;

			// Find the entry's index in the name table (at the end of the file).
			in.seekg(static_cast<std::streamoff>(header.nameTable));
			std::optional<std::uint32_t> found;
			std::string                  name;
			for (std::uint32_t i = 0; i < header.count && in; ++i) {
				std::uint16_t length = 0;
				if (!in.read(reinterpret_cast<char*>(&length), sizeof(length)))
					break;
				name.resize(length);
				if (!in.read(name.data(), length))
					break;
				if (!found && Lower(name) == a_entry) {
					found = i;
					break;
				}
			}
			if (!found)
				return false;

#pragma pack(push, 1)
			struct Record
			{
				std::uint32_t nameHash;
				char          extension[4];
				std::uint32_t dirHash;
				std::uint32_t flags;
				std::uint64_t offset;
				std::uint32_t packedSize;
				std::uint32_t size;
				std::uint32_t align;
			} record{};
#pragma pack(pop)
			static_assert(sizeof(Record) == 36);
			in.clear();
			in.seekg(records + static_cast<std::streamoff>(*found) * 36);
			if (!in.read(reinterpret_cast<char*>(&record), sizeof(record)) || record.size == 0 || record.size > (64u << 20))
				return false;
			const std::uint32_t stored = record.packedSize ? record.packedSize : record.size;
			Bytes               raw(stored);
			in.seekg(static_cast<std::streamoff>(record.offset));
			if (!in.read(reinterpret_cast<char*>(raw.data()), stored))
				return false;
			if (!record.packedSize) {
				a_out = std::move(raw);
				return true;
			}
			return compression == 0 && Inflate(raw.data(), raw.size(), record.size, a_out);
		}

		bool StartsWithI(std::string_view a_text, std::string_view a_prefix)
		{
			return a_text.size() >= a_prefix.size() &&
			       std::equal(a_prefix.begin(), a_prefix.end(), a_text.begin(), [](char a, char b) {
					   return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
				   });
		}

		// A file under Data, looked up like the game: loose first, then mod archives (most recently
		// changed first), then the game's own archives.
		bool ReadDataFile(const fs::path& a_dataDir, const std::string& a_relative, Bytes& a_out, std::string& a_source)
		{
			const std::string entry = Lower(a_relative);
			std::error_code   ec;

			// Loose file. Windows paths are case-insensitive, so the path can be used as written.
			const fs::path loose = a_dataDir / fs::path(a_relative).make_preferred();
			if (fs::is_regular_file(loose, ec) && ReadWhole(loose, a_out)) {
				a_source = "loose " + a_relative;
				return true;
			}

			struct Archive
			{
				fs::path        path;
				fs::file_time_type time;
				bool            vanilla;
			};
			std::vector<Archive> archives;
			for (const auto& item : fs::directory_iterator(a_dataDir, ec)) {
				if (!item.is_regular_file(ec) || Lower(item.path().extension().string()) != ".ba2")
					continue;
				const auto file = item.path().filename().string();
				archives.push_back({ item.path(), item.last_write_time(ec), StartsWithI(file, "Starfield - ") });
			}
			std::sort(archives.begin(), archives.end(), [](const Archive& a, const Archive& b) {
				if (a.vanilla != b.vanilla)
					return !a.vanilla;  // mods first
				return a.time > b.time;
			});
			for (const auto& archive : archives) {
				if (ReadFromBa2(archive.path, entry, a_out)) {
					a_source = archive.path.filename().string() + " : " + a_relative;
					return true;
				}
			}
			return false;
		}

		// ------------------------------------------------------------------ fontconfig

		// Finds `map "<token>" = "<font name>"` and `fontlib "<swf name>"` lines.
		void ParseFontConfig(const Bytes& a_data, const std::string& a_token, std::string& a_font, std::string& a_lib)
		{
			const std::string text(a_data.begin(), a_data.end());
			std::size_t       start = 0;
			while (start < text.size()) {
				auto end = text.find('\n', start);
				if (end == std::string::npos)
					end = text.size();
				const std::string_view line(text.data() + start, end - start);
				start = end + 1;

				auto quoted = [&](std::size_t a_from, std::size_t& a_next) -> std::optional<std::string> {
					const auto open = line.find('"', a_from);
					if (open == std::string_view::npos)
						return std::nullopt;
					const auto close = line.find('"', open + 1);
					if (close == std::string_view::npos)
						return std::nullopt;
					a_next = close + 1;
					return std::string(line.substr(open + 1, close - open - 1));
				};
				std::size_t next = 0;
				if (StartsWithI(line, "fontlib")) {
					if (auto lib = quoted(0, next))
						a_lib = *lib;
				} else if (StartsWithI(line, "map")) {
					auto token = quoted(0, next);
					if (token && Lower(*token) == Lower(a_token)) {
						if (auto font = quoted(next, next))
							a_font = *font;
					}
				}
			}
		}

		// ------------------------------------------------------------------ SWF

		class BitReader
		{
		public:
			BitReader(const std::uint8_t* a_data, std::size_t a_size) :
				_data(a_data), _size(a_size) {}

			bool          ok() const { return _ok; }
			std::uint32_t U(int a_bits)
			{
				std::uint32_t value = 0;
				for (int i = 0; i < a_bits; ++i) {
					if (_pos >= _size) {
						_ok = false;
						return 0;
					}
					value = (value << 1) | ((_data[_pos] >> (7 - _bit)) & 1u);
					if (++_bit == 8) {
						_bit = 0;
						++_pos;
					}
				}
				return value;
			}
			std::int32_t S(int a_bits)
			{
				const auto value = U(a_bits);
				if (a_bits > 0 && (value & (1u << (a_bits - 1))))
					return static_cast<std::int32_t>(value) - static_cast<std::int32_t>(1u << a_bits);
				return static_cast<std::int32_t>(value);
			}

		private:
			const std::uint8_t* _data;
			std::size_t         _size;
			std::size_t         _pos = 0;
			int                 _bit = 0;
			bool                _ok = true;
		};

		// A DefineFont3 glyph is a SWF shape: style changes (moves) and straight or curved edges,
		// with coordinates relative to the previous point.
		bool DecodeGlyph(const std::uint8_t* a_data, std::size_t a_size, std::vector<Command>& a_out)
		{
			BitReader bits(a_data, a_size);
			const int fillBits = static_cast<int>(bits.U(4));
			const int lineBits = static_cast<int>(bits.U(4));
			float     x = 0, y = 0;
			for (int guard = 0; guard < 100000 && bits.ok(); ++guard) {
				if (bits.U(1) == 0) {  // style change or end
					const auto flags = bits.U(5);
					if (flags == 0)
						return bits.ok();
					if (flags & 1) {
						const int n = static_cast<int>(bits.U(5));
						x = static_cast<float>(bits.S(n));
						y = static_cast<float>(bits.S(n));
						a_out.push_back({ Command::kMove, x, y, 0, 0 });
					}
					if (flags & 2)
						bits.U(fillBits);
					if (flags & 4)
						bits.U(fillBits);
					if (flags & 8)
						bits.U(lineBits);
					if (flags & 16)
						return false;  // new style arrays don't occur in font glyphs
				} else if (bits.U(1) == 1) {  // straight edge
					const int n = static_cast<int>(bits.U(4)) + 2;
					float     dx = 0, dy = 0;
					if (bits.U(1) == 1) {
						dx = static_cast<float>(bits.S(n));
						dy = static_cast<float>(bits.S(n));
					} else if (bits.U(1) == 1) {
						dy = static_cast<float>(bits.S(n));
					} else {
						dx = static_cast<float>(bits.S(n));
					}
					x += dx;
					y += dy;
					a_out.push_back({ Command::kLine, x, y, 0, 0 });
				} else {  // curved edge (quadratic)
					const int   n = static_cast<int>(bits.U(4)) + 2;
					const float cx = x + static_cast<float>(bits.S(n));
					const float cy = y + static_cast<float>(bits.S(n));
					x = cx + static_cast<float>(bits.S(n));
					y = cy + static_cast<float>(bits.S(n));
					a_out.push_back({ Command::kQuad, x, y, cx, cy });
				}
			}
			return false;
		}

		bool ParseDefineFont3(const std::uint8_t* a_body, std::size_t a_size, const std::string& a_wanted, Font& a_font)
		{
			if (a_size < 5)
				return false;
			const std::uint8_t flags = a_body[2];
			const std::uint8_t nameLength = a_body[4];
			if (5u + nameLength + 2 > a_size)
				return false;
			std::string name(reinterpret_cast<const char*>(a_body + 5), nameLength);
			while (!name.empty() && name.back() == '\0')
				name.pop_back();
			if (Lower(name) != Lower(a_wanted))
				return false;

			std::size_t   p = 5u + nameLength;
			std::uint16_t count = 0;
			std::memcpy(&count, a_body + p, 2);
			p += 2;
			const bool         wideOffsets = (flags & 0x08) != 0;
			const std::size_t  tableStart = p;
			const std::size_t  offsetSize = wideOffsets ? 4 : 2;
			if (tableStart + (count + 1u) * offsetSize > a_size)
				return false;
			auto offset = [&](std::size_t a_i) -> std::size_t {
				std::uint32_t value = 0;
				std::memcpy(&value, a_body + tableStart + a_i * offsetSize, offsetSize);
				return tableStart + value;
			};

			a_font.name = name;
			a_font.glyphs.resize(count);
			for (std::size_t i = 0; i < count; ++i) {
				const auto from = offset(i);
				const auto to = offset(i + 1);
				if (from > to || to > a_size)
					return false;
				DecodeGlyph(a_body + from, to - from, a_font.glyphs[i].commands);
			}

			std::size_t codes = offset(count);
			if (codes + count * 2u > a_size)
				return false;
			for (std::size_t i = 0; i < count; ++i) {
				std::uint16_t code = 0;
				std::memcpy(&code, a_body + codes + i * 2, 2);
				a_font.index.emplace(static_cast<char32_t>(code), i);
			}
			std::size_t q = codes + count * 2u;

			if (flags & 0x80) {  // layout: ascent, descent, leading, advances
				std::uint16_t ascent = 0, descent = 0;
				if (q + 6 + count * 2u > a_size)
					return false;
				std::memcpy(&ascent, a_body + q, 2);
				std::memcpy(&descent, a_body + q + 2, 2);
				q += 6;
				a_font.ascent = ascent;
				a_font.descent = descent;
				for (std::size_t i = 0; i < count; ++i) {
					std::int16_t advance = 0;
					std::memcpy(&advance, a_body + q + i * 2, 2);
					a_font.glyphs[i].advance = advance;
				}
			} else {
				a_font.ascent = Font::kEm * 0.8f;
				a_font.descent = Font::kEm * 0.2f;
				for (auto& glyph : a_font.glyphs)
					glyph.advance = Font::kEm * 0.6f;
			}

			// Cap height from 'H', for centring capitals in a row.
			a_font.capHeight = a_font.ascent * 0.7f;
			if (const auto* h = a_font.Find(U'H')) {
				float top = 0;
				for (const auto& c : h->commands)
					top = std::min({ top, c.y, c.type == Command::kQuad ? c.cy : 0.0f });
				if (top < 0)
					a_font.capHeight = -top;
			}
			return true;
		}

		bool ParseSwf(const Bytes& a_file, const std::string& a_wanted, Font& a_font, std::string& a_error)
		{
			if (a_file.size() < 8) {
				a_error = "SWF too short";
				return false;
			}
			std::uint32_t length = 0;
			std::memcpy(&length, a_file.data() + 4, 4);
			Bytes swf;
			if (a_file[0] == 'C' && a_file[1] == 'W' && a_file[2] == 'S') {
				Bytes body;
				if (length < 8 || length > (128u << 20) || !Inflate(a_file.data() + 8, a_file.size() - 8, length - 8, body)) {
					a_error = "could not decompress the SWF";
					return false;
				}
				swf.assign(a_file.begin(), a_file.begin() + 8);
				swf.insert(swf.end(), body.begin(), body.end());
			} else if (a_file[0] == 'F' && a_file[1] == 'W' && a_file[2] == 'S') {
				swf = a_file;
			} else {
				a_error = "unsupported SWF compression (only zlib or none)";
				return false;
			}

			// Skip the frame rectangle (5-bit field size, then 4 fields), frame rate and count.
			const int   rectBits = swf[8] >> 3;
			std::size_t pos = 8 + (5 + 4 * rectBits + 7) / 8 + 4;
			while (pos + 2 <= swf.size()) {
				std::uint16_t codeAndLength = 0;
				Get(swf, pos, codeAndLength);
				pos += 2;
				const unsigned code = codeAndLength >> 6;
				std::uint32_t  size = codeAndLength & 0x3F;
				if (size == 0x3F) {
					if (!Get(swf, pos, size))
						break;
					pos += 4;
				}
				if (pos + size > swf.size())
					break;
				if (code == 75 && ParseDefineFont3(swf.data() + pos, size, a_wanted, a_font))
					return true;
				if (code == 0)
					break;
				pos += size;
			}
			a_error = "font \"" + a_wanted + "\" not found in the SWF";
			return false;
		}
	}

	bool Load(const fs::path& a_dataDir, const std::string& a_language, const std::string& a_wanted, Font& a_font,
		std::string& a_error)
	{
		std::string language = a_language.empty() ? "en" : Lower(a_language);
		Bytes       config;
		std::string configSource;
		if (!ReadDataFile(a_dataDir, "Interface/fontconfig_" + language + ".txt", config, configSource)) {
			language = "en";
			ReadDataFile(a_dataDir, "Interface/fontconfig_en.txt", config, configSource);
		}

		// A name starting with '$' is a fontconfig name; anything else is the font's own name.
		std::string fontName = a_wanted;
		std::string lib = "fonts_" + language;
		if (!config.empty())
			ParseFontConfig(config, a_wanted, fontName, lib);
		if (!fontName.empty() && fontName[0] == '$') {
			a_error = "\"" + a_wanted + "\" isn't mapped in " + (configSource.empty() ? "fontconfig" : configSource);
			return false;
		}

		Bytes       swf;
		std::string swfSource;
		if (!ReadDataFile(a_dataDir, "Interface/" + lib + ".swf", swf, swfSource)) {
			a_error = "Interface/" + lib + ".swf not found (loose or in an archive)";
			return false;
		}
		if (!ParseSwf(swf, fontName, a_font, a_error))
			return false;
		a_font.source = swfSource;
		return true;
	}
}
