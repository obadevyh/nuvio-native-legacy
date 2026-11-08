Faster seek thumbnails when a 4K film has to be decoded on the CPU, and a quicker recovery when a copy is rejected.

- 4K files can now use a second decoder when no copy or hardware decoder is available. On the tested Shield, with that route forced, Dune: Part Two went from 428 seconds to 236 seconds for the full thumbnail bar, downloading the same amount, with no rebuffering. It uses about 70 MB more memory, starts only with enough free memory, and stops if memory runs low or playback struggles.
- If a smaller copy fails the picture check after thumbnails have started, the 4K file now takes over with its hardware decoder kept ready instead of falling back to the CPU.

Most plays on a fast box won't change, because a copy or the hardware decoder usually does this work. These are observed results on one device and connection, not a guarantee for every title or box; weaker boxes have not been measured.
