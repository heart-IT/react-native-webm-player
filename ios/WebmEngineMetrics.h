// Assembles the spec's metrics snapshot from the pump and decoders.
//
// Split from WebmPlaybackEngine so the field-by-field parity reasoning (what
// each number means, and why it matches Android's) lives in one place.
#pragma once

#import <Foundation/Foundation.h>

#import "WebmPlaybackEngine.h"

@class WebmAudioDecoder;
@class WebmDemuxPump;
@class WebmVideoDecoder;

WebmPlaybackMetrics WebmCollectMetrics(WebmDemuxPump* _Nullable pump,
                                       WebmAudioDecoder* _Nullable audio,
                                       WebmVideoDecoder* _Nullable video);
