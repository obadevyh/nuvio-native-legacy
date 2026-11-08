A small safety fix: the thumbnail memory floor now also applies while the player is reconnecting, as it already does during buffering, so a thumbnail decoder can't hold its memory through a long reconnect when memory is low.

Thumbnail speed and quality are unchanged. This uses the same mechanism tested for buffering in 2.0.24; the reconnect case itself has not been separately tested. Weaker boxes have not been measured.
