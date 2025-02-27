/*
 * Copyright (c) 2019 Roc Streaming authors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "roc_audio/poison_writer.h"
#include "roc_audio/sample.h"

namespace roc {
namespace audio {

PoisonWriter::PoisonWriter(IFrameWriter& writer)
    : writer_(writer) {
}

void PoisonWriter::write(Frame& frame) {
    writer_.write(frame);

    const size_t frame_size = frame.num_samples();
    sample_t* frame_data = frame.samples();

    for (size_t n = 0; n < frame_size; n++) {
        frame_data[n] = SampleMax;
    }
}

} // namespace audio
} // namespace roc
