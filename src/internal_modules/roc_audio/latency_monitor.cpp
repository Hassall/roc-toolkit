/*
 * Copyright (c) 2017 Roc Streaming authors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "roc_audio/latency_monitor.h"
#include "roc_core/log.h"
#include "roc_core/panic.h"
#include "roc_core/stddefs.h"

namespace roc {
namespace audio {

namespace {

const core::nanoseconds_t LogInterval = 5 * core::Second;

double timestamp_to_ms(const SampleSpec& sample_spec,
                       packet::timestamp_diff_t timestamp) {
    return (double)sample_spec.rtp_timestamp_2_ns(timestamp) / core::Millisecond;
}

} // namespace

LatencyMonitor::LatencyMonitor(IFrameReader& frame_reader,
                               const packet::SortedQueue& incoming_queue,
                               const Depacketizer& depacketizer,
                               ResamplerReader* resampler,
                               const LatencyMonitorConfig& config,
                               core::nanoseconds_t target_latency,
                               const SampleSpec& input_sample_spec,
                               const SampleSpec& output_sample_spec)
    : frame_reader_(frame_reader)
    , incoming_queue_(incoming_queue)
    , depacketizer_(depacketizer)
    , resampler_(resampler)
    , rate_limiter_(LogInterval)
    , update_interval_((packet::timestamp_t)input_sample_spec.ns_2_rtp_timestamp(
          config.fe_update_interval))
    , update_pos_(0)
    , has_update_pos_(false)
    , freq_coeff_(0)
    , niq_latency_(0)
    , e2e_latency_(0)
    , has_niq_latency_(false)
    , has_e2e_latency_(false)
    , target_latency_(input_sample_spec.ns_2_rtp_timestamp(target_latency))
    , min_latency_(input_sample_spec.ns_2_rtp_timestamp(config.min_latency))
    , max_latency_(input_sample_spec.ns_2_rtp_timestamp(config.max_latency))
    , max_scaling_delta_(config.max_scaling_delta)
    , input_sample_spec_(input_sample_spec)
    , output_sample_spec_(output_sample_spec)
    , valid_(false) {
    roc_log(
        LogDebug,
        "latency monitor: initializing:"
        " target_latency=%lu(%.3fms) in_rate=%lu out_rate=%lu"
        " fe_enable=%d fe_profile=%s fe_interval=%.3fms",
        (unsigned long)target_latency_,
        timestamp_to_ms(input_sample_spec_, target_latency_),
        (unsigned long)input_sample_spec_.sample_rate(),
        (unsigned long)output_sample_spec_.sample_rate(), (int)config.fe_enable,
        fe_profile_to_str(config.fe_profile),
        timestamp_to_ms(input_sample_spec_, (packet::timestamp_diff_t)update_interval_));

    if (target_latency < config.min_latency || target_latency > config.max_latency
        || target_latency <= 0) {
        roc_log(LogError,
                "latency monitor: invalid config:"
                " target_latency=%ldns min_latency=%ldns max_latency=%ldns",
                (long)target_latency, (long)config.min_latency, (long)config.max_latency);
        return;
    }

    if (config.fe_enable) {
        if (config.fe_update_interval <= 0) {
            roc_log(LogError, "latency monitor: invalid config: fe_update_interval=%ld",
                    (long)config.fe_update_interval);
            return;
        }

        if (!resampler_) {
            roc_panic(
                "latency monitor: freq estimator is enabled, but resampler is null");
        }

        fe_.reset(new (fe_) FreqEstimator(config.fe_profile,
                                          (packet::timestamp_t)target_latency_));
        if (!fe_) {
            return;
        }

        if (!init_scaling_(input_sample_spec.sample_rate(),
                           output_sample_spec.sample_rate())) {
            return;
        }
    }

    valid_ = true;
}

bool LatencyMonitor::is_valid() const {
    return valid_;
}

LatencyMonitorStats LatencyMonitor::stats() const {
    roc_panic_if(!is_valid());

    LatencyMonitorStats stats;
    stats.niq_latency = input_sample_spec_.rtp_timestamp_2_ns(niq_latency_);
    stats.e2e_latency = input_sample_spec_.rtp_timestamp_2_ns(e2e_latency_);

    return stats;
}

bool LatencyMonitor::read(Frame& frame) {
    roc_panic_if(!is_valid());

    if (!frame_reader_.read(frame)) {
        return false;
    }

    update_e2e_latency_(frame.capture_timestamp());

    return true;
}

bool LatencyMonitor::update(packet::timestamp_t stream_position) {
    roc_panic_if(!is_valid());

    update_niq_latency_();

    if (has_niq_latency_) {
        if (!check_latency_(niq_latency_)) {
            return false;
        }
        if (fe_) {
            if (!update_scaling_(stream_position, niq_latency_)) {
                return false;
            }
        }
        report_latency_();
    }

    return true;
}

void LatencyMonitor::update_niq_latency_() {
    if (!depacketizer_.is_started()) {
        return;
    }

    const packet::timestamp_t niq_head = depacketizer_.next_timestamp();

    packet::PacketPtr latest_packet = incoming_queue_.latest();
    if (!latest_packet) {
        return;
    }

    const packet::timestamp_t niq_tail = latest_packet->end();

    niq_latency_ = packet::timestamp_diff(niq_tail, niq_head);
    has_niq_latency_ = true;
}

void LatencyMonitor::update_e2e_latency_(core::nanoseconds_t capture_ts) {
    if (capture_ts == 0) {
        return;
    }

    const core::nanoseconds_t current_ts = core::timestamp(core::ClockUnix);

    e2e_latency_ = input_sample_spec_.ns_2_rtp_timestamp(current_ts - capture_ts);
    has_e2e_latency_ = true;
}

bool LatencyMonitor::check_latency_(packet::timestamp_diff_t latency) const {
    if (latency < min_latency_) {
        roc_log(
            LogDebug,
            "latency monitor: latency out of bounds: latency=%ld(%.3fms) min=%ld(%.3fms)",
            (long)latency, timestamp_to_ms(input_sample_spec_, latency),
            (long)min_latency_, timestamp_to_ms(input_sample_spec_, min_latency_));
        return false;
    }

    if (latency > max_latency_) {
        roc_log(
            LogDebug,
            "latency monitor: latency out of bounds: latency=%ld(%.3fms) max=%ld(%.3fms)",
            (long)latency, timestamp_to_ms(input_sample_spec_, latency),
            (long)max_latency_, timestamp_to_ms(input_sample_spec_, max_latency_));
        return false;
    }

    return true;
}

bool LatencyMonitor::init_scaling_(size_t input_sample_rate, size_t output_sample_rate) {
    roc_panic_if_not(resampler_);

    if (input_sample_rate == 0 || output_sample_rate == 0) {
        roc_log(LogError, "latency monitor: invalid sample rates: input=%lu output=%lu",
                (unsigned long)input_sample_rate, (unsigned long)output_sample_rate);
        return false;
    }

    if (!resampler_->set_scaling(1.0f)) {
        roc_log(LogError,
                "latency monitor: scaling factor out of bounds: input=%lu output=%lu",
                (unsigned long)input_sample_rate, (unsigned long)output_sample_rate);
        return false;
    }

    return true;
}

bool LatencyMonitor::update_scaling_(packet::timestamp_t stream_position,
                                     packet::timestamp_diff_t latency) {
    roc_panic_if_not(resampler_);
    roc_panic_if_not(fe_);

    if (latency < 0) {
        latency = 0;
    }

    if (!has_update_pos_) {
        has_update_pos_ = true;
        update_pos_ = stream_position;
    }

    while (stream_position >= update_pos_) {
        fe_->update((packet::timestamp_t)latency);
        update_pos_ += update_interval_;
    }

    freq_coeff_ = fe_->freq_coeff();
    freq_coeff_ = std::min(freq_coeff_, 1.0f + max_scaling_delta_);
    freq_coeff_ = std::max(freq_coeff_, 1.0f - max_scaling_delta_);

    if (!resampler_->set_scaling(freq_coeff_)) {
        roc_log(LogDebug,
                "latency monitor: scaling factor out of bounds: fe=%.6f trim_fe=%.6f",
                (double)fe_->freq_coeff(), (double)freq_coeff_);
        return false;
    }

    return true;
}

void LatencyMonitor::report_latency_() {
    if (!rate_limiter_.allow()) {
        return;
    }

    roc_log(LogDebug,
            "latency monitor:"
            " e2e_latency=%ld(%.3fms) niq_latency=%ld(%.3fms) target_latency=%ld(%.3fms)"
            " fe=%.6f trim_fe=%.6f",
            (long)e2e_latency_, timestamp_to_ms(input_sample_spec_, e2e_latency_),
            (long)niq_latency_, timestamp_to_ms(input_sample_spec_, niq_latency_),
            (long)target_latency_, timestamp_to_ms(input_sample_spec_, target_latency_),
            (double)(fe_ ? fe_->freq_coeff() : 0), (double)freq_coeff_);
}

} // namespace audio
} // namespace roc
