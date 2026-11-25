# Sample catalog responses

Apple Music API responses for the catalog parser's tests and fuzzer
(`src/net/catalog_parser.hpp`, #105). They're written by hand in the shape of
the examples in Apple's public API documentation, not captured from an
account, so nothing personal or licensed is in them; the names, ids and URLs
are made up.

- `album.json`: GET /v1/catalog/us/albums/{id}, with three tracks on two discs.
- `playlist.json`: GET /v1/catalog/us/playlists/{id}, two songs and a music
  video, with a `next` page of tracks.
- `songs.json`: GET /v1/catalog/us/songs?ids=..., two songs.
- `search.json`: GET /v1/catalog/us/search?types=songs,albums,playlists.
- `search-empty.json`: a search with no matches.
- `not-found.json`: what Apple sends with a 404.

The 500-track playlist for the benchmark is made in code
(`tests/catalog_samples.hpp`), not kept here.
