More reliable thumbnails, a much shorter wait on very high-bitrate 4K, and a fix for a title's source list being filled with another title's sources.

- High-bitrate 4K no longer waits for a buffer it can't reach. A 113 Mbps file could wait minutes before its first thumbnail. If the buffer is full and steady, thumbnails can now start.
- The timing check on a smaller copy is stricter and more repeatable. It no longer counts weak matches, recovers from a bad start, and keeps thumbnails better aligned between plays.
- Leaving a film, timing out or a network failure during that check no longer counts against the add-on.
- Fixed a timing bug where switching between titles could put one film's sources in another film's list. This was seen once, and the bug did not recur while testing the fix.
- Dead links are skipped faster when looking for a copy.

Tested on an NVIDIA Shield. Se7en in 4K through TorBox: about 90-130 s, down from 288 s. Dune: Part Two in 4K through TorBox: about 70 s. No rebuffers in those runs.

Not yet confirmed: the thumbnail timing is now consistent between plays, but it has not been checked against an independent reference. The source-list fix has not been seen working live, because the bug did not recur. Weaker boxes have not been measured, and the stricter check may take longer on some titles or fall back to the slower 4K route.
