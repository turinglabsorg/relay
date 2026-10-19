# Apple RFB 003.889 in Relay

When a server greets with `RFB 003.889`, Relay speaks macOS Screen Sharing's
own dialect in Standard mode instead of plain RFB 3.8 through LibVNCClient.
Any failure before authentication falls back to the LibVNC path.

Why: in Standard mode on a physical display, Apple's daemon answers bare
incremental `FramebufferUpdateRequest`s seconds late by design. A session
that arms `AutoFrameBufferUpdate` (client message `0x09`, version 1,
interval 0, whole-desktop rectangle) gets each changed rectangle pushed as
it is captured, and the daemon paces itself by how fast the socket drains.

Implemented, in cleartext form (Apple's viewer's default without its
record-layer preference):

- Greeting `RFB 003.889`, security type 30: Diffie-Hellman (server
  generator, key length, prime, public key), credentials as a 128-byte
  block (user at 0, password at 64) AES-128-ECB encrypted under
  `MD5(shared secret)`, then the client's public key.
- `ClientInit` `0x81`, extended ServerInit (22-byte name prefix).
- `ViewerInfo` (`0x21`, the measured 66-byte numeric form), `SetMode`
  control, `SetPixelFormat`, `SetEncodings` (adaptive or ZRLE first,
  cursor position, display info/layout, vendor keysyms, keyboard
  source, desktop size, last rect, raw last).
- Framebuffer updates, in the order Relay asks for them. Qualità asks
  for Apple's adaptive DCT codec (`0x3f3` / 1011) first, then ZRLE.
  Fluido asks for ZRLE first, then Apple's 16-bit codec (`0x3ea`,
  big-endian RGB555 on the shared zlib stream). Both then list
  CopyRect, flat zlib and raw last. ZRLE has its own zlib stream.
  CopyRect moves a region inside the framebuffer. A bad adaptive
  rectangle is length-prefixed, so it cannot desynchronise the session.
  Display layouts (`0x451`) resize the framebuffer, report
  the server-side scale and drop the arming, so every layout is
  answered with a re-arm and a repaint request.
- Fluido asks for server-side scaling (`SetServerScaling`, `0x08`)
  toward a 1440 px wide framebuffer; the Mac's own display is untouched
  and pointer coordinates are divided by the reported scale on input.
- Input dialect: the pointer mask is read as CGMouseButton numbers
  (right and middle swapped against RFB), Alt keysyms are sent as Meta
  (Option), and input is written immediately, never queued behind a
  framebuffer read.

Formats were measured by Andrew Vasilyev's remotex project and its
`docs/apple-vnc-889.md` (MIT); this implementation follows that document
and was verified against a live Mac.
