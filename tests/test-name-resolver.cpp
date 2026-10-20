#include "name-resolver.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace clip_atlas;

namespace {

GameIdentityInput identity(const std::string &executable, const std::string &window_title = "",
			   const std::string &product_name = "")
{
	GameIdentityInput input;
	input.executable = executable;
	input.window_title = window_title;
	input.product_name = product_name;
	return input;
}

} // namespace

TEST_CASE("normalize_executable lowercases the file name")
{
	CHECK(normalize_executable("EldenRing.EXE") == "eldenring.exe");
	CHECK(normalize_executable("  EldenRing.exe \t") == "eldenring.exe");
	CHECK(normalize_executable("C:\\Games\\Elden Ring\\EldenRing.exe") == "eldenring.exe");
	CHECK(normalize_executable("games/elden/EldenRing.exe") == "eldenring.exe");
	// Spaces right after the separator are trimmed too.
	CHECK(normalize_executable("C:\\Games\\ EldenRing.exe") == "eldenring.exe");
}

TEST_CASE("normalize_executable gives empty for unknown, blank and directory-only input")
{
	CHECK(normalize_executable("unknown") == "");
	CHECK(normalize_executable("UNKNOWN") == "");
	CHECK(normalize_executable("  Unknown  ") == "");
	CHECK(normalize_executable("C:\\Games\\unknown") == "");
	CHECK(normalize_executable("C:\\Games\\ UNKNOWN") == "");
	CHECK(normalize_executable("C:\\Games\\") == "");
	CHECK(normalize_executable("") == "");
	CHECK(normalize_executable(" \t ") == "");
}

TEST_CASE("normalize_executable lowercases ASCII letters and leaves other bytes alone")
{
	CHECK(normalize_executable("ゲーム.EXE") == "ゲーム.exe");
	CHECK(normalize_executable("CAF\xC3\x89.EXE") == "caf\xC3\x89.exe");
}

TEST_CASE("executable_stem removes one trailing .exe in any case")
{
	CHECK(executable_stem("EldenRing.exe") == "EldenRing");
	CHECK(executable_stem("game.EXE") == "game");
	CHECK(executable_stem("C:\\Games\\Celeste\\Celeste.Exe") == "Celeste");
	CHECK(executable_stem("tool") == "tool");
	CHECK(executable_stem("gameexe") == "gameexe");
	CHECK(executable_stem("game.exe ") == "game");
	// Only one suffix goes, so the inner ".exe" stays.
	CHECK(executable_stem("game.exe.exe") == "game.exe");
	CHECK(executable_stem(".exe") == "");
}

TEST_CASE("executable_stem gives empty for unknown and blank input")
{
	CHECK(executable_stem("unknown") == "");
	CHECK(executable_stem("UNKNOWN") == "");
	CHECK(executable_stem("") == "");
	CHECK(executable_stem("  ") == "");
}

TEST_CASE("is_generic_product_name matches the engine and runtime names")
{
	CHECK(is_generic_product_name("Unreal Engine"));
	CHECK(is_generic_product_name("Unity"));
	CHECK(is_generic_product_name("Java(TM) Platform SE binary"));
	CHECK(is_generic_product_name("Java Platform SE binary"));
	CHECK(is_generic_product_name("OpenJDK Platform binary"));
	CHECK(is_generic_product_name("Microsoft® Windows® Operating System"));
	CHECK(is_generic_product_name("Electron"));
	CHECK(is_generic_product_name("Godot Engine"));
}

TEST_CASE("is_generic_product_name compares whole names, ignoring case and surrounding spaces")
{
	CHECK(is_generic_product_name("unity"));
	CHECK(is_generic_product_name("UNREAL ENGINE"));
	CHECK(is_generic_product_name("  godot engine\t"));
	CHECK_FALSE(is_generic_product_name("Unity Game"));
	CHECK_FALSE(is_generic_product_name("Unity Engine"));
	CHECK_FALSE(is_generic_product_name("Elden Ring"));
}

TEST_CASE("is_generic_product_name treats blank input as generic")
{
	CHECK(is_generic_product_name(""));
	CHECK(is_generic_product_name(" \t "));
}

TEST_CASE("clean_window_title handles the documented examples")
{
	CHECK(clean_window_title("Minecraft 1.21.4") == "Minecraft");
	CHECK(clean_window_title("Celeste - Steam") == "Celeste");
	CHECK(clean_window_title("Hades | Epic Games") == "Hades");
	CHECK(clean_window_title("Terraria: Journey's End v1.4.4.9") == "Terraria: Journey's End");
	CHECK(clean_window_title("Doom 3") == "Doom 3");
	CHECK(clean_window_title("  -- 1.0 --  ") == "");
	CHECK(clean_window_title("") == "");
}

TEST_CASE("clean_window_title removes version tokens in every accepted form")
{
	CHECK(clean_window_title("Game 1.21.4") == "Game");
	CHECK(clean_window_title("Game v1.2") == "Game");
	CHECK(clean_window_title("Game V1.2") == "Game");
	CHECK(clean_window_title("Game (1.0.3)") == "Game");
	CHECK(clean_window_title("Game (v1.2)") == "Game");
	CHECK(clean_window_title("Game [2.0.1b]") == "Game");
	CHECK(clean_window_title("Game 1.0-beta") == "Game");
	CHECK(clean_window_title("Game 1.0.0.1") == "Game");
	CHECK(clean_window_title("v1.0 Game") == "Game");
	CHECK(clean_window_title("Game 1.0 Pro") == "Game Pro");
	CHECK(clean_window_title("1.0 2.0") == "");
}

TEST_CASE("clean_window_title keeps numbers that are not versions")
{
	CHECK(clean_window_title("Battlefield 2042") == "Battlefield 2042");
	CHECK(clean_window_title("FIFA 23") == "FIFA 23");
	CHECK(clean_window_title("Doom 3") == "Doom 3");
	CHECK(clean_window_title("Game v1") == "Game v1");
	CHECK(clean_window_title("Game 1.") == "Game 1.");
	CHECK(clean_window_title("Game 1.0.x") == "Game 1.0.x");
	CHECK(clean_window_title("Game (1.0") == "Game (1.0");
	CHECK(clean_window_title("Game ((1.0))") == "Game ((1.0))");
	CHECK(clean_window_title("Game 1.0_beta") == "Game 1.0_beta");
	CHECK(clean_window_title("Game1.2") == "Game1.2");
}

TEST_CASE("clean_window_title removes a launcher suffix with each separator")
{
	CHECK(clean_window_title("Game - Steam") == "Game");
	CHECK(clean_window_title("Game | Steam") == "Game");
	CHECK(clean_window_title("Game \xE2\x80\x93 Steam") == "Game"); // en dash
	CHECK(clean_window_title("Game \xE2\x80\x94 Steam") == "Game"); // em dash
}

TEST_CASE("clean_window_title removes each launcher name")
{
	CHECK(clean_window_title("Game - Steam") == "Game");
	CHECK(clean_window_title("Game - Epic Games") == "Game");
	CHECK(clean_window_title("Game - Epic Games Store") == "Game");
	CHECK(clean_window_title("Game - GOG Galaxy") == "Game");
	CHECK(clean_window_title("Game - Xbox") == "Game");
	CHECK(clean_window_title("Game - Microsoft Store") == "Game");
	CHECK(clean_window_title("Game - Battle.net") == "Game");
	CHECK(clean_window_title("Game - EA app") == "Game");
	CHECK(clean_window_title("Game - Ubisoft Connect") == "Game");
}

TEST_CASE("clean_window_title matches launcher names ignoring case and trailing whitespace")
{
	CHECK(clean_window_title("Game - STEAM") == "Game");
	CHECK(clean_window_title("Game | epic games store") == "Game");
	CHECK(clean_window_title("Game - battle.NET") == "Game");
	CHECK(clean_window_title("Celeste - Steam   ") == "Celeste");
	CHECK(clean_window_title("Celeste - Steam\t") == "Celeste");
}

TEST_CASE("clean_window_title removes launcher suffixes repeatedly")
{
	CHECK(clean_window_title("Game - Steam - Epic Games") == "Game");
	CHECK(clean_window_title("Minecraft 1.21.4 - Steam") == "Minecraft");
}

TEST_CASE("clean_window_title only strips a launcher name at the very end")
{
	CHECK(clean_window_title("Steam - Celeste") == "Steam - Celeste");
	CHECK(clean_window_title("Steam Deck - Game") == "Steam Deck - Game");
	CHECK(clean_window_title("Steam") == "Steam");
	CHECK(clean_window_title("Celeste-Steam") == "Celeste-Steam");
	CHECK(clean_window_title(" - Steam") == "");
}

TEST_CASE("clean_window_title trims separators and whitespace from both ends")
{
	CHECK(clean_window_title("Game -") == "Game");
	CHECK(clean_window_title("| Game |") == "Game");
	CHECK(clean_window_title("::Game::") == "Game");
	CHECK(clean_window_title("-Game-") == "Game");
	CHECK(clean_window_title("  Game \t") == "Game");
	CHECK(clean_window_title("\xE2\x80\x93Game\xE2\x80\x94") == "Game");
	CHECK(clean_window_title("Half-Life: Alyx") == "Half-Life: Alyx");
}

TEST_CASE("clean_window_title returns empty when no letter, digit or non-ASCII text is left")
{
	CHECK(clean_window_title("***") == "");
	CHECK(clean_window_title("?!") == "");
	CHECK(clean_window_title("[]") == "");
	CHECK(clean_window_title("()") == "");
	CHECK(clean_window_title("\xE2\x80\x94") == "");
	CHECK(clean_window_title("1.0") == "");
}

TEST_CASE("clean_window_title keeps UTF-8 text intact")
{
	CHECK(clean_window_title("ポケモン 1.2") == "ポケモン");
	CHECK(clean_window_title("Pokémon Scarlet - Steam") == "Pokémon Scarlet");
	CHECK(clean_window_title("ポケモン \xE2\x80\x93 Steam") == "ポケモン");
	CHECK(clean_window_title("Game \xE2\x80\x93 Part 2") == "Game \xE2\x80\x93 Part 2");
	CHECK(clean_window_title("Café") == "Café");
	CHECK(clean_window_title("ー") == "ー");
	CHECK(clean_window_title("Name\xE2\x80\x94") == "Name");
}

TEST_CASE("resolve_game prefers a user rule")
{
	UserGameRules rules = {{"eldenring.exe", UserGameRule{"Elden Ring", false}}};
	BundledGameNames bundled = {{"eldenring.exe", "Bundled Name"}};

	const ResolvedGame game = resolve_game(identity("EldenRing.exe", "Title 1.0", "Product"), rules, bundled);
	CHECK(game.display_name == "Elden Ring");
	CHECK(game.source == NameSource::user_rule);
	CHECK(game.folder_key == "eldenring.exe");
	CHECK_FALSE(game.ignored);
}

TEST_CASE("resolve_game uses the bundled name before the product name")
{
	UserGameRules rules;
	BundledGameNames bundled = {{"celeste.exe", "Celeste"}};

	const ResolvedGame game =
		resolve_game(identity("Celeste.exe", "Celeste Window 1.0", "Product"), rules, bundled);
	CHECK(game.display_name == "Celeste");
	CHECK(game.source == NameSource::bundled);
}

TEST_CASE("resolve_game uses the product name before the window title")
{
	const ResolvedGame game = resolve_game(identity("Hades.exe", "Hades - Steam", "Supergiant Hades"), {}, {});
	CHECK(game.display_name == "Supergiant Hades");
	CHECK(game.source == NameSource::product_name);
}

TEST_CASE("resolve_game skips a generic product name and uses the window title")
{
	const ResolvedGame unity = resolve_game(identity("Minecraft.exe", "Minecraft 1.21.4", "Unity"), {}, {});
	CHECK(unity.display_name == "Minecraft");
	CHECK(unity.source == NameSource::window_title);

	// The blocklist check ignores surrounding spaces, so padding does not sneak a generic name through.
	const ResolvedGame padded =
		resolve_game(identity("Minecraft.exe", "Minecraft 1.21.4", "  Java(TM) Platform SE binary  "), {}, {});
	CHECK(padded.display_name == "Minecraft");
	CHECK(padded.source == NameSource::window_title);
}

TEST_CASE("resolve_game uses the cleaned window title before the executable name")
{
	const ResolvedGame game = resolve_game(identity("Celeste.exe", "Celeste - Steam"), {}, {});
	CHECK(game.display_name == "Celeste");
	CHECK(game.source == NameSource::window_title);
}

TEST_CASE("resolve_game falls back to the executable name")
{
	const ResolvedGame game = resolve_game(identity("Tool.exe", "1.0"), {}, {});
	CHECK(game.display_name == "Tool");
	CHECK(game.source == NameSource::executable);
	CHECK(game.folder_key == "tool.exe");
}

TEST_CASE("resolve_game matches rules and bundled names on the executable ignoring case")
{
	UserGameRules rules = {{"eldenring.exe", UserGameRule{"Elden Ring", false}}};
	const ResolvedGame rule = resolve_game(identity("ELDENRING.EXE"), rules, {});
	CHECK(rule.display_name == "Elden Ring");
	CHECK(rule.source == NameSource::user_rule);
	CHECK(rule.folder_key == "eldenring.exe");

	BundledGameNames bundled = {{"celeste.exe", "Celeste"}};
	const ResolvedGame bundled_game = resolve_game(identity("Celeste.EXE"), {}, bundled);
	CHECK(bundled_game.display_name == "Celeste");
	CHECK(bundled_game.source == NameSource::bundled);
}

TEST_CASE("resolve_game falls through an empty rule name but still applies ignore")
{
	UserGameRules rules = {{"spam.exe", UserGameRule{"", true}}};
	const ResolvedGame game = resolve_game(identity("Spam.exe", "Spam Window"), rules, {});
	CHECK(game.display_name == "Spam Window");
	CHECK(game.source == NameSource::window_title);
	CHECK(game.ignored);
}

TEST_CASE("resolve_game falls through an empty rule name to the bundled name")
{
	UserGameRules rules = {{"x.exe", UserGameRule{"", false}}};
	BundledGameNames bundled = {{"x.exe", "X Game"}};
	const ResolvedGame game = resolve_game(identity("X.exe"), rules, bundled);
	CHECK(game.display_name == "X Game");
	CHECK(game.source == NameSource::bundled);
	CHECK_FALSE(game.ignored);
}

TEST_CASE("resolve_game still gives an ignored game a display name")
{
	UserGameRules rules = {{"launcher.exe", UserGameRule{"Launcher", true}}};
	const ResolvedGame game = resolve_game(identity("Launcher.exe", "Launcher 1.0"), rules, {});
	CHECK(game.display_name == "Launcher");
	CHECK(game.source == NameSource::user_rule);
	CHECK(game.ignored);
	CHECK(game.folder_key == "launcher.exe");
}

TEST_CASE("resolve_game matches an ignore rule on the executable ignoring case")
{
	UserGameRules rules = {{"tool.exe", UserGameRule{"", true}}};
	const ResolvedGame game = resolve_game(identity("TOOL.EXE"), rules, {});
	CHECK(game.ignored);
	CHECK(game.folder_key == "tool.exe");
}

TEST_CASE("resolve_game never matches a map key that is not lowercase")
{
	// The maps are documented as lowercase and the resolver does not lowercase their keys.
	UserGameRules rules = {{"EldenRing.exe", UserGameRule{"Elden Ring", true}}};
	const ResolvedGame game = resolve_game(identity("EldenRing.exe"), rules, {});
	CHECK(game.display_name == "EldenRing");
	CHECK(game.source == NameSource::executable);
	CHECK_FALSE(game.ignored);
}

TEST_CASE("resolve_game names an unknown executable from its window title")
{
	const ResolvedGame game = resolve_game(identity("unknown", "Celeste - Steam"), {}, {});
	CHECK(game.folder_key == "");
	CHECK(game.display_name == "Celeste");
	CHECK(game.source == NameSource::window_title);
	CHECK_FALSE(game.ignored);

	// A blank key has no rule or bundled entry, even one stored under the empty key.
	UserGameRules rules = {{"", UserGameRule{"Empty Key", true}}};
	BundledGameNames bundled = {{"", "Bundled Empty"}};
	const ResolvedGame stored = resolve_game(identity("unknown", "Celeste - Steam"), rules, bundled);
	CHECK(stored.display_name == "Celeste");
	CHECK(stored.source == NameSource::window_title);
	CHECK_FALSE(stored.ignored);
}

TEST_CASE("clean_window_title rejoins words with single spaces")
{
	CHECK(clean_window_title("Hades  Game") == "Hades Game");
	CHECK(clean_window_title("Hades   Game 1.0  ") == "Hades Game");
}

TEST_CASE("resolve_game reports none when nothing usable is reported")
{
	const ResolvedGame blank = resolve_game(identity(""), {}, {});
	CHECK(blank.display_name == "");
	CHECK(blank.source == NameSource::none);
	CHECK(blank.folder_key == "");
	CHECK_FALSE(blank.ignored);

	const ResolvedGame unknown = resolve_game(identity("unknown", "  -- 1.0 --  ", "Unity"), {}, {});
	CHECK(unknown.display_name == "");
	CHECK(unknown.source == NameSource::none);
}

TEST_CASE("resolve_game trims each candidate and skips blank ones")
{
	UserGameRules rules = {{"x.exe", UserGameRule{"  Elden Ring  ", false}}};
	const ResolvedGame padded = resolve_game(identity("X.exe"), rules, {});
	CHECK(padded.display_name == "Elden Ring");

	UserGameRules blank_rules = {{"x.exe", UserGameRule{"   ", false}}};
	BundledGameNames blank_bundled = {{"x.exe", " \t "}};
	const ResolvedGame blank = resolve_game(identity("X.exe", "", "   "), blank_rules, blank_bundled);
	CHECK(blank.display_name == "X");
	CHECK(blank.source == NameSource::executable);

	const ResolvedGame product = resolve_game(identity("Hades.exe", "", "  Supergiant Hades  "), {}, {});
	CHECK(product.display_name == "Supergiant Hades");
	CHECK(product.source == NameSource::product_name);
}

TEST_CASE("resolve_game takes the folder key and name from a full path")
{
	const ResolvedGame game = resolve_game(identity("C:\\Games\\Celeste\\Celeste.exe"), {}, {});
	CHECK(game.folder_key == "celeste.exe");
	CHECK(game.display_name == "Celeste");
	CHECK(game.source == NameSource::executable);
}

TEST_CASE("resolve_game keeps UTF-8 window titles")
{
	const ResolvedGame game = resolve_game(identity("Pokemon.exe", "ポケモン 1.2"), {}, {});
	CHECK(game.display_name == "ポケモン");
	CHECK(game.source == NameSource::window_title);
}

TEST_CASE("resolve_game never throws, even for odd bytes")
{
	UserGameRules rules = {{"x.exe", UserGameRule{"X", false}}};
	BundledGameNames bundled = {{"x.exe", "X"}};
	for (int value = 0; value < 256; ++value) {
		const std::string odd(1, static_cast<char>(value));
		REQUIRE_NOTHROW(resolve_game(identity(odd + ".exe", odd + " 1.0 - Steam", odd), rules, bundled));
		REQUIRE_NOTHROW(clean_window_title(odd + " 1.0 " + odd));
		REQUIRE_NOTHROW(normalize_executable(odd));
		REQUIRE_NOTHROW(executable_stem(odd));
	}
}
