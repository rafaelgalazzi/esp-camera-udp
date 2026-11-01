#pragma once
#include <fstream>
#include <sstream>
#include <map>
#include <set>
#include <string>
#include <stdexcept>
#include <iomanip>
#include <random>
#include <filesystem>

inline std::string trim(std::string s)
{
    auto a = s.find_first_not_of(" \r\n\t");
    return a == std::string::npos ? "" : s.substr(a, s.find_last_not_of(" \r\n\t") - a + 1);
}
inline uint64_t hex_id(const std::string &s)
{
    if (s.empty() || s.size() > 16 || s.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
        throw std::runtime_error("Invalid hexadecimal ID: " + s);
    auto v = std::stoull(s, nullptr, 16);
    if (!v)
        throw std::runtime_error("ID cannot be zero");
    return v;
}
inline std::string id_string(uint64_t v)
{
    std::ostringstream s;
    s << std::hex << std::setfill('0') << std::setw(16) << v;
    return s.str();
}
inline unsigned number(const std::string &s, unsigned lo, unsigned hi)
{
    size_t end = 0;
    auto n = std::stoul(s, &end);
    if (end != s.size() || n < lo || n > hi)
        throw std::runtime_error("Value out of range: " + s);
    return unsigned(n);
}
struct Pairing
{
    unsigned endpoint = 0;
    std::string role = "unassigned";
};
struct Config
{
    std::string udp_host = "0.0.0.0", http_host = "127.0.0.1", state_file = "pairings.txt", log_level = "INFO";
    uint16_t udp_port = 5000, control_port = 5001, http_port = 8080;
    unsigned timeout = 100, offline = 3000, stats_interval = 5, fps = 90;
    unsigned http_max_frame_age = 100;
    bool auto_pair = true, json = false;
    uint64_t server = 0;
    std::map<uint64_t, Pairing> pairs;
    void validate() const
    {
        if (log_level != "INFO" && log_level != "WARN" && log_level != "ERROR")
            throw std::runtime_error("log_level must be INFO, WARN or ERROR");
        if (udp_port == control_port)
            throw std::runtime_error("UDP and control ports must differ");
        if (pairs.size() > 3)
            throw std::runtime_error("At most three cameras are supported");
        std::set<unsigned> endpoints;
        std::set<std::string> roles;
        for (const auto &p : pairs)
        {
            if (p.second.endpoint < 1 || p.second.endpoint > 3 || !endpoints.insert(p.second.endpoint).second)
                throw std::runtime_error("Camera endpoints must be unique, 1..3");
            const auto &r = p.second.role;
            if (r != "unassigned" && r != "left_eye" && r != "right_eye" && r != "face")
                throw std::runtime_error("Invalid camera role");
            if (r != "unassigned" && !roles.insert(r).second)
                throw std::runtime_error("Camera roles must be unique");
        }
    }
    void read(const std::string &path)
    {
        std::ifstream f(path);
        if (!f)
            throw std::runtime_error("Cannot open config: " + path);
        std::string line, section;
        unsigned lineno = 0;
        while (std::getline(f, line))
        {
            ++lineno;
            line = trim(line);
            if (line.empty() || line[0] == '#' || line[0] == ';')
                continue;
            if (line.front() == '[' && line.back() == ']')
            {
                section = line.substr(1, line.size() - 2);
                continue;
            }
            auto eq = line.find('=');
            if (eq == std::string::npos)
                throw std::runtime_error("Expected key=value at line " + std::to_string(lineno));
            auto key = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
            if (section.rfind("camera.", 0) == 0)
            {
                auto &p = pairs[hex_id(section.substr(7))];
                if (key == "endpoint")
                    p.endpoint = number(v, 1, 3);
                else if (key == "role")
                    p.role = v;
                else
                    throw std::runtime_error("Unknown camera key: " + key);
                continue;
            }
            key = section + "." + key;
            if (key == "udp.host")
                udp_host = v;
            else if (key == "udp.port")
                udp_port = uint16_t(number(v, 1, 65535));
            else if (key == "udp.frame_timeout_ms")
                timeout = number(v, 10, 2000);
            else if (key == "http.host")
                http_host = v;
            else if (key == "http.port")
                http_port = uint16_t(number(v, 1, 65535));
            else if (key == "http.max_frame_age_ms")
                http_max_frame_age = number(v, 1, 60000);
            else if (key == "discovery.port")
                control_port = uint16_t(number(v, 1, 65535));
            else if (key == "discovery.auto_pair")
            {
                if (v != "true" && v != "false")
                    throw std::runtime_error("Expected true/false");
                auto_pair = v == "true";
            }
            else if (key == "discovery.state_file")
                state_file = v;
            else if (key == "stream.max_fps")
                fps = number(v, 1, 90);
            else if (key == "monitoring.offline_ms")
                offline = number(v, 1000, 60000);
            else if (key == "monitoring.stats_interval_seconds")
                stats_interval = number(v, 1, 3600);
            else if (key == "monitoring.log_level")
                log_level = v;
            else
                throw std::runtime_error("Unknown config key: " + key);
        }
        validate();
    }
    void load_state()
    {
        std::ifstream f(state_file);
        if (!f)
        {
            if (std::filesystem::exists(state_file))
                throw std::runtime_error("Cannot read state");
            return;
        }
        std::string label, id, role;
        unsigned ep;
        if (!(f >> label >> id) || label != "server")
            throw std::runtime_error("Invalid pairing state");
        server = hex_id(id);
        while (f >> label)
        {
            if (label != "camera" || !(f >> id >> ep >> role))
                throw std::runtime_error("Invalid pairing state");
            auto camera = hex_id(id);
            if (!pairs.count(camera))
                pairs[camera] = {ep, role};
        }
        validate();
    }
    void save_state()
    {
        if (!server)
        {
            std::random_device r;
            server = (uint64_t(r()) << 32) | r();
            if (!server)
                server = 1;
        }
        const auto tmp = state_file + ".tmp";
        {
            std::ofstream f(tmp, std::ios::trunc);
            if (!f)
                throw std::runtime_error("Cannot write pairing state");
            f << "server " << id_string(server) << "\n";
            for (const auto &p : pairs)
                f << "camera " << id_string(p.first) << " " << p.second.endpoint << " " << p.second.role << "\n";
            f.flush();
            if (!f)
                throw std::runtime_error("Cannot flush pairing state");
        }
#ifdef _WIN32
        if (!MoveFileExA(tmp.c_str(), state_file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("Cannot replace pairing state");
#else
        std::filesystem::rename(tmp, state_file);
#endif
    }
};
