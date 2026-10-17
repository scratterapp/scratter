# audio_engine

Platform-neutral audio processing in `scratterd`: the DLL + PI clock-drift controller, SIMD resampler and FFT tap feeding the telemetry page. See `docs/SPECIFICATION.md` section 3.1.4.

- `drift_controller.hpp`: turns the ring buffer fill into a playback ratio (EMA + PI, clamped to ±1000 ppm).
- `polyphase_resampler.hpp`: plays the audio at that ratio with a 16-tap windowed-sinc filter.

Both are header-only and platform-neutral; `tests/test_resampler_drift.cpp` covers them. Still to come: the FFT tap. Owners: @cocoa55 @kerelosgayed
