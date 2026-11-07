#include "io/offline_processor.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <functional>
#include <unordered_map>

#include <metavision/sdk/stream/event_file_reader.h>
#include <metavision/sdk/stream/hdf5_event_file_writer.h>
#include <metavision/sdk/stream/raw_event_file_reader.h>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

#include "core/raw_file_writer.h"
#include "io/event_file_source.h"

namespace evs {

const char *to_string(ExportFormat format) {
    switch (format) {
    case ExportFormat::RawEvt2:
        return "RAW EVT 2.0";
    case ExportFormat::RawEvt21:
        return "RAW EVT 2.1";
    case ExportFormat::RawEvt3:
        return "RAW EVT 3.0";
    case ExportFormat::Hdf5:
        return "HDF5 (ECF)";
    case ExportFormat::Csv:
        return "CSV (x,y,p,t)";
    case ExportFormat::Avi:
        return "AVI video (MJPG)";
    case ExportFormat::Images:
        return "Image sequence (PNG)";
    default:
        return "?";
    }
}

const char *default_extension(ExportFormat format) {
    switch (format) {
    case ExportFormat::Hdf5:
        return ".hdf5";
    case ExportFormat::Csv:
        return ".csv";
    case ExportFormat::Avi:
        return ".avi";
    case ExportFormat::Images:
        return "";
    default:
        return ".raw";
    }
}

namespace {

using Sink = std::function<void(const EventCD *, const EventCD *)>;

class CsvSink {
public:
    bool open(const std::filesystem::path &path) {
        file_.open(path, std::ios::binary | std::ios::trunc);
        if (file_)
            file_ << "x,y,p,t\n";
        return static_cast<bool>(file_);
    }
    void write(const EventCD *b, const EventCD *e) {
        buffer_.clear();
        char tmp[64];
        for (auto it = b; it != e; ++it) {
            char *p = tmp;
            p       = std::to_chars(p, tmp + sizeof(tmp), it->x).ptr;
            *p++    = ',';
            p       = std::to_chars(p, tmp + sizeof(tmp), it->y).ptr;
            *p++    = ',';
            p       = std::to_chars(p, tmp + sizeof(tmp), it->p).ptr;
            *p++    = ',';
            p       = std::to_chars(p, tmp + sizeof(tmp), it->t).ptr;
            *p++    = '\n';
            buffer_.append(tmp, p);
        }
        file_.write(buffer_.data(), static_cast<std::streamsize>(buffer_.size()));
    }
    bool close() {
        file_.close();
        return !file_.fail();
    }

private:
    std::ofstream file_;
    std::string buffer_;
};

// Triggers of a CSV export go to "<name>_triggers.csv" (p,id,t), created on the first trigger.
class CsvTriggerSink {
public:
    explicit CsvTriggerSink(std::filesystem::path path) : path_(std::move(path)) {}
    void write(const EventExtTrigger *b, const EventExtTrigger *e) {
        if (b == e)
            return;
        if (!file_.is_open()) {
            file_.open(path_, std::ios::binary | std::ios::trunc);
            file_ << "p,id,t\n";
        }
        for (auto it = b; it != e; ++it)
            file_ << it->p << ',' << it->id << ',' << it->t << '\n';
    }
    bool close() {
        if (!file_.is_open())
            return true;
        file_.close();
        return !file_.fail();
    }

private:
    std::filesystem::path path_;
    std::ofstream file_;
};

// Renders a frame every `period` us of recording time and hands it (BGR) to `emit`.
// Frame i covers [start + i * period, start + (i + 1) * period): it is rendered 1 us before its end, once all the
// events before the end have been ingested, so consecutive frames of the window-based modes share no event.
class FrameStepper {
public:
    using Emit = std::function<void(const cv::Mat &bgr, timestamp t_begin, timestamp t_end)>;

    FrameStepper(int width, int height, const PreviewSettings &preview, timestamp period, Emit emit) :
        renderer_(width, height), period_(std::max<timestamp>(1, period)), emit_(std::move(emit)) {
        renderer_.set_settings(preview);
    }
    void set_start(timestamp start) {
        next_frame_t_ = start + period_;
        started_      = true;
    }
    void write(const EventCD *b, const EventCD *e) {
        if (b == e)
            return;
        if (!started_)
            set_start(b->t);
        while (b != e) {
            const EventCD *split = std::lower_bound(b, e, next_frame_t_,
                                                    [](const EventCD &ev, timestamp t) { return ev.t < t; });
            renderer_.ingest(b, split);
            b = split;
            if (b != e)
                emit_frame();
        }
    }
    // Emits the last (partial) frame.
    void close() {
        if (started_)
            emit_frame();
        started_ = false;
    }
    uint64_t frames() const { return frames_; }

private:
    void emit_frame() {
        renderer_.render(next_frame_t_ - 1, rgba_);
        cv::Mat rgba(renderer_.height(), renderer_.width(), CV_8UC4, rgba_.data());
        cv::cvtColor(rgba, bgr_, cv::COLOR_RGBA2BGR);
        emit_(bgr_, next_frame_t_ - period_, next_frame_t_);
        next_frame_t_ += period_;
        ++frames_;
    }

    FrameRenderer renderer_;
    timestamp period_;
    Emit emit_;
    timestamp next_frame_t_ = 0;
    bool started_           = false;
    std::vector<uint8_t> rgba_;
    cv::Mat bgr_;
    uint64_t frames_ = 0;
};

class VideoSink {
public:
    VideoSink(int width, int height, const PreviewSettings &preview, double fps) :
        fps_(fps > 0 ? fps : 30.0), size_(width, height),
        frames_(width, height, preview, static_cast<timestamp>(1e6 / fps_),
                [this](const cv::Mat &bgr, timestamp, timestamp) { writer_.write(bgr); }) {}
    bool open(const std::filesystem::path &path) {
        writer_.open(path.string(), cv::CAP_OPENCV_MJPEG, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'), fps_, size_);
        return writer_.isOpened();
    }
    // Frames are rendered every 1/fps of recording time, starting at `start`.
    void set_start(timestamp start) { frames_.set_start(start); }
    void write(const EventCD *b, const EventCD *e) { frames_.write(b, e); }
    bool close() {
        frames_.close();
        writer_.release();
        return true;
    }
    uint64_t frames() const { return frames_.frames(); }

private:
    double fps_;
    cv::Size size_;
    cv::VideoWriter writer_;
    FrameStepper frames_;
};

// frame_000000.png ... in a folder, plus frames.csv (index,file,t_begin_us,t_end_us).
class ImageSink {
public:
    ImageSink(int width, int height, const PreviewSettings &preview) :
        frames_(width, height, preview, preview.accumulation_us,
                [this](const cv::Mat &bgr, timestamp t0, timestamp t1) { write_frame(bgr, t0, t1); }) {}
    bool open(const std::filesystem::path &dir, std::string *error) {
        dir_ = dir;
        std::error_code ec;
        std::filesystem::create_directories(dir_, ec);
        if (ec) {
            *error = "Cannot create the folder " + dir_.string() + ": " + ec.message();
            return false;
        }
        index_.open(dir_ / "frames.csv", std::ios::binary | std::ios::trunc);
        if (!index_) {
            *error = "Cannot write " + (dir_ / "frames.csv").string();
            return false;
        }
        index_ << "index,file,t_begin_us,t_end_us\n";
        return true;
    }
    void set_start(timestamp start) { frames_.set_start(start); }
    void write(const EventCD *b, const EventCD *e) { frames_.write(b, e); }
    bool close(std::string *error) {
        frames_.close();
        index_.close();
        if (!error_.empty())
            *error = error_;
        else if (index_.fail())
            *error = "Error while writing " + (dir_ / "frames.csv").string();
        return error->empty();
    }
    uint64_t frames() const { return frames_.frames(); }

private:
    void write_frame(const cv::Mat &bgr, timestamp t0, timestamp t1) {
        if (!error_.empty())
            return;
        char name[32];
        std::snprintf(name, sizeof(name), "frame_%06llu.png", static_cast<unsigned long long>(frames_.frames()));
        // Low compression: the frames are mostly flat, level 1 is much faster than the default for a slightly larger file.
        if (!cv::imwrite((dir_ / name).string(), bgr, {cv::IMWRITE_PNG_COMPRESSION, 1})) {
            error_ = "Cannot write " + (dir_ / name).string();
            return;
        }
        index_ << frames_.frames() << ',' << name << ',' << t0 << ',' << t1 << '\n';
    }

    std::filesystem::path dir_;
    std::ofstream index_;
    std::string error_;
    FrameStepper frames_;
};

// "20ms", "1.5ms", "500us", "2s"
std::string compact_time(timestamp us) {
    char buf[32];
    if (us >= 1'000'000 && us % 100'000 == 0)
        std::snprintf(buf, sizeof(buf), "%gs", us / 1e6);
    else if (us >= 1000 && us % 100 == 0)
        std::snprintf(buf, sizeof(buf), "%gms", us / 1e3);
    else
        std::snprintf(buf, sizeof(buf), "%lldus", static_cast<long long>(us));
    return buf;
}

// "50M", "2.5M", "500k", "800"
std::string compact_count(double n) {
    char buf[32];
    if (n >= 1e6)
        std::snprintf(buf, sizeof(buf), "%gM", n / 1e6);
    else if (n >= 1e3)
        std::snprintf(buf, sizeof(buf), "%gk", n / 1e3);
    else
        std::snprintf(buf, sizeof(buf), "%g", n);
    return buf;
}

} // namespace

std::string image_export_tag(const ExportJob &job, bool masked) {
    std::vector<std::string> parts;
    const PreviewSettings &p = job.preview;
    parts.push_back("dt-" + compact_time(p.accumulation_us));
    switch (p.mode) {
    case PreviewMode::TimeDecay:
        parts.push_back("decay-" + compact_time(p.decay_us));
        break;
    case PreviewMode::TimeSurface:
        parts.push_back("tsurf");
        break;
    case PreviewMode::Histogram:
    case PreviewMode::Difference:
        parts.push_back(std::string(p.mode == PreviewMode::Histogram ? "histo" : "diff") + "-sat" +
                        std::to_string(p.histo_saturation));
        break;
    case PreviewMode::Integration: {
        char step[16];
        std::snprintf(step, sizeof(step), "%g", p.integration_step);
        parts.push_back("integ-" + compact_time(p.decay_us) + "-step" + step);
        break;
    }
    default:
        break;
    }
    if (p.palette != Palette::Dark && p.mode != PreviewMode::TimeSurface && p.mode != PreviewMode::Integration)
        parts.push_back(p.palette == Palette::Light ? "light" : "gray");
    if (job.start_us > 0 || job.end_us >= 0) {
        char range[64];
        if (job.end_us >= 0)
            std::snprintf(range, sizeof(range), "t%g-%gs", job.start_us / 1e6, job.end_us / 1e6);
        else
            std::snprintf(range, sizeof(range), "t%gs-end", job.start_us / 1e6);
        parts.push_back(range);
    }
    if (job.apply_filters) {
        const FilterConfig &f = job.filters;
        if (f.activity_enabled) {
            std::string a = "sAct-" + compact_time(f.activity_window_us) + "-r" + std::to_string(f.activity_radius) +
                            "-n" + std::to_string(f.activity_min_neighbors);
            if (f.activity_same_polarity)
                a += "-samepol";
            if (!f.activity_look_ahead)
                a += "-causal";
            parts.push_back(a);
        }
        if (f.refractory_enabled)
            parts.push_back("sRefr-" + compact_time(f.refractory_us));
        if (f.trail_enabled) {
            const char *mode = f.trail_mode == TrailMode::Trail       ? "sTrail-" :
                               f.trail_mode == TrailMode::StcCutTrail ? "sSTCcut-" :
                                                                        "sSTCkeep-";
            parts.push_back(mode + compact_time(f.trail_threshold_us));
        }
        if (f.polarity_enabled && f.keep_on != f.keep_off)
            parts.push_back(f.keep_on ? "ONonly" : "OFFonly");
        if (f.erc_enabled) {
            std::string e = "sERC-" + compact_count(static_cast<double>(f.erc_rate));
            if (f.erc_period_us != 200)
                e += "-w" + compact_time(f.erc_period_us);
            parts.push_back(e);
        }
        if (masked && f.mask_enabled)
            parts.push_back("masked");
    }
    std::string tag;
    for (const auto &part : parts)
        tag += (tag.empty() ? "" : "_") + part;
    return tag;
}

std::filesystem::path default_image_folder(const std::filesystem::path &input, const std::string &tag) {
    auto name = input.stem();
    name += "_images_";
    name += std::filesystem::u8path(tag);
    return input.parent_path() / name;
}

ExportResult run_export(const ExportJob &job, ExportProgress *progress) {
    const auto wall_start = std::chrono::steady_clock::now();
    ExportResult result;

    auto src = EventFileSource::open(job.input, true, &result.error);
    if (!src)
        return result;
    const int width = src->width(), height = src->height();

    FilterPipeline pipeline(width, height);
    FilterConfig config = job.filters;
    if (!job.apply_filters) {
        config                    = FilterConfig();
        config.mask_enabled       = false;
    }
    pipeline.set_config(config);
    if (job.apply_filters)
        pipeline.set_mask(job.mask);

    // ---- output
    RawFileWriter raw_writer;
    std::unique_ptr<Metavision::HDF5EventFileWriter> hdf5_writer;
    CsvSink csv;
    CsvTriggerSink csv_triggers(job.output.parent_path() / (job.output.stem().string() + "_triggers.csv"));
    std::unique_ptr<VideoSink> video;
    std::unique_ptr<ImageSink> images;
    Sink sink;
    std::function<void(const EventExtTrigger *, const EventExtTrigger *)> trigger_sink = [](auto, auto) {};
    bool opened = false;
    try {
        switch (job.format) {
        case ExportFormat::RawEvt2:
        case ExportFormat::RawEvt21:
        case ExportFormat::RawEvt3: {
            const EvtFormat f = job.format == ExportFormat::RawEvt2  ? EvtFormat::Evt2 :
                                job.format == ExportFormat::RawEvt21 ? EvtFormat::Evt21 :
                                                                       EvtFormat::Evt3;
            opened = raw_writer.open(job.output, f, width, height, src->metadata(), &result.error);
            sink   = [&](const EventCD *b, const EventCD *e) { raw_writer.write(b, e); };
            trigger_sink = [&](const EventExtTrigger *b, const EventExtTrigger *e) { raw_writer.write_triggers(b, e); };
            break;
        }
        case ExportFormat::Hdf5: {
            std::unordered_map<std::string, std::string> md(src->metadata().begin(), src->metadata().end());
            md["geometry"] = std::to_string(width) + "x" + std::to_string(height);
            hdf5_writer    = std::make_unique<Metavision::HDF5EventFileWriter>(job.output, md);
            opened         = hdf5_writer->is_open();
            sink           = [&](const EventCD *b, const EventCD *e) { hdf5_writer->add_events(b, e); };
            trigger_sink   = [&](const EventExtTrigger *b, const EventExtTrigger *e) { hdf5_writer->add_events(b, e); };
            break;
        }
        case ExportFormat::Csv:
            opened = csv.open(job.output);
            sink   = [&](const EventCD *b, const EventCD *e) { csv.write(b, e); };
            trigger_sink = [&](const EventExtTrigger *b, const EventExtTrigger *e) { csv_triggers.write(b, e); };
            break;
        case ExportFormat::Avi:
            video  = std::make_unique<VideoSink>(width, height, job.preview, job.video_fps);
            opened = video->open(job.output);
            if (opened && job.start_us > 0)
                video->set_start(job.start_us);
            sink = [&](const EventCD *b, const EventCD *e) { video->write(b, e); };
            break;
        case ExportFormat::Images:
            images = std::make_unique<ImageSink>(width, height, job.preview);
            opened = images->open(job.output, &result.error);
            images->set_start(job.start_us); // frames aligned on the range start (recording time 0 by default)
            sink = [&](const EventCD *b, const EventCD *e) { images->write(b, e); };
            break;
        default:
            break;
        }
    } catch (const std::exception &e) {
        result.error = e.what();
        opened       = false;
    }
    if (!opened) {
        if (result.error.empty())
            result.error = "Cannot create " + job.output.string();
        return result;
    }

    // ---- progress estimation: bytes for RAW files, time otherwise
    uint64_t bytes_read = 0;
    if (auto *raw = src->raw_reader())
        raw->add_raw_read_callback([&](const std::uint8_t *b, const std::uint8_t *e) { bytes_read += e - b; });
    timestamp duration = -1;
    if (!src->raw_reader()) {
        try {
            duration = src->reader().get_duration();
        } catch (...) {}
    }
    timestamp last_t = 0;

    // ---- processing
    // Events are collected during each read() and processed after it: the decoder delivers the CD events and the
    // triggers of a chunk separately, so both streams are complete up to the chunk end only once read() returns.
    std::vector<EventCD> pending_cd;
    std::vector<EventExtTrigger> pending_triggers;
    src->reader().add_read_callback(
        [&](const EventCD *b, const EventCD *e) { pending_cd.insert(pending_cd.end(), b, e); });
    src->reader().add_read_callback(
        [&](const EventExtTrigger *b, const EventExtTrigger *e) { pending_triggers.insert(pending_triggers.end(), b, e); });
    // Filters holding events (activity look-ahead, ERC emulation) delay their output by up to this time.
    const timestamp hold = pipeline.holding_time();
    const auto in_range  = [&](timestamp t) { return t >= job.start_us && (job.end_us < 0 || t <= job.end_us); };

    bool past_end = false;
    auto process_pending = [&] {
        if (!pending_cd.empty()) {
            last_t = std::max(last_t, pending_cd.back().t);
            if (job.end_us >= 0 && pending_cd.back().t > job.end_us)
                past_end = true;
            auto b = std::lower_bound(pending_cd.begin(), pending_cd.end(), job.start_us,
                                      [](const EventCD &ev, timestamp t) { return ev.t < t; });
            auto e = job.end_us < 0 ? pending_cd.end() :
                                      std::upper_bound(b, pending_cd.end(), job.end_us,
                                                       [](timestamp t, const EventCD &ev) { return t < ev.t; });
            if (b != e) {
                result.events_in += static_cast<uint64_t>(e - b);
                const auto &out = pipeline.process(&*b, &*b + (e - b));
                result.events_out += out.size();
                if (!out.empty())
                    sink(out.data(), out.data() + out.size());
            }
        }
        if (!pending_triggers.empty()) {
            last_t = std::max(last_t, pending_triggers.back().t);
            pending_triggers.erase(std::remove_if(pending_triggers.begin(), pending_triggers.end(),
                                                  [&](const EventExtTrigger &t) { return !in_range(t.t); }),
                                   pending_triggers.end());
            result.triggers += pending_triggers.size();
            if (!pending_triggers.empty())
                trigger_sink(pending_triggers.data(), pending_triggers.data() + pending_triggers.size());
        }
        pending_cd.clear();
        pending_triggers.clear();
        if (raw_writer.is_open())
            raw_writer.advance(last_t - hold - 1);
    };

    try {
        while (!past_end) {
            if (progress && progress->cancel) {
                result.cancelled = true;
                break;
            }
            const bool more = src->reader().read();
            process_pending();
            if (!more)
                break;
            if (progress) {
                double f = 0;
                if (src->file_size() > 0 && src->raw_reader())
                    f = static_cast<double>(bytes_read) / static_cast<double>(src->file_size());
                else if (duration > 0)
                    f = static_cast<double>(last_t) / static_cast<double>(duration);
                if (job.end_us > 0)
                    f = std::max(f, static_cast<double>(last_t - job.start_us) /
                                        static_cast<double>(std::max<timestamp>(1, job.end_us - job.start_us)));
                progress->fraction   = std::clamp(f, 0.0, 1.0);
                progress->events_in  = result.events_in;
                progress->events_out = result.events_out;
            }
        }
        const auto &tail = pipeline.flush();
        result.events_out += tail.size();
        if (!tail.empty())
            sink(tail.data(), tail.data() + tail.size());

        switch (job.format) {
        case ExportFormat::Hdf5:
            hdf5_writer->close();
            break;
        case ExportFormat::Csv:
            if (!csv.close() || !csv_triggers.close())
                result.error = "Error while writing " + job.output.string();
            break;
        case ExportFormat::Avi:
            video->close();
            result.frames = video->frames();
            break;
        case ExportFormat::Images:
            images->close(&result.error);
            result.frames = images->frames();
            break;
        default:
            raw_writer.close();
            break;
        }
    } catch (const std::exception &e) {
        result.error = e.what();
    }

    if (progress) {
        progress->fraction   = 1.0;
        progress->events_in  = result.events_in;
        progress->events_out = result.events_out;
    }
    result.ok      = result.error.empty() && !result.cancelled;
    result.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - wall_start).count();
    return result;
}

} // namespace evs
