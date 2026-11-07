#pragma once
#include <network/secure.hpp>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace game::secure_config {
inline std::string env(const char* key) {
    const char* value=std::getenv(key);
    return value && *value ? value : "";
}
inline std::string storage_directory() {
    namespace fs=std::filesystem;
    // Keep the private server identity and the client's trust pins outside Git.
    const auto xdg=env("XDG_CONFIG_HOME");
    if(!xdg.empty())return (fs::path(xdg)/"vulkan-game"/"network").string();
    const auto local=env("LOCALAPPDATA");
    if(!local.empty())return (fs::path(local)/"vulkan-game"/"network").string();
    const auto appdata=env("APPDATA");
    if(!appdata.empty())return (fs::path(appdata)/"vulkan-game"/"network").string();
    const auto home=env("HOME");
    if(!home.empty())return (fs::path(home)/".config"/"vulkan-game"/"network").string();
    throw std::runtime_error("Cannot locate per-user DTLS storage; set XDG_CONFIG_HOME or HOME");
}
inline net::secure::settings load(net::secure::role side,
                                  const std::string& server_host={},
                                  std::uint16_t server_port=0) {
    net::secure::settings s;
    s.storage_directory=storage_directory();
    if(side==net::secure::role::client) {
        if(server_host.empty() || !server_port)
            throw std::invalid_argument("Client needs --host and --port");
        s.max_peers=1;
        s.trust_identity=server_host+":"+std::to_string(server_port);
    }
    return s;
}
}
