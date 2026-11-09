#include <csignal>
#include "net.hpp"
#ifdef _WIN32
#include <windows.h>
#endif
#include "config.hpp"
#include "reassembly.hpp"
#include "process_stats.hpp"
#include "terminal_panel.hpp"
#include <iostream>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <array>
#include <future>
#include <ctime>

static std::atomic<bool> stopping{false};
static_assert(std::atomic<bool>::is_always_lock_free, "Signal handler requires lock-free atomic bool");
static void stop_handler(int) { stopping = 1; }
#ifdef _WIN32
static BOOL WINAPI console_handler(DWORD event)
{
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT || event == CTRL_CLOSE_EVENT)
    {
        stopping = 1;
        return TRUE;
    }
    return FALSE;
}
#endif
static std::mutex log_mutex;
static Config config;
static TerminalPanel panel;
static uint64_t last_camera_hello = 0;
static std::string escaped(const std::string &s)
{
    std::string out;
    for (unsigned char c : s)
    {
        if (c == '"' || c == '\\')
            out += '\\';
        if (c >= 32)
            out += char(c);
    }
    return out;
}
static void log(const std::string &level, const std::string &msg)
{
    auto rank = [](const std::string &l)
    { return l == "ERROR" ? 2 : l == "WARN" ? 1
                                            : 0; };
    if (rank(level) < rank(config.log_level))
        return;
    std::lock_guard<std::mutex> guard(log_mutex);
    if (panel.active())
    {
        if (msg.rfind("STATS ", 0) != 0 && msg.rfind("HTTP_STATS ", 0) != 0 && msg.rfind("PROCESS ", 0) != 0)
            panel.event = level + " " + msg;
        return;
    }
    auto time = std::time(nullptr);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &time);
#else
    gmtime_r(&time, &utc);
#endif
    char stamp[32]{};
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%SZ", &utc);
    if (config.json)
        std::cout << "{\"time\":\"" << stamp << "\",\"level\":\"" << level << "\",\"message\":\"" << escaped(msg) << "\"}\n";
    else
        std::cout << stamp << " " << level << " " << msg << "\n";
    std::cout.flush();
}
static std::string camera_url(unsigned endpoint)
{
    const auto host = config.http_host == "0.0.0.0" ? "127.0.0.1" : config.http_host;
    return "http://" + host + ":" + std::to_string(config.http_port) + "/camera/" + std::to_string(endpoint) + ".mjpeg";
}
static std::string camera_location(uint64_t id)
{
    const auto &pair = config.pairs.at(id);
    return " endpoint=" + std::to_string(pair.endpoint) + " role=" + pair.role + " url=" + camera_url(pair.endpoint);
}
struct Camera
{
    iris::Reassembler frames;
    sockaddr_in source{};
    uint32_t boot = 0;
    uint64_t hello_at = 0, registered_at = 0, reconnections = 0;
    bool registered = false, ever_streamed = false;
    std::string state = "DISCOVERED";
    iris::Counters previous;
};
static std::mutex cameras_mutex;
static std::condition_variable frame_ready;
static std::map<uint64_t, Camera> cameras;
static std::atomic<uint64_t> next_client_id{1};

static void render_panel(uint64_t now, bool force = false)
{
    static uint64_t last = 0;
    if (!panel.active() || (!force && now - last < 250))
        return;
    last = now;
    std::lock_guard<std::mutex> camera_guard(cameras_mutex);
    std::vector<std::string> lines{"OpenIris UDP Bridge - cameras", "URLs neste PC | Ctrl+C para sair", ""};
    for (unsigned endpoint = 1; endpoint <= 3; ++endpoint)
    {
        std::string state = "UNPAIRED", details = "Aguardando descoberta automatica...";
        for (const auto &pair : config.pairs)
        {
            if (pair.second.endpoint != endpoint)
                continue;
            const auto &camera = cameras.at(pair.first);
            state = camera.registered ? camera.state : "WAITING";
            details = "IP: " + (camera.registered ? ip_string(camera.source) : "--") +
                      " | ID: " + id_string(pair.first) + " | " + pair.second.role;
        }
        lines.push_back("Camera " + std::to_string(endpoint) + " | " + state);
        lines.push_back(camera_url(endpoint));
        lines.push_back(details);
        lines.push_back("");
    }
    std::lock_guard<std::mutex> log_guard(log_mutex);
    lines.push_back("Ultimo evento:");
    lines.push_back(panel.event);
    panel.draw(lines);
}

struct HttpMetrics
{
    uint64_t client_id = next_client_id++, camera;
    uint64_t sent = 0, skipped = 0, expired = 0, failures = 0;
    uint64_t previous_sent = 0, reported_at = now_ms();
    double send_sum = 0, send_max = 0, bridge_sum = 0, bridge_max = 0;
    explicit HttpMetrics(uint64_t id) : camera(id) {}
    void report(bool closed = false)
    {
        const auto now = now_ms();
        if (!closed && now - reported_at < config.stats_interval * 1000ULL)
            return;
        const auto count = sent - previous_sent;
        const double seconds = double(now - reported_at) / 1000.0;
        std::ostringstream line;
        line << std::fixed << std::setprecision(2)
             << "HTTP_STATS client=" << client_id << " camera=" << id_string(camera)
             << " phase=" << (closed ? "closed" : "periodic")
             << " fps_sent=" << (seconds > 0 ? count / seconds : 0)
             << " sent=" << sent << " skipped=" << skipped << " expired=" << expired
             << " send_failures=" << failures
             << " send_ms_avg=" << (count ? send_sum / count : 0) << " send_ms_max=" << send_max
             << " bridge_ms_avg=" << (count ? bridge_sum / count : 0) << " bridge_ms_max=" << bridge_max;
        log("INFO", line.str());
        previous_sent = sent;
        reported_at = now;
        send_sum = send_max = bridge_sum = bridge_max = 0;
    }
    ~HttpMetrics()
    {
        try
        {
            report(true);
        }
        catch (...)
        {
        }
    }
};

static void client(Socket raw)
{
    OwnedSocket socket(raw);
    try
    {
        nonblocking(socket);
        int no_delay = 1;
        if (setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&no_delay), sizeof(no_delay)) != 0)
            throw std::runtime_error("Cannot enable TCP_NODELAY");
        int packageSize = 16 * 1024;
        setsockopt(socket, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<char *>(&packageSize), sizeof(packageSize));
        std::string request;
        const auto deadline = now_ms() + 2000;
        while (!stopping && now_ms() < deadline && request.size() < 4096 && request.find("\r\n\r\n") == std::string::npos)
        {
            if (!ready(socket, false, 50))
                continue;
            char buf[1024];
            int n = recv(socket, buf, sizeof(buf), 0);
            if (n <= 0)
                return;
            request.append(buf, size_t(n));
        }
        if (request.find("\r\n\r\n") == std::string::npos)
            return;
        std::istringstream first(request);
        std::string method, path, version;
        first >> method >> path >> version;
        uint64_t id = 0;
        {
            std::lock_guard<std::mutex> lock(cameras_mutex);
            for (const auto &p : config.pairs)
                if (path == "/camera/" + std::to_string(p.second.endpoint) + ".mjpeg")
                    id = p.first;
        }
        if (method != "GET" || !id)
        {
            send_text(socket, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", now_ms() + 500);
            return;
        }
        if (!send_text(socket, "HTTP/1.1 200 OK\r\nContent-Type: multipart/x-mixed-replace; boundary=frame\r\nCache-Control: no-store, no-cache, must-revalidate\r\nPragma: no-cache\r\nConnection: close\r\n\r\n", now_ms() + 1000))
            return;
        uint64_t generation = 0;
        bool first_frame = true;
        HttpMetrics metrics(id);
        while (!stopping)
        {
            metrics.report();
            // A finite housekeeping wake detects disconnected idle clients; new
            // frames wake immediately through the condition variable.
            if (ready(socket, false, 0))
            {
                char byte;
                if (recv(socket, &byte, 1, 0) <= 0)
                    return;
            }
            std::shared_ptr<const std::vector<uint8_t>> jpeg;
            uint64_t completed_at = 0;
            {
                std::unique_lock<std::mutex> lock(cameras_mutex);
                auto &c = cameras.at(id);
                if (first_frame)
                {
                    generation = c.frames.generation - (c.frames.latest ? 1 : 0);
                    first_frame = false;
                }
                frame_ready.wait_for(lock, std::chrono::milliseconds(100), [&]
                                     { return stopping.load() || c.frames.generation != generation; });
                if (stopping)
                    break;
                if (c.frames.generation != generation)
                {
                    metrics.skipped += c.frames.generation - generation - 1;
                    generation = c.frames.generation;
                    if (c.frames.latest)
                    {
                        completed_at = c.frames.last_complete;
                        if (now_ms() - completed_at < config.http_max_frame_age)
                            jpeg = c.frames.latest;
                        else
                            ++metrics.expired;
                    }
                }
            }
            if (!jpeg)
                continue;
            // Recheck immediately before writing multipart bytes. Once started,
            // complete the part or close the connection on the send deadline.
            if (now_ms() - completed_at >= config.http_max_frame_age)
            {
                ++metrics.expired;
                continue;
            }
            const auto send_started = std::chrono::steady_clock::now();
            auto until = now_ms() + 1000;
            if (!send_text(socket, "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: " + std::to_string(jpeg->size()) + "\r\n\r\n", until) ||
                !send_all(socket, jpeg->data(), jpeg->size(), until) || !send_text(socket, "\r\n", until))
            {
                ++metrics.failures;
                return;
            }
            const double send_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - send_started).count();
            const double bridge_ms = double(now_ms() - completed_at);
            ++metrics.sent;
            metrics.send_sum += send_ms;
            metrics.send_max = std::max(metrics.send_max, send_ms);
            metrics.bridge_sum += bridge_ms;
            metrics.bridge_max = std::max(metrics.bridge_max, bridge_ms);
        }
    }
    catch (const std::exception &e)
    {
        log("WARN", std::string("HTTP client: ") + e.what());
    }
}
static void http_loop(Socket listener)
{
    std::array<std::future<void>, 8> clients;
    while (!stopping)
    {
        if (!ready(listener, false, 100))
            continue;
        Socket s = accept(listener, nullptr, nullptr);
        if (s == bad_socket)
            continue;
        bool assigned = false;
        for (auto &f : clients)
            if (!f.valid() || f.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready)
            {
                try
                {
                    if (f.valid())
                        f.get();
                    f = std::async(std::launch::async, client, s);
                    assigned = true;
                }
                catch (...)
                {
                    close_socket(s);
                    assigned = true;
                }
                break;
            }
        if (!assigned)
            close_socket(s);
    }
    for (auto &f : clients)
        if (f.valid())
            f.get();
}
static void receive(Socket socket, bool control)
{
    uint8_t buffer[2048];
    sockaddr_in source{};
#ifdef _WIN32
    int length = sizeof(source);
#else
    socklen_t length = sizeof(source);
#endif
    int n = recvfrom(socket, reinterpret_cast<char *>(buffer), sizeof(buffer), 0, reinterpret_cast<sockaddr *>(&source), &length);
    iris::Header h;
    if (n <= 0 || !iris::decode(buffer, size_t(n), h))
        return;
    auto now = now_ms();
    std::lock_guard<std::mutex> lock(cameras_mutex);
    if (control)
    {
        if (h.type != iris::Type::hello)
            return;
        last_camera_hello = now;
        if (h.server && h.server != config.server)
        {
            if (config.auto_pair && config.pairs.empty())
            {
                // Recover a lost installation identity from an already paired camera.
                config.server = h.server;
                config.save_state();
                log("WARN", "PAIRING_RECOVERED server=" + id_string(config.server) + " from_camera=" + id_string(h.camera) + " ip=" + ip_string(source));
            }
            else
            {
                static uint64_t last_warning = 0;
                if (!last_warning || now - last_warning >= 10000)
                {
                    log("WARN", "PAIRING_MISMATCH camera=" + id_string(h.camera) + " ip=" + ip_string(source) + " camera_server=" + id_string(h.server) + " bridge_server=" + id_string(config.server) + " (camera detected; existing pairings or auto_pair=false prevent recovery)");
                    last_warning = now;
                }
                return;
            }
        }
        auto p = config.pairs.find(h.camera);
        if (p == config.pairs.end())
        {
            if (!config.auto_pair || config.pairs.size() >= 3)
            {
                static uint64_t last_warning = 0;
                if (!last_warning || now - last_warning >= 10000)
                {
                    log("WARN", "PAIRING_REJECTED camera=" + id_string(h.camera) + " (auto_pair=false or all three camera slots occupied)");
                    last_warning = now;
                }
                return;
            }
            unsigned endpoint = 1;
            for (; endpoint <= 3; ++endpoint)
            {
                bool used = false;
                for (auto &item : config.pairs)
                    used |= item.second.endpoint == endpoint;
                if (!used)
                    break;
            }
            config.pairs[h.camera] = {endpoint, "unassigned"};
            config.save_state();
            log("INFO", "FOUND camera=" + id_string(h.camera) + " endpoint=" + std::to_string(endpoint) + " role=unassigned (edit config.ini to assign)");
        }
        auto &c = cameras[h.camera];
        if (c.registered && now - c.hello_at < 6000 && c.source.sin_addr.s_addr != source.sin_addr.s_addr)
        {
            // An active ID cannot migrate between IPs until its presence lease expires.
            return;
        }
        if (!c.registered || c.boot != h.boot || c.source.sin_addr.s_addr != source.sin_addr.s_addr)
        {
            if (c.registered)
                ++c.reconnections;
            c.frames.reset();
            c.boot = h.boot;
            c.source = source;
            c.registered = true;
            c.registered_at = now;
            c.state = "CONNECTING";
            std::string metadata;
            for (size_t i = 0; i < h.payload; ++i)
            {
                auto ch = buffer[iris::header_size + i];
                metadata += ch >= 32 && ch <= 126 ? char(ch) : '?';
            }
            log("INFO", "REGISTER camera=" + id_string(h.camera) + " ip=" + ip_string(source) + " boot=" + std::to_string(h.boot) + " device=" + metadata);
            log("INFO", "CAMERA camera=" + id_string(h.camera) + " ip=" + ip_string(source) + " state=CONNECTING" + camera_location(h.camera));
        }
        c.hello_at = now;
        iris::Header ack = h;
        ack.type = iris::Type::accept;
        ack.server = config.server;
        ack.payload = 4;
        uint8_t reply[iris::header_size + 4];
        iris::encode(reply, ack);
        iris::write_be(reply + iris::header_size, config.udp_port, 2);
        iris::write_be(reply + iris::header_size + 2, config.fps, 2);
        sendto(socket, reinterpret_cast<char *>(reply), sizeof(reply), 0, reinterpret_cast<sockaddr *>(&source), sizeof(source));
    }
    else
    {
        auto found = cameras.find(h.camera);
        if (h.type != iris::Type::jpeg || h.server != config.server || found == cameras.end())
            return;
        auto &c = found->second;
        if (!c.registered || now - c.hello_at > 6000 || c.boot != h.boot || c.source.sin_addr.s_addr != source.sin_addr.s_addr)
            return;
        if (c.frames.push(h, buffer + iris::header_size, now, config.timeout))
        {
            frame_ready.notify_all();
            if (c.state == "STREAMING")
                return;
            log("INFO", std::string(c.ever_streamed ? "RECONNECTED" : "STREAMING") + " camera=" + id_string(h.camera) + camera_location(h.camera));
            if (c.ever_streamed && c.state == "OFFLINE")
                ++c.reconnections;
            c.ever_streamed = true;
            c.state = "STREAMING";
        }
    }
}
static void monitor(uint64_t now, bool print, double seconds)
{
    static uint64_t last_discovery_warning = now;
    if (now - last_discovery_warning >= 10000 && (!last_camera_hello || now - last_camera_hello >= 10000))
    {
        log("WARN", "DISCOVERY_WAIT: no camera HELLO received in the last 10 seconds on UDP " + std::to_string(config.control_port) + "; check camera power/Wi-Fi, same LAN, Windows firewall for this executable, and Wi-Fi client isolation.");
        last_discovery_warning = now;
    }
    static auto prior_process = process_stats();
    if (print)
    {
        auto current = process_stats();
        log("INFO", "PROCESS cpu_pct_one_core=" + std::to_string(double(current.cpu_us - prior_process.cpu_us) / (seconds * 10000.0)) + " rss_bytes=" + std::to_string(current.rss_bytes));
        prior_process = current;
    }
    std::lock_guard<std::mutex> lock(cameras_mutex);
    for (auto &pair : cameras)
    {
        auto &c = pair.second;
        c.frames.expire(now, config.timeout);
        auto age = c.frames.last_complete ? now - c.frames.last_complete : now - c.registered_at;
        auto state = c.state;
        if ((c.frames.last_complete && age >= config.offline) || (!c.frames.last_complete && c.registered && age >= config.offline))
            state = "OFFLINE";
        else if (c.frames.last_complete && age >= 500)
            state = "DEGRADED";
        if (state != c.state)
        {
            log("WARN", state + " camera=" + id_string(pair.first));
            c.state = state;
        }
        if (!print)
            continue;
        const auto &s = c.frames.stats;
        const auto &old = c.previous;
        double received = double(s.packets - old.packets), missing = double(s.missing - old.missing);
        std::ostringstream line;
        line << std::fixed << std::setprecision(1) << "STATS camera=" << id_string(pair.first) << " role=" << config.pairs.at(pair.first).role
             << " ip=" << ip_string(c.source) << " state=" << c.state << " fps_rx=" << (s.started - old.started) / seconds
             << " fps_complete=" << (s.completed - old.completed) / seconds << " bytes_s=" << (s.bytes - old.bytes) / seconds
             << " loss_est_pct=" << (received + missing ? 100.0 * missing / (received + missing) : 0)
             << " dropped=" << s.dropped << " duplicates=" << s.duplicates << " invalid=" << s.invalid << " stale=" << s.stale
             << " age_ms=" << (c.frames.last_complete ? std::to_string(age) : "none") << " reconnects=" << c.reconnections;
        log("INFO", line.str());
        c.previous = s;
    }
}
int main(int argc, char **argv)
{
    try
    {
        Network network;
        std::filesystem::path path = "config.ini";
#ifdef _WIN32
        std::wstring executable(32768, L'\0');
        const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
        if (!length || length >= executable.size())
            throw std::runtime_error("Cannot locate executable directory");
        executable.resize(length);
        path = std::filesystem::path(executable).parent_path() / "config.ini";
#endif
        std::string level;
        unsigned interval = 0;
        bool json = false;
        for (int i = 1; i < argc; ++i)
        {
            std::string a = argv[i];
            if (a == "--help")
            {
                std::cout << "openiris-udp-bridge [--config config.ini] [--log-level INFO|WARN|ERROR] [--stats-interval seconds] [--json]\n";
                return 0;
            }
            if (a == "--json")
            {
                json = true;
                continue;
            }
            if (i + 1 >= argc)
                throw std::runtime_error("Missing argument: " + a);
            if (a == "--config")
                path = argv[++i];
            else if (a == "--log-level")
                level = argv[++i];
            else if (a == "--stats-interval")
                interval = number(argv[++i], 1, 3600);
            else
                throw std::runtime_error("Unknown argument: " + a);
        }
        config.read(path);
        if (std::filesystem::path(config.state_file).is_relative())
            config.state_file = (std::filesystem::absolute(path).parent_path() / config.state_file).string();
        if (!level.empty())
            config.log_level = level;
        if (interval)
            config.stats_interval = interval;
        config.json = json;
        config.validate();
        config.load_state();
        config.save_state();
        for (auto &p : config.pairs)
            cameras.try_emplace(p.first);
        OwnedSocket udp(bind_socket(config.udp_host, config.udp_port, false));
        OwnedSocket control(bind_socket(config.udp_host, config.control_port, false));
        OwnedSocket http(bind_socket(config.http_host, config.http_port, true));
        int buffer = 1024 * 1024;
        setsockopt(udp, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<char *>(&buffer), sizeof(buffer));
        std::signal(SIGINT, stop_handler);
        std::signal(SIGTERM, stop_handler);
#ifdef _WIN32
        SetConsoleCtrlHandler(console_handler, TRUE);
#endif
        log("INFO", "UDP=" + config.udp_host + ":" + std::to_string(config.udp_port) + " discovery=" + std::to_string(config.control_port) + " HTTP=" + config.http_host + ":" + std::to_string(config.http_port) + " server=" + id_string(config.server));
        log("INFO", "Discovery uses camera UDP broadcast/unicast; if none appear, check Wi-Fi client isolation and inbound UDP firewall rules.");
        log("INFO", "Camera URLs for this PC (copy into your browser or tracking app; video starts when the camera is streaming):");
        for (unsigned endpoint = 1; endpoint <= 3; ++endpoint)
        {
            std::string details = " state=UNPAIRED";
            for (const auto &pair : config.pairs)
                if (pair.second.endpoint == endpoint)
                    details = " camera=" + id_string(pair.first) + " role=" + pair.second.role + " state=WAITING";
            log("INFO", "CAMERA endpoint=" + std::to_string(endpoint) + details + " url=" + camera_url(endpoint));
        }
        panel.start(config.json);
        render_panel(now_ms(), true);
        auto http_task = std::async(std::launch::async, http_loop, http.s);
        auto last = now_ms();
        auto tick = last;
        try
        {
            while (!stopping)
            {
                // Bounded batches keep control and expiration responsive under video load.
                if (ready(control, false, 0))
                    receive(control, true);
                if (ready(udp, false, 5))
                    for (int i = 0; i < 64 && ready(udp, false, 0); ++i)
                        receive(udp, false);
                auto now = now_ms();
                if (now - tick >= 20)
                {
                    bool print = now - last >= config.stats_interval * 1000ULL;
                    monitor(now, print, double(now - last) / 1000.0);
                    render_panel(now);
                    tick = now;
                    if (print)
                        last = now;
                }
            }
        }
        catch (...)
        {
            stopping = 1;
            frame_ready.notify_all();
            http_task.get();
            throw;
        }
        frame_ready.notify_all();
        http_task.get();
        log("INFO", "Stopped");
        render_panel(now_ms(), true);
        panel.stop();
    }
    catch (const std::exception &e)
    {
        panel.stop();
        std::cerr << "ERROR " << e.what() << "\n";
        return 1;
    }
    return 0;
}
