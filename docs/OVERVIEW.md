`scratterd` and `am-ui` form a high-performance, native desktop client for Apple Music on Linux and Windows. Instead of wrapping the entire Apple Music web application inside a heavy, persistent browser window (like Electron or WebView2), the system separates **media playback** from **the user interface** into two independent native C++ programs that work together like `tmux` or `mpd`.

## 1. What the System Does

- **Plays Protected Apple Music Streams Natively:** Authenticates with an Apple Music account, decrypts DRM-protected audio streams (up to the 256 kbps AAC-LC web ceiling), and plays them directly through the operating system’s native low-latency audio engine (PipeWire on Linux, WASAPI on Windows).
    
- **Survives Closing the Window:** Because the audio engine runs as an invisible background daemon (`scratterd`), closing the graphical window (`am-ui`) does not stop the music. It simply frees all graphical memory while playback continues in the background.
    
- **Opens and Attaches in Under 5 Milliseconds:** Launching the graphical interface (`am-ui`) does not boot a browser or reload a web page. It opens a native GPU-accelerated window that attaches to the running background daemon almost instantaneously.
    
- **Integrates with OS Media Keys & Overlays:** Hardware media keys (Play, Pause, Next, Volume) and desktop media applets (GNOME/KDE media players on Linux, the Windows 11 media flyout) talk directly to the background daemon, working seamlessly whether the UI window is open or closed.
    
- **Browses Large Libraries with Zero Lag:** Searching the Apple Music catalog, opening 1,000-song playlists, and scrubbing through real-time 120 FPS audio visualizers happen natively in C++ without JavaScript garbage-collection stutters or DOM layout delays.
    

## 2. The Core Engineering Problem (Why This Architecture Exists)

Building a native Apple Music client faces a fundamental roadblock: **Apple Music requires browser-grade DRM (Google Widevine on Linux/Windows) and JavaScript cryptographic challenges to play audio.**

Most third-party clients solve this the easy way: they bundle an entire Chromium browser (via Electron) and render the UI inside it. That approach eats 700 MB to 1.2 GB of RAM, drains laptop batteries, and introduces input lag.

**How this project solves it:** It accepts that a browser runtime is strictly necessary for DRM decryption, but refuses to use the browser for **anything else**. By shrinking the browser down to a headless, invisible decryption worker and moving networking, UI rendering, and audio output into native C++, the project gets official DRM compatibility at a fraction of the memory and CPU cost.

## 3. How It Achieves It (The 4 Core Mechanisms)

|**Capability**|**Traditional Electron Approach**|**How scratterd + am-ui Achieves It**|
|---|---|---|
|**1. Authentication**|Keeps a full browser window alive forever just to hold login cookies.|**Ephemeral Popup + OS Vault:** Opens a temporary login window once for Apple ID + 2FA, extracts the session token into the OS keyring (Linux Secret Service / Windows DPAPI), and destroys the window. All future startups are 100% invisible.|
|**2. Catalog & Search**|Fetches JSON inside JavaScript (`fetch()`) and builds heavy HTML/CSS DOM trees.|**Native C++ Networking & SIMD Parsing:** The UI fetches catalog data directly from Apple’s servers using asynchronous `libcurl`, parses the JSON in under a millisecond using CPU vector instructions (`simdjson`), and renders directly to the GPU via Dear ImGui.|
|**3. DRM & Audio Output**|Lets Chromium decrypt the audio and send it through its own generic browser audio mixer.|**In-Memory Audio Tap:** Runs an invisible, 1-FPS-throttled Chromium Embedded Framework (CEF) instance inside the daemon. As soon as Widevine decrypts a chunk of audio, `CefAudioHandler` intercepts the raw audio samples in RAM and hands them to a custom C++ audio engine via a lock-free ring buffer.|
|**4. UI-to-Engine Sync**|Sends JSON strings back and forth between browser processes many times per second.|**Split Control & Shared Memory Planes:** Button clicks (Play/Pause/Seek) travel over ultra-fast binary OS pipes (`AF_UNIX` / Named Pipes). High-speed visual data (playback progress bar, 64-band FFT audio visualizer) is written by the daemon into a shared RAM page that the UI reads 120 times a second with zero OS overhead.|

## 4. End-to-End Walkthrough: What Happens When You Play a Song

To see how the pieces fit together in practice, here is the exact lifecycle of a user interaction from cold boot to hearing audio:

### Step 1: Invisible Startup (`scratterd`)

When the user logs into their computer, the background daemon (`scratterd`) starts silently. It reads the saved Apple Music user token from the OS credential vault and initializes an off-screen, non-rendering CEF instance loaded with MusicKit JS and the Widevine L3 decryption module.

### Step 2: Instant UI Launch & Native Search (`am-ui`)

The user launches `am-ui`. A native Vulkan (Linux) or DirectX 11 (Windows) window appears in under 5 milliseconds. When the user types a search query or clicks a playlist, `am-ui` does **not** ask the browser to load a page—it fires an asynchronous HTTP/2 request directly to Apple’s REST API using `libcurl`, parses the response with `simdjson`, and draws the tracklist immediately.

### Step 3: The Binary Play Command (Control Plane)

When the user clicks a song, `am-ui` packs a compact, 32-byte binary `LoadTrack` command containing the track ID and sends it over a local Unix Domain Socket (Linux) or Named Pipe (Windows) to `scratterd`. That takes less than 25 microseconds.

### Step 4: Headless Decryption & Lock-Free Handoff (Audio Plane)

`scratterd` receives the track ID and tells its internal, invisible MusicKit JS instance to start streaming.

1. Widevine L3 negotiates the decryption key with Apple's license server and begins decoding the 256 kbps AAC stream into raw 32-bit floating-point audio frames.
    
2. Instead of playing the sound itself, Chromium fires `CefAudioHandler::OnAudioStreamPacket`, handing the raw audio frames to `scratterd`.
    
3. `scratterd` drops those frames into a **lock-free Single-Producer Single-Consumer (SPSC) ring buffer** pinned in physical RAM.
    
4. A dedicated real-time audio thread (running on PipeWire or WASAPI) pulls the audio out of the ring buffer, runs a mathematical clock-drift controller so the browser clock and sound card crystal never fall out of sync, and sends the audio to the headphones.
    

### Step 5: Zero-Copy UI Updates & Detaching (Telemetry Plane)

As the audio plays, the real-time audio thread continuously writes the current song timestamp, buffer health, and audio spectrum frequencies into a single page of **shared memory**.

- While `am-ui` is open, it reads that shared memory page at 120 FPS to animate the progress bar and spectrum visualizer smoothly without ever interrupting the audio thread.
    
- If the user closes `am-ui` to save workspace clutter, the window process exits completely. `scratterd` keeps playing the queue in the background, updating the desktop's native MPRIS / Windows media controls until the user reopens `am-ui`.