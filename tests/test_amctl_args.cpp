// amctl's argument parsing (tools/amctl/amctl_args.hpp). Talking to a real
// daemon is covered in test_daemon.cpp.

#include "amctl/amctl_args.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace {

using am::amctl::Command;
using am::amctl::Coverage;
using am::amctl::parse_arguments;
using am::common::Opcode;

auto parse(std::initializer_list<std::string_view> words) {
    const std::vector<std::string_view> args(words);
    return parse_arguments(args);
}

} // namespace

TEST_CASE("each command parses", "[amctl]") {
    CHECK(parse({"play"})->command == Command::Play);
    CHECK(parse({"pause"})->command == Command::Pause);
    CHECK(parse({"stop"})->command == Command::Stop);
    CHECK(parse({"toggle"})->command == Command::Toggle);
    CHECK(parse({"ping"})->command == Command::Ping);
    CHECK(parse({"status"})->command == Command::Status);
    CHECK(parse({"watch"})->command == Command::Watch);

    const auto volume = parse({"volume", "30"});
    REQUIRE(volume.has_value());
    CHECK(volume->command == Command::Volume);
    CHECK(volume->volume_percent == 30);
}

TEST_CASE("volume accepts 0 to 100 and nothing else", "[amctl]") {
    CHECK(parse({"volume", "0"})->volume_percent == 0);
    CHECK(parse({"volume", "100"})->volume_percent == 100);

    CHECK_FALSE(parse({"volume"}).has_value());
    CHECK_FALSE(parse({"volume", "101"}).has_value());
    CHECK_FALSE(parse({"volume", "-1"}).has_value());
    CHECK_FALSE(parse({"volume", "50%"}).has_value());
    CHECK_FALSE(parse({"volume", "loud"}).has_value());
    CHECK_FALSE(parse({"volume", ""}).has_value());
    CHECK_FALSE(parse({"volume", " 5"}).has_value());
    CHECK_FALSE(parse({"volume", "99999999999999999999"}).has_value());
}

TEST_CASE("the daemon name defaults to the real one and --name changes it", "[amctl]") {
    CHECK(parse({"play"})->name == am::common::kDaemonName);

    const auto named = parse({"--name", "test-daemon", "pause"});
    REQUIRE(named.has_value());
    CHECK(named->name == "test-daemon");
    CHECK(named->command == Command::Pause);

    CHECK_FALSE(parse({"--name"}).has_value());
    CHECK_FALSE(parse({"--name", "", "play"}).has_value());
    CHECK_FALSE(parse({"--name", "test-daemon"}).has_value()); // no command
}

TEST_CASE("bad command lines are refused with a reason", "[amctl]") {
    const auto empty = parse({});
    REQUIRE_FALSE(empty.has_value());
    CHECK(empty.error() == "no command given");

    const auto unknown = parse({"skip"});
    REQUIRE_FALSE(unknown.has_value());
    CHECK(unknown.error() == "unknown command \"skip\"");

    CHECK_FALSE(parse({"play", "now"}).has_value());
    CHECK_FALSE(parse({"volume", "30", "40"}).has_value());
    CHECK_FALSE(parse({"pause", "--name", "x"}).has_value()); // --name goes first
}

TEST_CASE("status and watch take --json and nothing else", "[amctl]") {
    CHECK_FALSE(parse({"status"})->json);
    CHECK(parse({"status", "--json"})->json);
    CHECK(parse({"watch", "--json"})->json);
    CHECK(parse({"watch", "--json"})->command == Command::Watch);

    CHECK_FALSE(parse({"status", "--yaml"}).has_value());
    CHECK_FALSE(parse({"status", "--json", "--json"}).has_value());
    CHECK_FALSE(parse({"play", "--json"}).has_value());
}

// coverage() itself is checked against a running daemon in test_daemon.cpp.
// This half only checks that the commands it names are real ones.
TEST_CASE("every request amctl claims to send has a command that parses", "[amctl]") {
    for (unsigned byte = 0; byte <= 0xFF; ++byte) {
        const auto opcode = static_cast<Opcode>(byte);
        if (!am::common::is_known_opcode(opcode)) {
            continue;
        }
        const Coverage c = am::amctl::coverage(opcode);
        if (c.kind != Coverage::Kind::Command) {
            CHECK(c.command.empty());
            continue;
        }
        INFO("opcode 0x" << std::hex << byte << ", command " << std::string(c.command));
        auto parsed = c.command == "volume" ? parse({c.command, "50"})
                      : c.command == "load" ? parse({c.command, "tone"})
                      : c.command == "seek" ? parse({c.command, "1:23"})
                                            : parse({c.command});
        CHECK(parsed.has_value());
    }
}

TEST_CASE("load takes one of the three sources", "[amctl]") {
    using am::common::TrackSource;
    CHECK(parse({"load", "tone"})->source == TrackSource::Tone);
    CHECK(parse({"load", "clear-film"})->source == TrackSource::ClearFilm);
    CHECK(parse({"load", "widevine-film"})->source == TrackSource::WidevineFilm);
    CHECK(parse({"load", "tone"})->command == Command::Load);

    CHECK_FALSE(parse({"load"}).has_value());
    CHECK_FALSE(parse({"load", "film"}).has_value());
    CHECK_FALSE(parse({"load", "Tone"}).has_value());
    CHECK_FALSE(parse({"load", "tone", "extra"}).has_value());
}

TEST_CASE("seek takes a time, or a jump forward or back", "[amctl]") {
    const auto to = parse({"seek", "1:23"});
    REQUIRE(to.has_value());
    CHECK(to->command == Command::Seek);
    CHECK(to->seek_seconds == 83);
    CHECK(to->seek_direction == 0);

    CHECK(parse({"seek", "83"})->seek_seconds == 83);
    CHECK(parse({"seek", "0"})->seek_seconds == 0);
    CHECK(parse({"seek", "1:02:03"})->seek_seconds == 3723);
    CHECK(parse({"seek", "12:00"})->seek_seconds == 720);

    const auto forward = parse({"seek", "+10"});
    REQUIRE(forward.has_value());
    CHECK(forward->seek_seconds == 10);
    CHECK(forward->seek_direction == 1);
    const auto back = parse({"seek", "-1:30"});
    REQUIRE(back.has_value());
    CHECK(back->seek_seconds == 90);
    CHECK(back->seek_direction == -1);

    CHECK_FALSE(parse({"seek"}).has_value());
    CHECK_FALSE(parse({"seek", ""}).has_value());
    CHECK_FALSE(parse({"seek", "+"}).has_value());
    CHECK_FALSE(parse({"seek", "1:5"}).has_value());  // a typo, not 1:05
    CHECK_FALSE(parse({"seek", "1:75"}).has_value()); // seconds go to 59
    CHECK_FALSE(parse({"seek", "1::23"}).has_value());
    CHECK_FALSE(parse({"seek", "1:23:"}).has_value());
    CHECK_FALSE(parse({"seek", "1:00:00:00"}).has_value());
    CHECK_FALSE(parse({"seek", "+-5"}).has_value());
    CHECK_FALSE(parse({"seek", "1.5"}).has_value());
    CHECK_FALSE(parse({"seek", "soon"}).has_value());
    CHECK_FALSE(parse({"seek", "99999999999999999999"}).has_value());
    // Fits in 64 bits, but not once it's turned into microseconds.
    CHECK_FALSE(parse({"seek", "18446744073710"}).has_value());
    CHECK_FALSE(parse({"seek", "+18446744073710"}).has_value());
    CHECK_FALSE(parse({"seek", "99999999:00:00"}).has_value());
    CHECK(parse({"seek", "8640000"})->seek_seconds == 8'640'000); // 100 days, the most
    CHECK_FALSE(parse({"seek", "8640001"}).has_value());
    CHECK_FALSE(parse({"seek", "10", "20"}).has_value());
}
