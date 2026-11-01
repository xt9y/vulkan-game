#include "application.hpp"

#include "clt/app/application.hpp"
#include "options.hpp"
#include "srv/app/application.hpp"
#include "srv/shared/net/socket.hpp"

#include <stdexcept>

namespace game::application
{
    namespace
    {
        void run_server()
        {
            try
            {
                srv::application::run();
            }
            catch (...)
            {
                get()->stop.store(true, std::memory_order_relaxed);
                throw;
            }
        }

        int run_combined()
        {
            // Bind before launching the worker or initializing client graphics.
            srv::application::init();
            get()->server_worker = std::async(std::launch::async, run_server);

            clt::application::init();
            const int result = clt::application::run();

            get()->stop.store(true, std::memory_order_relaxed);
            get()->server_worker.get();

            return result;
        }
    } // namespace

    void init()
    {
        if (get()->initialized)
        {
            throw std::runtime_error("game::application already initialized");
        }

        get()->stop.store(false, std::memory_order_relaxed);
        net::socket::init();
        get()->initialized = true;
    }

    int run()
    {
        if (!get()->initialized)
        {
            throw std::runtime_error("game::application is not initialized");
        }

        switch (options::get()->mode)
        {
            case options::mode::server_only:
                srv::application::init();
                return srv::application::run();

            case options::mode::client_only:
                clt::application::init();
                return clt::application::run();

            case options::mode::server_and_client:
                return run_combined();
        }

        return 1;
    }

    void shutdown() noexcept
    {
        if (!get()->initialized)
        {
            return;
        }

        get()->stop.store(true, std::memory_order_relaxed);
        clt::application::shutdown();

        if (get()->server_worker.valid())
        {
            get()->server_worker.wait();
            get()->server_worker = {};
        }

        srv::application::shutdown();
        net::socket::shutdown();
        get()->initialized = false;
    }
} // namespace game::application
