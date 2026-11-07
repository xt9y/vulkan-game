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
    if(side==net::secure::role::client)s.max_peers=1;

    const auto ca=env("VULKAN_NET_CA");
    const auto cert=env("VULKAN_NET_CERT");
    const auto key=env("VULKAN_NET_KEY");
    if(!ca.empty() || !cert.empty() || !key.empty()) {
        // Explicit strict mutual-TLS deployments retain the original behavior.
        if(ca.empty() || cert.empty() || key.empty())
            throw std::runtime_error("Explicit DTLS mode requires all CA/CERT/KEY variables");
        s.ca_file=ca;
        s.certificate_file=cert;
        s.private_key_file=key;
        if(side==net::secure::role::client) {
            const auto name=env("VULKAN_NET_SERVER_NAME");
            s.server_name=name.empty()?server_host:name;
        }
        return s;
    }

    // Zero-config: server generates a persistent EC key; client pins it on
    // first contact and fails closed if the same host:port later changes key.
    s.automatic_identity=true;
    s.storage_directory=storage_directory();
    if(side==net::secure::role::client) {
        if(server_host.empty() || !server_port)
            throw std::invalid_argument("Client needs --host and --port");
        s.trust_identity=server_host+":"+std::to_string(server_port);
    }
    return s;
}
}
