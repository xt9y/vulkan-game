#pragma once

#include <vk/vk.hpp>

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <vector>

namespace clt::upload
{
    // Accumulate immutable mesh copies into one submission and retire staging
    // allocations only after its fence signals. Never idle the entire queue.
    class batch
    {
        vk::CommandBuffer command{};
        std::vector<vk::buffer::resource> staging;
        vk::Fence fence{};
        bool submitted = false;

    public:
        batch() = default;
        batch(const batch&) = delete;
        batch& operator=(const batch&) = delete;

        vk::buffer::resource add(const void* data, std::size_t bytes,
                                 vk::BufferUsageFlags usage,
                                 vk::DeviceSize capacity = 0)
        {
            if (!data || !bytes)
                throw std::invalid_argument("Invalid static mesh upload");

            vk::buffer::resource source{};
            vk::buffer::resource destination{};
            try
            {
                source = vk::buffer::create(
                    bytes, vk::BufferUsageFlagBits::eTransferSrc,
                    vk::MemoryPropertyFlagBits::eHostVisible |
                    vk::MemoryPropertyFlagBits::eHostCoherent
                );
                vk::buffer::write(source, data, bytes);
                destination = vk::buffer::create(
                    std::max<vk::DeviceSize>(bytes, capacity),
                    usage | vk::BufferUsageFlagBits::eTransferDst,
                    vk::MemoryPropertyFlagBits::eDeviceLocal
                );
                if (!command) command = vk::command::begin_one_time();

                vk::BufferCopy copy{};
                copy.size = bytes;
                command.copyBuffer(source.handle, destination.handle, {copy});
                staging.push_back(source);
                return destination;
            }
            catch (...)
            {
                if (source.handle) vk::buffer::destroy(source);
                if (destination.handle) vk::buffer::destroy(destination);
                throw;
            }
        }

        void write(vk::buffer::resource& target, const void* data,
                   std::size_t bytes, std::size_t offset = 0)
        {
            if (!data || !bytes || offset > target.size || bytes > target.size - offset)
            {
                throw std::invalid_argument("Invalid staged buffer update");
            }

            auto source = vk::buffer::create(
                bytes, vk::BufferUsageFlagBits::eTransferSrc,
                vk::MemoryPropertyFlagBits::eHostVisible |
                vk::MemoryPropertyFlagBits::eHostCoherent
            );
            try
            {
                vk::buffer::write(source, data, bytes);
                if (!command) command = vk::command::begin_one_time();
                vk::BufferCopy copy{};
                copy.size = bytes;
                copy.dstOffset = offset;
                command.copyBuffer(source.handle, target.handle, {copy});
                staging.push_back(source);
            }
            catch (...)
            {
                vk::buffer::destroy(source);
                throw;
            }
        }

        void finish()
        {
            if (!command) return;
            auto device = vk::device::get()->handle;

            command.end();
            vk::FenceCreateInfo info{};
            fence = device.createFence(info);

            vk::SubmitInfo submit{};
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &command;
            vk::submit::queue(vk::queue::get()->graphics, {submit}, fence);
            submitted = true;
            if (device.waitForFences(fence, true, UINT64_MAX) != vk::Result::eSuccess)
                throw std::runtime_error("Mesh upload fence failed");

            release();
        }

        void release()
        {
            if (command)
            {
                vk::command::free({command});
                command = nullptr;
            }
            for (auto& source : staging)
                vk::buffer::destroy(source);
            staging.clear();

            if (fence)
            {
                vk::device::get()->handle.destroyFence(fence);
                fence = nullptr;
            }
            submitted = false;
        }

        ~batch()
        {
            // The caller must call finish() for pending copies. The destructor
            // releases unsubmitted work during exception unwinding.
            if (submitted && fence)
            {
                try
                {
                    (void)vk::device::get()->handle.waitForFences(fence, true, UINT64_MAX);
                }
                catch (...)
                {
                }
            }
            release();
        }
    };
}
