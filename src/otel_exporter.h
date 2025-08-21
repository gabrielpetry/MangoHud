#pragma once
#ifndef MANGOHUD_OTEL_EXPORTER_H
#define MANGOHUD_OTEL_EXPORTER_H

#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <map>

struct overlay_params;

class OtelExporter {
public:
    explicit OtelExporter(const overlay_params* params);
    ~OtelExporter();

    void start();
    void stop();
    void update_metrics();
    bool is_enabled() const { return enabled_; }

private:
    void run();
    void setup_http_server();
    void handle_metrics_request(int client_socket);
    std::string generate_metrics();
    std::string escape_label_value(const std::string& value);

    const overlay_params* params_;
    std::atomic<bool> enabled_;
    std::atomic<bool> should_stop_;
    std::thread server_thread_;
    std::thread metrics_thread_;

    std::mutex metrics_mutex_;
    std::condition_variable metrics_cv_;

    int server_socket_;
    std::atomic<bool> metrics_updated_;
    std::string bind_address_;
    uint16_t bind_port_;

    // Cached metrics
    struct MetricsData {
        double fps = 0.0;
        float frametime = 0.0;
        float cpu_load = 0.0;
        float cpu_power = 0.0;
        int cpu_mhz = 0;
        int cpu_temp = 0;
        float gpu_load = 0.0;
        int gpu_temp = 0;
        int gpu_core_clock = 0;
        int gpu_mem_clock = 0;
        float gpu_power = 0.0;
        float gpu_vram_used = 0.0;
        float ram_used = 0.0;
        float swap_used = 0.0;
        float process_rss = 0.0;
        std::string process_name;
        std::string graphics_api;
        int process_pid = 0;
        std::chrono::steady_clock::time_point timestamp;
    } current_metrics_;

    void parse_bind_address(const std::string& address);
    void start_delay_timer();
};

extern std::unique_ptr<OtelExporter> otel_exporter;

#endif // MANGOHUD_OTEL_EXPORTER_H
