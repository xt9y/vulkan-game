#pragma once

#include <vk/vk.hpp>

#include <cstddef>
#include <stdexcept>

namespace clt::upload
{
    // Static geometry: upload once, then keep it in device-local memory.
    // Scene synchronization already runs after the renderer's frame fence.
    inline vk::buffer::resource static_buffer(
        const void* data,
        std::size_t bytes,
        vk::BufferUsageFlags usage
    )
    {
        if (!data || !bytes)
        {
            throw std::invalid_argument("Invalid static mesh upload");
        }

        vk::buffer::resource staging;
        vk::buffer::resource destination;
        vk::CommandBuffer command{};
        bool submitted = false;

        try
        {
            staging = vk::buffer::create(
                bytes,
                vk::BufferUsageFlagBits::eTransferSrc,
                vk::MemoryPropertyFlagBits::eHostVisible |
                vk::MemoryPropertyFlagBits::eHostCoherent
            );

            destination = vk::buffer::create(
                bytes,
                usage | vk::BufferUsageFlagBits::eTransferDst,
                vk::MemoryPropertyFlagBits::eDeviceLocal
            );

            vk::buffer::write(staging, data, bytes);

            command = vk::command::begin_one_time();

            vk::BufferCopy copy{};
            copy.size = bytes;
            command.copyBuffer(staging.handle, destination.handle, {copy});
            command.end();

            vk::SubmitInfo submit{};
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &command;

            const auto queue = vk::queue::get()->graphics;
            vk::submit::queue(queue, {submit});
            submitted = true;
            vk::submit::wait(queue);

            vk::command::free({command});
            command = {};
            vk::buffer::destroy(staging);
            return destination;
        }
        catch (...)
        {
            if (submitted)
            {
                try
                {
                    vk::submit::wait(vk::queue::get()->graphics);
                }
                catch (...)
                {
                    // The caller handles fatal device errors.
                }
            }

            if (command)
            {
                vk::command::free({command});
            }

            if (staging.handle)
            {
                vk::buffer::destroy(staging);
            }

            if (destination.handle)
            {
                vk::buffer::destroy(destination);
            }

            throw;
        }
    }
} // namespace clt::upload
