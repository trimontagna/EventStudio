#include "core/ptv_planner.h"

#include <algorithm>
#include <cmath>

namespace evs {

PtvEstimate estimate_ptv(const PtvSetup &s) {
    PtvEstimate e{};
    e.scale_um_per_px    = s.fov_width_mm * 1000.0 / std::max(1, s.sensor_width_px);
    e.magnification      = s.pixel_pitch_um / e.scale_um_per_px;
    e.velocity_px_per_us = s.velocity_m_s / e.scale_um_per_px; // (m/s) / (um/px) = px/us
    e.image_diam_geom_um = e.magnification * s.particle_diam_um;
    e.image_diam_diff_um = 2.44 * s.f_number * (e.magnification + 1.0) * s.wavelength_nm * 1e-3;
    e.image_diam_px      = std::max(1.0, std::hypot(e.image_diam_geom_um, e.image_diam_diff_um) / s.pixel_pitch_um);
    e.pixel_dwell_us     = e.velocity_px_per_us > 0 ? e.image_diam_px / e.velocity_px_per_us : INFINITY;
    e.particles_in_roi   = s.ppp * static_cast<double>(s.roi_width_px) * s.roi_height_px;
    // A moving image of diameter D sweeps v * D new pixels per unit time, each producing ~events_per_pixel events.
    e.events_per_particle_s = e.velocity_px_per_us * 1e6 * e.image_diam_px * s.events_per_pixel;
    e.total_event_rate      = e.events_per_particle_s * e.particles_in_roi;
    e.rate_load             = s.max_event_rate > 0 ? e.total_event_rate / s.max_event_rate : INFINITY;
    e.latency_blur_px       = e.velocity_px_per_us * s.pixel_latency_us;
    // Events of one particle on neighbouring pixels are a pixel dwell apart, spread by the pixel response time:
    // a few dwell times, but at least the latency, and never below 10 us.
    e.suggested_activity_window_us = std::max({10.0, 4.0 * e.pixel_dwell_us, s.pixel_latency_us});
    const double per_ppp           = e.events_per_particle_s * static_cast<double>(s.roi_width_px) * s.roi_height_px;
    e.max_ppp_for_bandwidth        = per_ppp > 0 ? s.max_event_rate / per_ppp : INFINITY;
    e.exceeds_bandwidth            = e.rate_load > 1.0;
    e.faster_than_pixel            = e.pixel_dwell_us < s.pixel_latency_us;
    return e;
}

} // namespace evs
