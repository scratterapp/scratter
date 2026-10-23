# Our patches to CEF and Chromium

Applied to the Chromium checkout before building our CEF. How and when to apply
them: [`docs/learning/cef-host.md`](../../docs/learning/cef-host.md#our-patch-to-chromium-windows).

| Patch | Platforms | What it does |
| --- | --- | --- |
| `widevine-cdm-dir.patch` | Windows | Registers the installed Widevine CDM folder passed as `--widevine-cdm-dir`, for a browser running with `--disable-component-update`. Nothing is downloaded or copied, and the CDM and its checks are unchanged. |

Each patch is made with `git diff` in `chromium/src` against CEF branch 7871
(Chromium 150.0.7871.256). After moving to a new CEF branch, check that each
one still applies (`git apply --check`) and refresh it if not.
