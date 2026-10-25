Android sharing update 2.0.19

- Improve seek thumbnail support for compatible MP4 and Dolby Vision files, including indexed CPU fallback and HTTP redirects.
- Use the retained thumbnail transport, bounded lookahead and playback-aware pacing defaults without requiring ADB flags.
- Share a fair four-active-exchange HTTP budget between progressive playback and thumbnail reads, with playback buffering gates and bounded timeout recovery.
- Allow an existing hardware thumbnail decoder to refine the 10-second grid at the normal memory floor; preserve separate decoder-creation safeguards.
- Fix thumbnail source initialization after a failed first request.
- Replace the misleading failure message that claimed a smaller copy was required.

The rejected player read-ahead, ready-first and independent-lane pipeline experiments and shell replay/benchmark controls are excluded. No claim of full 10-second coverage in 60 seconds; source, bitrate, network and device memory still affect generation. Unsupported Dolby Vision profile 5 remains unsupported by the direct thumbnail path.

Android sharing release only. Existing settings and cached thumbnails are preserved. Installs over previous sharing builds with the same signing key.
