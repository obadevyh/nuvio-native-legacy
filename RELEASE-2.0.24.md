Fixes a memory safeguard from 2.0.23: thumbnail decoders could keep their memory while the player was buffering or backing off.

- The second thumbnail decoder now stops as soon as memory runs low, playback struggles or CPU thumbnails are switched off, even during buffering. It used to wait until buffering or a 20-second cooldown had ended.
- The memory floor now also applies during buffering, so the first decoder can be released too.

Tested on one Shield with memory pressure and playback trouble simulated: the decoders closed within a few seconds, about 60-75 MB came back per decoder, thumbnails restarted once memory recovered, and there was no rebuffering. Thumbnail speed and quality are unchanged. Weaker boxes have not been measured.
