#include "otel_exporter.h"
#include "overlay_params.h"
#include "logging.h"
#include "overlay.h"
#include "config.h"
#include "hud_elements.h"
#include <spdlog/spdlog.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <sstream>
#include <iomanip>

std::unique_ptr<OtelExporter> otel_exporter;

extern logData currentLogData;

OtelExporter::OtelExporter(const overlay_params* params)
    : params_(params)
    , enabled_(params->enabled[OVERLAY_PARAM_ENABLED_otel_enabled])
    , should_stop_(false)
    , server_socket_(-1)
    , metrics_updated_(false) 
{
    if (!enabled_) {
        return;
    }
    
    parse_bind_address(params->otel_port);
    SPDLOG_INFO("OpenTelemetry exporter initialized on {}:{}", bind_address_, bind_port_);
}

OtelExporter::~OtelExporter() {
    SPDLOG_DEBUG("OpenTelemetry exporter destructor called");
    stop();
}

void OtelExporter::parse_bind_address(const std::string& address) {
    size_t colon_pos = address.find(':');
    if (colon_pos != std::string::npos) {
        bind_address_ = address.substr(0, colon_pos);
        std::string port_str = address.substr(colon_pos + 1);
        try {
            int port = std::stoi(port_str);
            if (port < 1 || port > 65535) {
                SPDLOG_ERROR("Invalid port number for OpenTelemetry exporter: {}. Using default 16969", port);
                bind_port_ = 16969;
            } else {
                bind_port_ = static_cast<uint16_t>(port);
            }
        } catch (const std::exception& e) {
            SPDLOG_ERROR("Failed to parse port for OpenTelemetry exporter: {}. Using default 16969", port_str);
            bind_port_ = 16969;
        }
    } else {
        bind_address_ = "0.0.0.0";
        try {
            int port = std::stoi(address);
            if (port < 1 || port > 65535) {
                SPDLOG_ERROR("Invalid port number for OpenTelemetry exporter: {}. Using default 16969", port);
                bind_port_ = 16969;
            } else {
                bind_port_ = static_cast<uint16_t>(port);
            }
        } catch (const std::exception& e) {
            SPDLOG_ERROR("Failed to parse port for OpenTelemetry exporter: {}. Using default 16969", address);
            bind_port_ = 16969;
        }
    }
    
    // Validate IP address format (basic check)
    if (bind_address_ != "0.0.0.0" && bind_address_ != "127.0.0.1" && 
        bind_address_ != "localhost" && bind_address_.find_first_not_of("0123456789.") != std::string::npos) {
        SPDLOG_WARN("IP address format might be invalid for OpenTelemetry exporter: {}", bind_address_);
    }
}

void OtelExporter::start() {
    if (!enabled_ || server_thread_.joinable()) {
        return;
    }
    
    should_stop_ = false;
    server_thread_ = std::thread(&OtelExporter::start_delay_timer, this);
    metrics_thread_ = std::thread(&OtelExporter::run, this);
}

void OtelExporter::start_delay_timer() {
    SPDLOG_INFO("OpenTelemetry exporter starting in {} seconds", params_->otel_start_timeout);
    std::this_thread::sleep_for(std::chrono::seconds(params_->otel_start_timeout));
    
    if (!should_stop_) {
        setup_http_server();
    }
}

void OtelExporter::stop() {
    should_stop_ = true;
    
    if (server_socket_ >= 0) {
        close(server_socket_);
        server_socket_ = -1;
    }
    
    metrics_cv_.notify_all();
    
    if (server_thread_.joinable()) {
        server_thread_.join();
    }
    
    if (metrics_thread_.joinable()) {
        metrics_thread_.join();
    }
}

void OtelExporter::setup_http_server() {
    server_socket_ = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket_ < 0) {
        SPDLOG_ERROR("Failed to create socket for OpenTelemetry exporter: {}", strerror(errno));
        return;
    }
    
    int opt = 1;
    if (setsockopt(server_socket_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        SPDLOG_WARN("Failed to set SO_REUSEADDR: {}", strerror(errno));
    }
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(bind_port_);
    
    if (inet_pton(AF_INET, bind_address_.c_str(), &addr.sin_addr) <= 0) {
        SPDLOG_ERROR("Invalid bind address for OpenTelemetry exporter: {}", bind_address_);
        close(server_socket_);
        server_socket_ = -1;
        return;
    }
    
    if (bind(server_socket_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        SPDLOG_ERROR("Failed to bind OpenTelemetry exporter to {}:{}: {}", 
                    bind_address_, bind_port_, strerror(errno));
        close(server_socket_);
        server_socket_ = -1;
        return;
    }
    
    if (listen(server_socket_, 5) < 0) {
        SPDLOG_ERROR("Failed to listen on OpenTelemetry exporter socket: {}", strerror(errno));
        close(server_socket_);
        server_socket_ = -1;
        return;
    }
    
    SPDLOG_INFO("OpenTelemetry exporter listening on {}:{}", bind_address_, bind_port_);
    
    while (!should_stop_) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(server_socket_, &read_fds);
        
        struct timeval timeout;
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;
        
        int result = select(server_socket_ + 1, &read_fds, nullptr, nullptr, &timeout);
        if (result < 0) {
            if (errno != EINTR) {
                SPDLOG_ERROR("Select failed on OpenTelemetry exporter socket: {}", strerror(errno));
                break;
            }
            continue;
        }
        
        if (result > 0 && FD_ISSET(server_socket_, &read_fds)) {
            int client_socket = accept(server_socket_, nullptr, nullptr);
            if (client_socket >= 0) {
                handle_metrics_request(client_socket);
                close(client_socket);
            }
        }
    }
}

void OtelExporter::handle_metrics_request(int client_socket) {
    char buffer[1024];
    int bytes_read = recv(client_socket, buffer, sizeof(buffer) - 1, 0);
    if (bytes_read <= 0) {
        return;
    }
    
    buffer[bytes_read] = '\0';
    
    // Simple HTTP request parsing - look for GET /metrics
    if (strstr(buffer, "GET /metrics") != nullptr) {
        std::string metrics = generate_metrics();
        
        std::ostringstream response;
        response << "HTTP/1.1 200 OK\r\n";
        response << "Content-Type: text/plain; version=0.0.4; charset=utf-8\r\n";
        response << "Content-Length: " << metrics.length() << "\r\n";
        response << "Connection: close\r\n";
        response << "\r\n";
        response << metrics;
        
        std::string response_str = response.str();
        send(client_socket, response_str.c_str(), response_str.length(), 0);
    } else {
        // Return 404 for other requests
        const char* not_found = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        send(client_socket, not_found, strlen(not_found), 0);
    }
}

void OtelExporter::run() {
    while (!should_stop_) {
        std::unique_lock<std::mutex> lock(metrics_mutex_);
        
        auto timeout = std::chrono::milliseconds(params_->otel_update_interval);
        if (metrics_cv_.wait_for(lock, timeout, [this] { return should_stop_; })) {
            break;
        }
        
        update_metrics();
    }
}

void OtelExporter::update_metrics() {
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    
    // Update metrics from current log data
    current_metrics_.fps = currentLogData.fps;
    current_metrics_.frametime = currentLogData.frametime;
    current_metrics_.cpu_load = currentLogData.cpu_load;
    current_metrics_.cpu_power = currentLogData.cpu_power;
    current_metrics_.cpu_mhz = currentLogData.cpu_mhz;
    current_metrics_.cpu_temp = currentLogData.cpu_temp;
    current_metrics_.gpu_load = static_cast<float>(currentLogData.gpu_load);
    current_metrics_.gpu_temp = currentLogData.gpu_temp;
    current_metrics_.gpu_core_clock = currentLogData.gpu_core_clock;
    current_metrics_.gpu_mem_clock = currentLogData.gpu_mem_clock;
    current_metrics_.gpu_power = currentLogData.gpu_power;
    current_metrics_.gpu_vram_used = currentLogData.gpu_vram_used;
    current_metrics_.ram_used = currentLogData.ram_used;
    current_metrics_.swap_used = currentLogData.swap_used;
    current_metrics_.process_rss = currentLogData.process_rss;
    
    // Get process information
    current_metrics_.process_name = get_program_name();
    current_metrics_.process_pid = getpid();
    
    // Get graphics API from HUDElements
    extern HudElements HUDElements;
    if (HUDElements.sw_stats && HUDElements.sw_stats->engine < 18) { // Sanity check
        extern const char* engines[];
        current_metrics_.graphics_api = engines[HUDElements.sw_stats->engine];
    } else {
        current_metrics_.graphics_api = "unknown";
    }
    
    current_metrics_.timestamp = std::chrono::steady_clock::now();
    metrics_updated_ = true;
}

std::string OtelExporter::escape_label_value(const std::string& value) {
    std::string result;
    result.reserve(value.length() + 10);
    
    for (char c : value) {
        switch (c) {
            case '\\':
                result += "\\\\";
                break;
            case '"':
                result += "\\\"";
                break;
            case '\n':
                result += "\\n";
                break;
            case '\r':
                result += "\\r";
                break;
            case '\t':
                result += "\\t";
                break;
            default:
                result += c;
                break;
        }
    }
    
    return result;
}

std::string OtelExporter::generate_metrics() {
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    
    std::ostringstream oss;
    
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    
    std::string labels = "process_name=\"" + escape_label_value(current_metrics_.process_name) + 
                        "\",graphics_api=\"" + escape_label_value(current_metrics_.graphics_api) + 
                        "\",pid=\"" + std::to_string(current_metrics_.process_pid) + "\"";
    
    // FPS metrics
    oss << "# HELP mangohud_fps_current Current frames per second\n";
    oss << "# TYPE mangohud_fps_current gauge\n";
    oss << "mangohud_fps_current{" << labels << "} " << std::fixed << std::setprecision(2) 
        << current_metrics_.fps << " " << now << "\n";
    
    oss << "# HELP mangohud_frametime_ms Current frame time in milliseconds\n";
    oss << "# TYPE mangohud_frametime_ms gauge\n";
    oss << "mangohud_frametime_ms{" << labels << "} " << std::fixed << std::setprecision(3) 
        << current_metrics_.frametime << " " << now << "\n";
    
    // CPU metrics
    oss << "# HELP mangohud_cpu_load_percent CPU load percentage\n";
    oss << "# TYPE mangohud_cpu_load_percent gauge\n";
    oss << "mangohud_cpu_load_percent{" << labels << "} " << std::fixed << std::setprecision(1) 
        << current_metrics_.cpu_load << " " << now << "\n";
    
    oss << "# HELP mangohud_cpu_power_watts CPU power consumption in watts\n";
    oss << "# TYPE mangohud_cpu_power_watts gauge\n";
    oss << "mangohud_cpu_power_watts{" << labels << "} " << std::fixed << std::setprecision(1) 
        << current_metrics_.cpu_power << " " << now << "\n";
    
    oss << "# HELP mangohud_cpu_frequency_mhz CPU frequency in MHz\n";
    oss << "# TYPE mangohud_cpu_frequency_mhz gauge\n";
    oss << "mangohud_cpu_frequency_mhz{" << labels << "} " << current_metrics_.cpu_mhz 
        << " " << now << "\n";
    
    oss << "# HELP mangohud_cpu_temperature_celsius CPU temperature in Celsius\n";
    oss << "# TYPE mangohud_cpu_temperature_celsius gauge\n";
    oss << "mangohud_cpu_temperature_celsius{" << labels << "} " << current_metrics_.cpu_temp 
        << " " << now << "\n";
    
    // GPU metrics
    oss << "# HELP mangohud_gpu_load_percent GPU load percentage\n";
    oss << "# TYPE mangohud_gpu_load_percent gauge\n";
    oss << "mangohud_gpu_load_percent{" << labels << "} " << std::fixed << std::setprecision(1) 
        << current_metrics_.gpu_load << " " << now << "\n";
    
    oss << "# HELP mangohud_gpu_temperature_celsius GPU temperature in Celsius\n";
    oss << "# TYPE mangohud_gpu_temperature_celsius gauge\n";
    oss << "mangohud_gpu_temperature_celsius{" << labels << "} " << current_metrics_.gpu_temp 
        << " " << now << "\n";
    
    oss << "# HELP mangohud_gpu_core_clock_mhz GPU core clock in MHz\n";
    oss << "# TYPE mangohud_gpu_core_clock_mhz gauge\n";
    oss << "mangohud_gpu_core_clock_mhz{" << labels << "} " << current_metrics_.gpu_core_clock 
        << " " << now << "\n";
    
    oss << "# HELP mangohud_gpu_memory_clock_mhz GPU memory clock in MHz\n";
    oss << "# TYPE mangohud_gpu_memory_clock_mhz gauge\n";
    oss << "mangohud_gpu_memory_clock_mhz{" << labels << "} " << current_metrics_.gpu_mem_clock 
        << " " << now << "\n";
    
    oss << "# HELP mangohud_gpu_power_watts GPU power consumption in watts\n";
    oss << "# TYPE mangohud_gpu_power_watts gauge\n";
    oss << "mangohud_gpu_power_watts{" << labels << "} " << std::fixed << std::setprecision(1) 
        << current_metrics_.gpu_power << " " << now << "\n";
    
    oss << "# HELP mangohud_gpu_vram_used_gb GPU VRAM used in GB\n";
    oss << "# TYPE mangohud_gpu_vram_used_gb gauge\n";
    oss << "mangohud_gpu_vram_used_gb{" << labels << "} " << std::fixed << std::setprecision(3) 
        << current_metrics_.gpu_vram_used << " " << now << "\n";
    
    // Memory metrics
    oss << "# HELP mangohud_ram_used_gb System RAM used in GB\n";
    oss << "# TYPE mangohud_ram_used_gb gauge\n";
    oss << "mangohud_ram_used_gb{" << labels << "} " << std::fixed << std::setprecision(3) 
        << current_metrics_.ram_used << " " << now << "\n";
    
    oss << "# HELP mangohud_swap_used_gb System swap used in GB\n";
    oss << "# TYPE mangohud_swap_used_gb gauge\n";
    oss << "mangohud_swap_used_gb{" << labels << "} " << std::fixed << std::setprecision(3) 
        << current_metrics_.swap_used << " " << now << "\n";
    
    oss << "# HELP mangohud_process_rss_gb Process RSS memory in GB\n";
    oss << "# TYPE mangohud_process_rss_gb gauge\n";
    oss << "mangohud_process_rss_gb{" << labels << "} " << std::fixed << std::setprecision(3) 
        << current_metrics_.process_rss << " " << now << "\n";
    
    return oss.str();
}