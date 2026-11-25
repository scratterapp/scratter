#pragma once

// Catalog responses for the catalog parser's tests, fuzzer and benchmark
// (src/net/catalog_parser.hpp, #105): the saved samples in
// tests/data/catalog/, and a playlist of any length made in code.

#include "net/http_client.hpp" // kBodyPadding

#include <cstddef>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>

namespace am::test {

// A body the way HttpResponse hands it over: the bytes, then kBodyPadding
// zero bytes that aren't part of it.
class PaddedBody {
  public:
    explicit PaddedBody(std::string_view json) : bytes_(json), size_(json.size()) {
        bytes_.append(net::kBodyPadding, '\0');
    }

    [[nodiscard]] std::span<const std::byte> span() const noexcept {
        return std::as_bytes(std::span(bytes_.data(), size_));
    }

  private:
    std::string bytes_;
    std::size_t size_;
};

// tests/data/catalog/<name>, or an empty string if it can't be read.
// AM_CATALOG_SAMPLES is set by tests/CMakeLists.txt.
inline std::string read_sample(std::string_view name) {
    const std::string path = std::string(AM_CATALOG_SAMPLES) + "/" + std::string(name);
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

// A catalog playlist with `tracks` songs, each with the attributes a real
// response has (most of which the parser skips), for the 500-track gate.
// Song i has id 1700000000 + i, title "Song <i>", track number i % 20 + 1,
// and lasts 180000 + i milliseconds.
inline std::string make_playlist_json(std::size_t tracks) {
    std::string json =
        R"({"data":[{"id":"pl.0123456789abcdef0123456789abcdef","type":"playlists",)"
        R"("href":"/v1/catalog/us/playlists/pl.0123456789abcdef0123456789abcdef",)"
        R"("attributes":{"artwork":{"height":1080,"width":1080,)"
        R"("url":"https://example.invalid/pl/{w}x{h}cc.jpg"},)"
        R"("curatorName":"Apple Music","description":{"standard":"A long one."},)"
        R"("name":"Five Hundred","playlistType":"editorial"},)"
        R"("relationships":{"tracks":{"href":"/v1/catalog/us/playlists/pl.x/tracks",)"
        R"("data":[)";
    // Appends each piece in turn (a + b + c would make a string for each +).
    const auto add = [&json](const auto&... pieces) { (json.append(pieces), ...); };
    for (std::size_t i = 0; i < tracks; ++i) {
        const std::string n = std::to_string(i);
        const std::string id = std::to_string(1700000000 + i);
        if (i > 0) {
            json += ',';
        }
        add(R"({"id":")", id, R"(","type":"songs","href":"/v1/catalog/us/songs/)", id,
            R"(","attributes":{"albumName":"Album )", n, R"(","artistName":"Artist )", n,
            R"(","artwork":{"bgColor":"1d1f21","height":3000,"textColor1":"f2f2f2",)"
            R"("textColor2":"d9d9d9","textColor3":"c7c8c8","textColor4":"b2b3b3",)"
            R"("url":"https://is1-ssl.mzstatic.com/image/thumb/Music/v4/00/00/)",
            n,
            R"(/source/{w}x{h}bb.jpg","width":3000},)"
            R"("composerName":"Composer )",
            n, R"(","discNumber":1,"durationInMillis":)", std::to_string(180000 + i),
            R"(,"genreNames":["Pop","Music"],"hasLyrics":true,"hasTimeSyncedLyrics":true,)"
            R"("isAppleDigitalMaster":true,"isMasteredForItunes":true,"audioLocale":"en-US",)"
            R"("audioTraits":["atmos","lossless","lossy-stereo","spatial"],)"
            R"("isrc":"USXXX26)",
            id, R"(","name":"Song )", n, R"(","playParams":{"id":")", id,
            R"(","kind":"song"},"previews":[{"url":"https://audio-ssl.itunes.apple.com/)"
            R"(itunes-assets/AudioPreview/)",
            id, R"(.m4a"}],"releaseDate":"2026-01-01","trackNumber":)", std::to_string(i % 20 + 1),
            R"(,"url":"https://music.apple.com/us/album/song/)", id, R"(?i=)", id, R"("}})");
    }
    json += R"(]}}}]})";
    return json;
}

} // namespace am::test
