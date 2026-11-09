Faster thumbnails on titles with many dead links, and a fix for correct copies being wrongly thrown away.

- Dead links are skipped quickly instead of being retried. Same-size mirrors of a copy and 1080p backups now get tried too.
- The copy check no longer waits behind thumbnail generation, and a slow host no longer makes it reject a copy that is matching. Before this, one correct copy was rejected and 441 finished thumbnails were discarded.

Tested on an NVIDIA Shield with PenguPlay's Dune: Part Two (TorBox off): 987 of 1002 thumbnails in 101 s, down from 148 s, with no rebuffers. The host was also faster during that run, so expect less on a slow day.

Not separately tested: the check's longer 5-minute limit on a really slow host. Weaker boxes have not been measured.
