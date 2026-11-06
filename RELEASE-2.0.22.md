Fixes slow seek thumbnails on 4K titles whose smaller copies cannot be used, and finds more usable copies.

- When no copy in the source list works, Nuvio now goes back to the 4K file with its hardware decoder instead of the much slower CPU route: on the tested Shield, Dune: Part Two from PenguPlay took 162 seconds instead of 458.
- 1080p copies are now tried after the 720p ones instead of being skipped (disc remuxes still are). On PenguPlay, six 720p copies of Dune were dead or could not be seeked, and a 1080p copy made the full thumbnail bar in 148 seconds.
- Fast devices stop trying copies from a source whose copies keep failing (6 tries in the last 3 days, none usable) and go straight to the 4K file. Slower devices still try every copy, because a working copy saves them many minutes.

These are observed results on one device and connection, not a guarantee for every title or box.
