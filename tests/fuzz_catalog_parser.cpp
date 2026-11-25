// libFuzzer target for the catalog parser (src/net/catalog_parser.hpp, #105):
// the parser reads responses from the internet, so whatever bytes arrive, it
// must answer with a catalog or an error, never crash or read out of bounds.
//
// Only built with Clang and -DAM_BUILD_FUZZERS=ON (the linux-fuzz preset).
// Start it from the saved responses, so it mutates JSON that looks like
// Apple's instead of looking for braces by chance:
//
//   cmake --preset linux-fuzz
//   cmake --build --preset linux-fuzz
//   mkdir -p build/linux-fuzz/catalog-corpus
//   ./build/linux-fuzz/tests/fuzz_catalog_parser -runs=10000000 \
//       build/linux-fuzz/catalog-corpus tests/data/catalog
//
// (libFuzzer adds what it finds to the first folder, so keep the samples
// second.) ctest --preset linux-fuzz runs a short smoke version.

#include "net/catalog_parser.hpp"
#include "net/http_client.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

namespace {

using am::common::LinearArena;
using am::net::Album;
using am::net::CatalogParser;
using am::net::Playlist;
using am::net::Song;

// Big enough for most inputs, small enough that some fill it.
constexpr std::size_t kArenaBytes = 64 * 1024;

// Made once: the parser and the arena are meant to be reused.
CatalogParser& parser() {
    static CatalogParser instance = *CatalogParser::create(1024 * 1024);
    return instance;
}

LinearArena& arena() {
    static LinearArena instance = *LinearArena::create(kArenaBytes);
    return instance;
}

// Reads every byte of every string handed back, so ASan catches one that
// points anywhere it shouldn't, and checks no more came out than fits.
std::size_t touch(std::string_view text) {
    std::size_t sum = 0;
    for (const char c : text) {
        sum += static_cast<unsigned char>(c);
    }
    if (text.size() > kArenaBytes) {
        std::abort();
    }
    return sum;
}

std::size_t touch(const Song& song) {
    return touch(song.id) + touch(song.title) + touch(song.artist) + touch(song.album) +
           touch(song.artwork.url_template);
}

std::size_t touch(std::span<const Song> songs, std::size_t input_size) {
    // Each song is at least {"id":"x"}: there can't be more than the input
    // has room for.
    if (songs.size() > input_size) {
        std::abort();
    }
    std::size_t sum = 0;
    for (const Song& song : songs) {
        if (song.id.empty()) {
            std::abort(); // the parser promises every resource has an id
        }
        sum += touch(song);
    }
    return sum;
}

std::size_t touch(const Album& album, std::size_t input_size) {
    return touch(album.id) + touch(album.title) + touch(album.artist) + touch(album.release_date) +
           touch(album.artwork.url_template) + touch(album.tracks_next) +
           touch(album.tracks, input_size);
}

std::size_t touch(const Playlist& playlist, std::size_t input_size) {
    return touch(playlist.id) + touch(playlist.name) + touch(playlist.curator) +
           touch(playlist.description) + touch(playlist.artwork.url_template) +
           touch(playlist.tracks_next) + touch(playlist.tracks, input_size);
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    // A body the way HttpResponse hands it over, with the padding after it.
    std::vector<std::byte> padded(size + am::net::kBodyPadding);
    if (size > 0) {
        std::memcpy(padded.data(), data, size);
    }
    const std::span<const std::byte> body(padded.data(), size);

    // The sum goes nowhere, but volatile stops the compiler dropping the reads.
    volatile std::size_t sum = 0;

    arena().reset();
    if (const auto songs = parser().parse_songs(body, arena())) {
        sum = sum + touch(*songs, size);
    }
    arena().reset();
    if (const auto album = parser().parse_album(body, arena())) {
        sum = sum + touch(*album, size);
    }
    arena().reset();
    if (const auto playlist = parser().parse_playlist(body, arena())) {
        sum = sum + touch(*playlist, size);
    }
    arena().reset();
    if (const auto results = parser().parse_search(body, arena())) {
        sum = sum + touch(results->songs, size);
        for (const Album& album : results->albums) {
            sum = sum + touch(album, size);
        }
        for (const Playlist& playlist : results->playlists) {
            sum = sum + touch(playlist, size);
        }
    }
    return 0;
}
