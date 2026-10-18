#pragma once

// The daemon's embedded browser: a headless CEF that plays a web page and
// hands us the page's decoded audio (spec 3.1.1 and 3.1.2).
//
//   CefHost::start()          CEF's own UI thread        CEF's audio thread
//   ----------------          -------------------        ------------------
//   writes the Widevine   ->  creates an invisible   ->  OnAudioStreamPacket
//   hint file, starts CEF     browser on the page        -> AudioPath::write_planar
//                             runs play/pause/stop       -> ring buffer -> PipeWire
//
// This header has no CEF types in it, so main_daemon.cpp builds the same with
// or without CEF. With the AM_WITH_CEF CMake option off (the default, and what
// CI builds), cef_host_off.cpp stands in and start() fails with a message
// saying how to turn it on. With it on, cef_host.cpp is the real thing.
//
// For now the only page is a built-in one that plays Google's public
// Widevine test film (test_film_page.hpp). Apple Music comes with issue #4.
//
// Call everything here from one thread, the daemon's reactor thread. CEF runs
// on threads of its own (CefSettings::multi_threaded_message_loop), so the
// reactor keeps running as before.

#include "common/telemetry_shm.hpp"
#include "daemon/audio_engine/audio_path.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <string>

namespace am::daemon {

struct CefHostOptions {
    // The sample rate the sink plays at. CEF resamples the page's audio to it,
    // so the ring buffer only ever holds audio at the sink's rate.
    std::uint32_t sample_rate_hz = 48000;
    // Chromium's sandbox for the page and the Widevine CDM. On by default
    // (spec 3.1.1); --no-sandbox turns it off for machines that can't run it.
    bool sandbox = true;
    // The installed Widevine CDM folder. Empty: look in the usual places
    // (am::pal::find_widevine_cdm()).
    std::filesystem::path widevine_dir;
    // Play the same film without encryption (--clear-film): no Widevine and
    // no license server, so it plays even where licensing doesn't work.
    bool clear_film = false;
};

class CefHost {
  public:
    // Starts CEF and opens the test film page. The page's audio goes into
    // `audio` through write_planar(), so nothing else may write to `audio`'s
    // ring buffer while the host runs, and `audio` has to outlive the host.
    // On failure the string says what went wrong, for the user.
    static std::expected<CefHost, std::string> start(const CefHostOptions& options,
                                                     audio::AudioPath& audio);

    CefHost(CefHost&&) noexcept;
    CefHost& operator=(CefHost&&) noexcept;
    CefHost(const CefHost&) = delete;
    CefHost& operator=(const CefHost&) = delete;

    // Closes the page and shuts CEF down. Blocks until the browser has closed
    // (a few hundred milliseconds), and must run before `audio` goes away.
    ~CefHost();

    // Plays, pauses or stops the page, to match amctl and the media keys.
    // Cheap to call with the state it already has.
    void set_state(common::PlaybackState state);

  private:
    struct Impl;
    explicit CefHost(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace am::daemon
