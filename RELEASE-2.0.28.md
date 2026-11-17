Fewer playback drops on TorBox, and thumbnails start sooner when a copy is a slightly different length.

- Fixed playback dropping out on TorBox while thumbnails were being made for a 4K film. Thumbnails could open up to 10 connections to the same TorBox server as the player. TorBox then stopped answering for 45 minutes or more, so the film stopped and later sources on that server failed too. TorBox, Real-Debrid and other debrid servers now share the player's 4 connections. PenguPlay's storage keeps its extra connections.
- When the smaller copy used for thumbnails is a few seconds longer or shorter than the film, thumbnails now start at the first good timing match instead of waiting for the whole check.
- Thumbnails saved from an earlier play are no longer shifted by that check.

Found and tested on a Meta Quest 3, which shares this code.

Not yet confirmed: the connection fix has been checked by unit tests, not yet in playback, and not on a TV. Thumbnails on TorBox 4K files may build more slowly, because they now share connections with the player.
