Smaller copies for thumbnails are no longer switched off by mistake.

- When several copies from an add-on failed, Nuvio could stop using that add-on's smaller copies for 3 days. Thumbnails then read the full 4K file, which is slower and puts more load on the same server as the film. Two cases no longer count as failures:
  - a copy that is a different cut of the film (another running time);
  - a copy that won't open while the film itself is buffering or failing, for example when the network or the debrid service is having trouble.
- Genuinely broken copies (dead links, the wrong format, a rejected timing check) still count, so add-ons whose copies never work are still skipped.

Found and tested on a Meta Quest 3, which shares this code.

Not yet confirmed: this change has not been run on a TV. The TV build compiles with it.
