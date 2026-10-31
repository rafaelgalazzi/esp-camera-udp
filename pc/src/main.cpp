#include <csignal>
#include "net.hpp"
#ifdef _WIN32
#include <windows.h>
#endif
#include "config.hpp"
#include "reassembly.hpp"
#include "process_stats.hpp"
#include <iostream>
#include <mutex>
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
static std::map<uint64_t, Camera> cameras;

static void client(Socket raw)
{
    OwnedSocket socket(raw);
    try
    {
        nonblocking(socket);
        int small = 16 * 1024;
        setsockopt(socket, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<char *>(&small), sizeof(small));
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
        while (!stopping)
        {
            std::shared_ptr<const std::vector<uint8_t>> jpeg;
            {
                std::lock_guard<std::mutex> lock(cameras_mutex);
                auto &c = cameras.at(id);
                if (c.frames.generation != generation && c.frames.latest && now_ms() - c.frames.last_complete < config.offline)
                {
                    jpeg = c.frames.latest;
                    generation = c.frames.generation;
                }
            }
            if (!jpeg)
            {
                if (ready(socket, false, 20))
                {
                    char byte;
                    if (recv(socket, &byte, 1, 0) <= 0)
                        return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            auto until = now_ms() + 1000;
            if (!send_text(socket, "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: " + std::to_string(jpeg->size()) + "\r\n\r\n", until) ||
                !send_all(socket, jpeg->data(), jpeg->size(), until) || !send_text(socket, "\r\n", until))
                return;
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
        if (h.type != iris::Type::hello || (h.server && h.server != config.server))
            return;
        auto p = config.pairs.find(h.camera);
        if (p == config.pairs.end())
        {
            if (!config.auto_pair || config.pairs.size() >= 3)
                return;
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
        if (c.frames.push(h, buffer + iris::header_size, now, config.timeout) && c.state != "STREAMING")
        {
            log("INFO", std::string(c.ever_streamed ? "RECONNECTED" : "STREAMING") + " camera=" + id_string(h.camera));
            if (c.ever_streamed && c.state == "OFFLINE")
                ++c.reconnections;
            c.ever_streamed = true;
            c.state = "STREAMING";
        }
    }
}
static void monitor(uint64_t now, bool print, double seconds)
{
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
        std::string path = "config.ini", level;
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
                    tick = now;
                    if (print)
                        last = now;
                }
            }
        }
        catch (...)
        {
            stopping = 1;
            http_task.get();
            throw;
        }
        http_task.get();
        log("INFO", "Stopped");
    }
    catch (const std::exception &e)
    {
        std::cerr << "ERROR " << e.what() << "\n";
        return 1;
    }
    return 0;
}
