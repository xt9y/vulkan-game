#pragma once
#include <network/secure.hpp>
#include <cstdlib>
#include <stdexcept>
#include <string>

namespace game::secure_config {
// Every build fails closed when credentials are missing. Provision outside git.
inline std::string required(const char* key) {
    const char* value=std::getenv(key);
    if(!value||!*value)
        throw std::runtime_error(std::string("Missing DTLS credential: ")+key);
    return value;
}
inline net::secure::settings load(net::secure::role side,
                                  const std::string& server_host={}) {
    net::secure::settings s;
    s.ca_file=required("VULKAN_NET_CA");
    s.certificate_file=required("VULKAN_NET_CERT");
    s.private_key_file=required("VULKAN_NET_KEY");
    if(side==net::secure::role::client) {
        s.max_peers=1;
        const char* override_name=std::getenv("VULKAN_NET_SERVER_NAME");
        s.server_name=override_name&&*override_name ?
            override_name:server_host;
    }
    return s;
}
}
