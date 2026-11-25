// Apple Music catalog responses into the arena (catalog_parser.hpp, #105).
//
// Apple's API speaks JSON:API: every response has `data`, an array of
// resources, and each resource has an `id`, a `type`, its `attributes` and
// its `relationships` to other resources (an album's tracks, say), which are
// resources again:
//
//   {"data": [{"id": "1440857781", "type": "albums",
//              "attributes": {"name": "...", "artistName": "...", ...},
//              "relationships": {"tracks": {"data": [{"id": ..., "type": "songs",
//                                                     "attributes": {...}}, ...],
//                                           "next":
//                                           "/v1/catalog/us/albums/.../tracks?offset=300"}}}]}
//
// simdjson's On-Demand API reads the body front to back without building a
// tree first: each object below is walked field by field, the fields we want
// are read and copied into the arena, and everything else is skipped over.
// Field order doesn't matter. A value can only be read once, and only while
// the walk is at it, which is why the helpers take an ondemand::value and use
// it up.

#include "net/catalog_parser.hpp"

#include "net/http_client.hpp" // kBodyPadding

#include <simdjson.h>

#include <charconv>
#include <limits>
#include <new>
#include <system_error>
#include <utility>

namespace am::net {

namespace {

namespace od = simdjson::ondemand;
using common::LinearArena;

// HttpResponse::body is followed by kBodyPadding zero bytes; simdjson needs
// at least SIMDJSON_PADDING of them.
static_assert(kBodyPadding >= simdjson::SIMDJSON_PADDING);

using Status = std::expected<void, ParseError>;

[[nodiscard]] ParseError from_simdjson(simdjson::error_code error) noexcept {
    switch (error) {
    case simdjson::UTF8_ERROR:
        return ParseError::NotUtf8;
    case simdjson::INCORRECT_TYPE:
    case simdjson::NUMBER_OUT_OF_RANGE:
    case simdjson::BIGINT_ERROR:
        return ParseError::WrongType;
    case simdjson::CAPACITY:
        return ParseError::TooLarge;
    case simdjson::MEMALLOC:
        return ParseError::OutOfMemory;
    default:
        return ParseError::NotJson;
    }
}

[[nodiscard]] std::unexpected<ParseError> fail(simdjson::error_code error) noexcept {
    return std::unexpected(from_simdjson(error));
}

// True for null, which Apple may send for a field it has no value for. We
// treat it the same as a missing field. A value that's no JSON type at all
// ('1', say) is an error here, not a wrong type later.
[[nodiscard]] std::expected<bool, ParseError> is_null(od::value& value) noexcept {
    od::json_type type{};
    if (const auto error = value.type().get(type)) {
        return fail(error);
    }
    if (type == od::json_type::unknown) {
        return std::unexpected(ParseError::NotJson);
    }
    return type == od::json_type::null;
}

// Calls on_field(key, value) for each field of an object, in the order they
// come, and stops at the first error.
template <typename OnField> Status for_each_field(od::object& object, const OnField& on_field) {
    for (auto result : object) {
        od::field field;
        if (const auto error = std::move(result).get(field)) {
            return fail(error);
        }
        std::string_view key;
        if (const auto error = field.unescaped_key().get(key)) {
            return fail(error);
        }
        if (Status status = on_field(key, field.value()); !status) {
            return status;
        }
    }
    return {};
}

// The same for an object that's a field's value. A null is the same as an
// empty object.
template <typename OnField> Status for_each_field(od::value value, const OnField& on_field) {
    const auto null = is_null(value);
    if (!null) {
        return std::unexpected(null.error());
    }
    if (*null) {
        return {};
    }
    od::object object;
    if (const auto error = value.get_object().get(object)) {
        return fail(error);
    }
    return for_each_field(object, on_field);
}

// A string, copied into the arena.
Status read_string(od::value value, LinearArena& arena, std::string_view& out) {
    const auto null = is_null(value);
    if (!null) {
        return std::unexpected(null.error());
    }
    if (*null) {
        out = {};
        return {};
    }
    std::string_view text;
    if (const auto error = value.get_string().get(text)) {
        return fail(error);
    }
    const auto copy = arena.copy_string(text);
    if (!copy) {
        return std::unexpected(ParseError::ArenaFull);
    }
    out = *copy;
    return {};
}

// A string we only look at, not keep: it points into the parser's buffer.
Status peek_string(od::value value, std::string_view& out) {
    const auto null = is_null(value);
    if (!null) {
        return std::unexpected(null.error());
    }
    if (*null) {
        out = {};
        return {};
    }
    if (const auto error = value.get_string().get(out)) {
        return fail(error);
    }
    return {};
}

// A whole number from 0 to `max`.
Status read_number(od::value value, std::uint64_t max, std::uint64_t& out) {
    const auto null = is_null(value);
    if (!null) {
        return std::unexpected(null.error());
    }
    if (*null) {
        out = 0;
        return {};
    }
    std::uint64_t number = 0;
    if (const auto error = value.get_uint64().get(number)) {
        return fail(error);
    }
    if (number > max) {
        return std::unexpected(ParseError::WrongType);
    }
    out = number;
    return {};
}

Status read_u32(od::value value, std::uint32_t& out) {
    std::uint64_t number = 0;
    if (Status status = read_number(value, std::numeric_limits<std::uint32_t>::max(), number);
        !status) {
        return status;
    }
    out = static_cast<std::uint32_t>(number);
    return {};
}

// "durationInMillis", into microseconds.
Status read_duration(od::value value, std::uint64_t& micros) {
    constexpr std::uint64_t kMaxMillis = std::numeric_limits<std::uint64_t>::max() / 1000;
    std::uint64_t millis = 0;
    if (Status status = read_number(value, kMaxMillis, millis); !status) {
        return status;
    }
    micros = millis * 1000;
    return {};
}

// The id as a number, if it's all digits and fits (catalog ids are).
[[nodiscard]] std::uint64_t to_catalog_id(std::string_view id) noexcept {
    std::uint64_t number = 0;
    const char* const end = id.data() + id.size();
    const auto [stop, error] = std::from_chars(id.data(), end, number);
    if (error != std::errc{} || stop != end) {
        return 0;
    }
    return number;
}

// {"url": "https://.../{w}x{h}bb.jpg", "width": 3000, "height": 3000, ...}
Status read_artwork(od::value value, LinearArena& arena, Artwork& artwork) {
    return for_each_field(value, [&](std::string_view key, od::value field) -> Status {
        if (key == "url") {
            return read_string(field, arena, artwork.url_template);
        }
        if (key == "width") {
            return read_u32(field, artwork.width);
        }
        if (key == "height") {
            return read_u32(field, artwork.height);
        }
        return {};
    });
}

// Reads an array of resources into the arena with read_one(value, item),
// which returns whether to keep the item. A null array is an empty one.
template <typename T, typename ReadOne>
Status read_array(od::value value, LinearArena& arena, std::span<const T>& out,
                  const ReadOne& read_one) {
    out = {};
    const auto null = is_null(value);
    if (!null) {
        return std::unexpected(null.error());
    }
    if (*null) {
        return {};
    }
    od::array array;
    if (const auto error = value.get_array().get(array)) {
        return fail(error);
    }
    // On-Demand reads front to back, so it can't know how many items there
    // are until it's been through them once. count_elements() does that pass
    // over the array (skipping, not reading) and rewinds, so the items can go
    // into one array in the arena, side by side.
    std::size_t count = 0;
    if (const auto error = array.count_elements().get(count)) {
        return fail(error);
    }
    if (count == 0) {
        return {};
    }
    const auto items = arena.make_array<T>(count);
    if (!items) {
        return std::unexpected(ParseError::ArenaFull);
    }
    std::size_t kept = 0;
    for (auto result : array) {
        od::value element;
        if (const auto error = std::move(result).get(element)) {
            return fail(error);
        }
        // count_elements() and this loop see the same array, so this can't
        // happen; but a write past the end would land in the arena's own
        // block, where ASan can't see it, so check anyway.
        if (kept == items->size()) {
            return std::unexpected(ParseError::NotJson);
        }
        T& item = (*items)[kept];
        const auto keep = read_one(element, item);
        if (!keep) {
            return std::unexpected(keep.error());
        }
        if (*keep) {
            ++kept;
        } else {
            item = T{};
        }
    }
    out = items->first(kept);
    return {};
}

Status read_song_attributes(od::value value, LinearArena& arena, Song& song) {
    return for_each_field(value, [&](std::string_view key, od::value field) -> Status {
        if (key == "name") {
            return read_string(field, arena, song.title);
        }
        if (key == "artistName") {
            return read_string(field, arena, song.artist);
        }
        if (key == "albumName") {
            return read_string(field, arena, song.album);
        }
        if (key == "durationInMillis") {
            return read_duration(field, song.duration_micros);
        }
        if (key == "trackNumber") {
            return read_u32(field, song.track_number);
        }
        if (key == "discNumber") {
            return read_u32(field, song.disc_number);
        }
        if (key == "artwork") {
            return read_artwork(field, arena, song.artwork);
        }
        return {};
    });
}

// One resource from a list of tracks: kept if it's a song (or says nothing
// about its type), left out if it's a music video or anything else.
std::expected<bool, ParseError> read_song(od::value value, LinearArena& arena, Song& song) {
    bool is_song = true;
    Status status = for_each_field(value, [&](std::string_view key, od::value field) -> Status {
        if (key == "id") {
            return read_string(field, arena, song.id);
        }
        if (key == "type") {
            std::string_view type;
            Status peeked = peek_string(field, type);
            is_song = type.empty() || type == "songs" || type == "library-songs";
            return peeked;
        }
        if (key == "attributes") {
            return read_song_attributes(field, arena, song);
        }
        return {};
    });
    if (!status) {
        return std::unexpected(status.error());
    }
    if (song.id.empty()) {
        return std::unexpected(ParseError::NoResource);
    }
    song.catalog_id = to_catalog_id(song.id);
    return is_song;
}

// "relationships": {"tracks": {"data": [...], "next": "..."}, ...}
Status read_tracks(od::value value, LinearArena& arena, std::span<const Song>& tracks,
                   std::string_view& next) {
    return for_each_field(value, [&](std::string_view key, od::value relationship) -> Status {
        if (key != "tracks") {
            return {};
        }
        return for_each_field(
            relationship, [&](std::string_view field_key, od::value field) -> Status {
                if (field_key == "data") {
                    return read_array<Song>(field, arena, tracks, [&](od::value v, Song& song) {
                        return read_song(v, arena, song);
                    });
                }
                if (field_key == "next") {
                    return read_string(field, arena, next);
                }
                return {};
            });
    });
}

std::expected<bool, ParseError> read_album(od::value value, LinearArena& arena, Album& album) {
    Status status = for_each_field(value, [&](std::string_view key, od::value field) -> Status {
        if (key == "id") {
            return read_string(field, arena, album.id);
        }
        if (key == "attributes") {
            return for_each_field(field, [&](std::string_view attribute, od::value v) -> Status {
                if (attribute == "name") {
                    return read_string(v, arena, album.title);
                }
                if (attribute == "artistName") {
                    return read_string(v, arena, album.artist);
                }
                if (attribute == "releaseDate") {
                    return read_string(v, arena, album.release_date);
                }
                if (attribute == "artwork") {
                    return read_artwork(v, arena, album.artwork);
                }
                return {};
            });
        }
        if (key == "relationships") {
            return read_tracks(field, arena, album.tracks, album.tracks_next);
        }
        return {};
    });
    if (!status) {
        return std::unexpected(status.error());
    }
    if (album.id.empty()) {
        return std::unexpected(ParseError::NoResource);
    }
    album.catalog_id = to_catalog_id(album.id);
    return true;
}

// "description": {"standard": "...", "short": "..."}: the standard one if
// there is one.
Status read_description(od::value value, LinearArena& arena, std::string_view& description) {
    std::string_view standard;
    std::string_view brief;
    Status status = for_each_field(value, [&](std::string_view key, od::value field) -> Status {
        if (key == "standard") {
            return read_string(field, arena, standard);
        }
        if (key == "short") {
            return read_string(field, arena, brief);
        }
        return {};
    });
    description = standard.empty() ? brief : standard;
    return status;
}

std::expected<bool, ParseError> read_playlist(od::value value, LinearArena& arena,
                                              Playlist& playlist) {
    Status status = for_each_field(value, [&](std::string_view key, od::value field) -> Status {
        if (key == "id") {
            return read_string(field, arena, playlist.id);
        }
        if (key == "attributes") {
            return for_each_field(field, [&](std::string_view attribute, od::value v) -> Status {
                if (attribute == "name") {
                    return read_string(v, arena, playlist.name);
                }
                if (attribute == "curatorName") {
                    return read_string(v, arena, playlist.curator);
                }
                if (attribute == "description") {
                    return read_description(v, arena, playlist.description);
                }
                if (attribute == "artwork") {
                    return read_artwork(v, arena, playlist.artwork);
                }
                return {};
            });
        }
        if (key == "relationships") {
            return read_tracks(field, arena, playlist.tracks, playlist.tracks_next);
        }
        return {};
    });
    if (!status) {
        return std::unexpected(status.error());
    }
    if (playlist.id.empty()) {
        return std::unexpected(ParseError::NoResource);
    }
    return true;
}

// The object every response is. Something that isn't JSON at all (an HTML
// error page from a proxy, say) is NotJson; JSON that isn't an object is
// WrongType.
Status root_object(od::document& doc, od::object& root) {
    od::json_type type{};
    if (const auto error = doc.type().get(type)) {
        return fail(error);
    }
    if (type == od::json_type::unknown) {
        return std::unexpected(ParseError::NotJson);
    }
    if (const auto error = doc.get_object().get(root)) {
        return fail(error);
    }
    return {};
}

// {"data": [...]}: every resource in it, read by read_one. Fails with
// NoResource when there's no `data` or it's empty.
template <typename T, typename ReadOne>
std::expected<std::span<const T>, ParseError> read_data(od::document& doc, LinearArena& arena,
                                                        const ReadOne& read_one) {
    od::object root;
    if (Status status = root_object(doc, root); !status) {
        return std::unexpected(status.error());
    }
    std::span<const T> items;
    Status status = for_each_field(root, [&](std::string_view key, od::value field) -> Status {
        if (key == "data") {
            return read_array<T>(field, arena, items, read_one);
        }
        return {};
    });
    if (!status) {
        return std::unexpected(status.error());
    }
    // Nothing may follow the document: trailing bytes mean it isn't the
    // JSON we think it is.
    if (!doc.at_end()) {
        return std::unexpected(ParseError::NotJson);
    }
    if (items.empty()) {
        return std::unexpected(ParseError::NoResource);
    }
    return items;
}

} // namespace

std::string_view to_string(ParseError error) noexcept {
    switch (error) {
    case ParseError::NotJson:
        return "not one whole JSON document";
    case ParseError::NotUtf8:
        return "not UTF-8";
    case ParseError::WrongType:
        return "a field has the wrong type";
    case ParseError::NoResource:
        return "no resource in the response";
    case ParseError::TooLarge:
        return "the response is too large";
    case ParseError::ArenaFull:
        return "the arena is full";
    case ParseError::OutOfMemory:
        return "out of memory";
    }
    return "unknown error";
}

struct CatalogParser::Impl {
    explicit Impl(std::size_t max_bytes) noexcept : parser(max_bytes), max_body_bytes(max_bytes) {}

    od::parser parser;
    std::size_t max_body_bytes;
    od::document doc; // the body being parsed; reused, so it's here and not on the stack

    // Starts a parse of `body`.
    Status open(std::span<const std::byte> body) noexcept {
        if (body.empty()) {
            return std::unexpected(ParseError::NotJson);
        }
        if (body.size() > max_body_bytes) {
            return std::unexpected(ParseError::TooLarge);
        }
        // The bytes after the body are the padding (see the header).
        const simdjson::padded_string_view json(reinterpret_cast<const char*>(body.data()),
                                                body.size(), body.size() + kBodyPadding);
        if (const auto error = parser.iterate(json).get(doc)) {
            return fail(error);
        }
        return {};
    }
};

std::expected<CatalogParser, ParseError>
CatalogParser::create(std::size_t max_body_bytes) noexcept {
    if (max_body_bytes == 0) {
        return std::unexpected(ParseError::TooLarge);
    }
    std::unique_ptr<Impl> impl(new (std::nothrow) Impl(max_body_bytes));
    if (!impl) {
        return std::unexpected(ParseError::OutOfMemory);
    }
    // All of the parser's own memory, now, so a parse never allocates.
    if (const auto error = impl->parser.allocate(max_body_bytes)) {
        return fail(error);
    }
    return CatalogParser(std::move(impl));
}

CatalogParser::CatalogParser(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
CatalogParser::CatalogParser(CatalogParser&&) noexcept = default;
CatalogParser& CatalogParser::operator=(CatalogParser&&) noexcept = default;
CatalogParser::~CatalogParser() = default;

std::expected<std::span<const Song>, ParseError>
CatalogParser::parse_songs(std::span<const std::byte> body, LinearArena& arena) noexcept {
    if (Status status = impl_->open(body); !status) {
        return std::unexpected(status.error());
    }
    return read_data<Song>(impl_->doc, arena,
                           [&](od::value v, Song& song) { return read_song(v, arena, song); });
}

std::expected<Album, ParseError> CatalogParser::parse_album(std::span<const std::byte> body,
                                                            LinearArena& arena) noexcept {
    if (Status status = impl_->open(body); !status) {
        return std::unexpected(status.error());
    }
    const auto albums = read_data<Album>(
        impl_->doc, arena, [&](od::value v, Album& album) { return read_album(v, arena, album); });
    if (!albums) {
        return std::unexpected(albums.error());
    }
    return albums->front();
}

std::expected<Playlist, ParseError> CatalogParser::parse_playlist(std::span<const std::byte> body,
                                                                  LinearArena& arena) noexcept {
    if (Status status = impl_->open(body); !status) {
        return std::unexpected(status.error());
    }
    const auto playlists =
        read_data<Playlist>(impl_->doc, arena, [&](od::value v, Playlist& playlist) {
            return read_playlist(v, arena, playlist);
        });
    if (!playlists) {
        return std::unexpected(playlists.error());
    }
    return playlists->front();
}

std::expected<SearchResults, ParseError>
CatalogParser::parse_search(std::span<const std::byte> body, LinearArena& arena) noexcept {
    if (Status status = impl_->open(body); !status) {
        return std::unexpected(status.error());
    }
    od::object root;
    if (Status status = root_object(impl_->doc, root); !status) {
        return std::unexpected(status.error());
    }
    // {"results": {"songs": {"data": [...]}, "albums": {...}, "playlists": {...}}}
    SearchResults results;
    bool found = false;
    Status status = for_each_field(root, [&](std::string_view key, od::value field) -> Status {
        if (key != "results") {
            return {};
        }
        found = true;
        return for_each_field(field, [&](std::string_view kind, od::value group) -> Status {
            // Only the three kinds we read; anything else Apple adds there is
            // skipped, whatever its type.
            if (kind != "songs" && kind != "albums" && kind != "playlists") {
                return {};
            }
            return for_each_field(group, [&](std::string_view group_key, od::value v) -> Status {
                if (group_key != "data") {
                    return {};
                }
                if (kind == "songs") {
                    return read_array<Song>(v, arena, results.songs, [&](od::value e, Song& s) {
                        return read_song(e, arena, s);
                    });
                }
                if (kind == "albums") {
                    return read_array<Album>(v, arena, results.albums, [&](od::value e, Album& a) {
                        return read_album(e, arena, a);
                    });
                }
                if (kind == "playlists") {
                    return read_array<Playlist>(
                        v, arena, results.playlists,
                        [&](od::value e, Playlist& p) { return read_playlist(e, arena, p); });
                }
                return {};
            });
        });
    });
    if (!status) {
        return std::unexpected(status.error());
    }
    if (!impl_->doc.at_end()) {
        return std::unexpected(ParseError::NotJson);
    }
    if (!found) {
        return std::unexpected(ParseError::NoResource);
    }
    return results;
}

} // namespace am::net
