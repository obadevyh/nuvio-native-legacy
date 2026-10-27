Dolby Vision Profile 5 seek thumbnails now use the original video with per-frame Dolby colour conversion, fixing unsupported-title notices and green previews on the tested Shield source.

GPU colour conversion accelerates this path, with CPU fallback if GPU setup or its colour check fails. Playback remains hardware decoded as before; Profile 5 thumbnail decoding uses CPU. Thumbnail resolution and playback/network safeguards are retained.

On the tested 69.5-minute 1080p Profile 5 source, generation reached 416/417 slots in 81 seconds versus 131 seconds with CPU colour conversion. This is one observed comparison, not a guarantee for every title or device.
