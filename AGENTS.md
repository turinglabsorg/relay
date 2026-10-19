# Relay

Linux Qt Quick VNC client for one Mac, over Tailscale or a trusted LAN.
The interface is Italian. Code, comments, and docs are English.

## Connections

- Ordinary RFB 3.8 goes through LibVNCClient.
- A greeting of `RFB 003.889` with a username uses the custom Apple Screen Sharing client in `src/session.cpp`. The wire format is in `docs/apple-889.md`.
- That path is cleartext Standard mode. It does not speak the encrypted HEVC record layer. A failure before authentication falls back to LibVNCClient.

## Latency

The Mac stalls when this client asks for raw frames. `SetEncodings` must list a codec the daemon actually sends, with raw last.

- Qualità (`lowBandwidth` false): adaptive DCT `0x3f3`, then ZRLE, CopyRect, zlib, raw. The decoder is `src/apple_mvs.cpp`. A bad adaptive rectangle is length-prefixed and must not drop the session.
- Fluido (`lowBandwidth` true, the default): ZRLE, then `0x3ea` big-endian RGB555 on the shared zlib stream, CopyRect, zlib, raw. Do not advertise Tight or JPEG; macOS ignores them from this client. Do not advertise `0x3e8` or `0x3e9`.
- `0x3ea` is not little-endian RGB565. Fluido still negotiates a 16-bit pixel format.
- ZRLE has its own zlib stream. Flat zlib and `0x3ea` share one.
- Do not treat a resolution change or `SetServerScaling` as the latency fix. Scaling is sent only in Fluido, and only when the framebuffer is wider than 1440.

## Checks

`make test` runs the VNC peer, UI captures, and keyring tests. `build/relay --self-test` covers input mapping plus the ZRLE and adaptive decoders. A behavior change needs those, and a live Mac session when the Apple path changed, before pushing.

## UI

Quality captions live in `ui/Main.qml`. Qualità means the adaptive codec. Fluido means less data and 16-bit color. Either choice applies on the next connection.
