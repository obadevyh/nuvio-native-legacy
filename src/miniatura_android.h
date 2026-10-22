// SEEK-BAR THUMBNAILS MADE ON THIS DEVICE (Android, test). A third provider for
// seekr.c next to Seekr.tv and OpenSeek: Settings > "Provedor das miniaturas" >
// "No aparelho (teste)".
//
// While a title plays, one worker thread makes a 320x180 thumbnail per 10 s from
// the nearest KEYFRAME (MiniaturaMotor.kt: the device decoder, a second session
// next to the player, or Android's software decoder). When the source list has a
// smaller copy of the same title (cached, 480p-1080p, same edition words, same
// runtime within 10 %), the thumbnails come from IT, after a frame check that
// the two line up (OpenSeek's timeline check, src: backend/openseek/timeline.py:
// 20 anchors, 32x18 grey signatures, one offset/speed mapping or a rejection).
// A 720p keyframe is ~1/20 of a 4K remux's, so the thumbnails cost ~100 MB per
// film instead of GBs. Rejected or none: the playing file itself.
//
// Order: where the person scrubs, +-1 min around the playhead, one every 5 min,
// +-5 % around the playhead, one every 60 s, the rest. Pauses while the player
// rebuffers or MemAvailable is under 300 MB. Thumbnails stay in RAM (JPEG,
// ~10 KB each) for the title playing; nothing is stored or sent anywhere.
#ifndef NV_MINIATURA_ANDROID_H
#define NV_MINIATURA_ANDROID_H
#include "seekr.h"

// NULL outside the Android build.
const SeekrProvedor *miniatura_provedor(void);
#endif
