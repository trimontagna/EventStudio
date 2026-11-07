#pragma once

namespace evs {

// Order-of-magnitude planning of an event-based particle tracking setup (all values editable in the UI).
struct PtvSetup {
    double fov_width_mm       = 50.0;   // field of view along the sensor width
    int sensor_width_px       = 1280;
    int roi_width_px          = 1280;   // pixels actually read (ROI)
    int roi_height_px         = 720;
    double pixel_pitch_um     = 4.86;   // IMX636
    double velocity_m_s       = 20.0;   // in-plane velocity
    double particle_diam_um   = 3.0;
    double f_number           = 8.0;
    double wavelength_nm      = 532.0;
    double ppp                = 0.001;  // particles per pixel (in the ROI)
    double events_per_pixel   = 2.0;    // events per pixel crossed by a particle image (ON + OFF, bright particles)
    double pixel_latency_us   = 100.0;  // pixel response time (datasheet: < 100 us at 1000 lux, less with more light)
    double max_event_rate     = 1e9;    // readout / link capacity, events per second (check the camera datasheet)
};

struct PtvEstimate {
    double scale_um_per_px;      // object-space size of one pixel
    double magnification;        // image / object
    double velocity_px_per_us;
    double image_diam_geom_um;   // M * dp (sensor plane)
    double image_diam_diff_um;   // 2.44 f# (M + 1) lambda
    double image_diam_px;        // sqrt(geom^2 + diff^2) / pitch, at least 1 px
    double pixel_dwell_us;       // time a particle image lights one pixel
    double particles_in_roi;
    double events_per_particle_s;
    double total_event_rate;
    double rate_load;            // total_event_rate / max_event_rate
    double latency_blur_px;      // displacement during one pixel response time
    double suggested_activity_window_us;
    bool exceeds_bandwidth;      // rate_load > 1
    bool faster_than_pixel;      // dwell < latency: reduced contrast, missed events, timestamp jitter
    double max_ppp_for_bandwidth; // seeding density at which the event rate reaches max_event_rate
};

PtvEstimate estimate_ptv(const PtvSetup &setup);

} // namespace evs
