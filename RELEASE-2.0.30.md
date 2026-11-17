Fewer TorBox playback drops, and a fix for one bad TorBox answer slowing thumbnails for a week.

- Fixed a cause of TorBox drops. If one TorBox server gave an unusual answer, Nuvio remembered it for every TorBox server for 7 days. Thumbnails then made about 5 times more requests than needed, and TorBox could stop answering your connection for about an hour, stopping the film. Now each TorBox server is checked again every session.
- Thumbnail requests to TorBox and other debrid services are now spaced out a little, so they can't arrive in bursts. Normal thumbnails aren't slowed.
- Fixed a rare crash when playback started buffering before the thumbnail system was ready.

Found and tested on a Meta Quest 3, which shares this code. After the fix, Dune in 4K through TorBox made 708 of 933 thumbnails in about 85 seconds, with no blocked connections.

Not yet confirmed: these changes have not been run on a TV. The TV build compiles, and its network tests pass.
