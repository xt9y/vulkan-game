#include "animation.hpp"

#include <ozz/animation/runtime/animation.h>
#include <ozz/animation/runtime/local_to_model_job.h>
#include <ozz/animation/runtime/sampling_job.h>
#include <ozz/animation/runtime/skeleton.h>
#include <ozz/base/io/archive.h>
#include <ozz/base/io/stream.h>
#include <ozz/base/maths/simd_math.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <stdexcept>

namespace clt::animation
{
    namespace
    {
        struct runtime
        {
            std::uint32_t id = 0;
            ozz::animation::Skeleton skeleton;
            std::vector<ozz::animation::Animation> clips;
            ozz::animation::SamplingJob::Context context;
            std::vector<ozz::math::SoaTransform> locals;
            std::vector<ozz::math::Float4x4> models;
            std::vector<matrix> inverse_bind;
        };

        std::vector<std::unique_ptr<runtime>>& skins()
        {
            static std::vector<std::unique_ptr<runtime>> storage;
            return storage;
        }

        template<class T>
        void read(const std::vector<std::uint8_t>& bytes, T& output)
        {
            if (bytes.empty()) throw std::runtime_error("Empty Ozz archive");
            ozz::io::MemoryStream stream;
            if (stream.Write(bytes.data(), bytes.size()) != bytes.size() ||
                stream.Seek(0, ozz::io::Stream::kSet))
                throw std::runtime_error("Invalid Ozz archive stream");
            ozz::io::IArchive archive(&stream);
            if (!archive.TestTag<T>())
                throw std::runtime_error("Ozz archive tag mismatch");
            archive >> output;
        }

        runtime* find(std::uint32_t id)
        {
            for (auto& item : skins())
                if (item->id == id) return item.get();
            return nullptr;
        }

        matrix multiply(const matrix& lhs, const matrix& rhs)
        {
            matrix result{};
            for (int column = 0; column < 4; ++column)
                for (int row = 0; row < 4; ++row)
                    for (int k = 0; k < 4; ++k)
                        result[column * 4 + row] +=
                            lhs[k * 4 + row] * rhs[column * 4 + k];
            return result;
        }
    } // namespace

    void synchronize(const ::scene::state& scene)
    {
        for (const auto& incoming : scene.skins)
        {
            if (find(incoming.id)) continue;
            auto item = std::make_unique<runtime>();
            item->id = incoming.id;
            read(incoming.skeleton, item->skeleton);
            if (item->skeleton.num_joints() == 0 ||
                incoming.inverse_bind.size() != std::size_t(item->skeleton.num_joints()))
                throw std::runtime_error("Invalid streamed skin joint count");
            item->inverse_bind = incoming.inverse_bind;
            item->clips.resize(incoming.clips.size());
            for (std::size_t i = 0; i < incoming.clips.size(); ++i)
            {
                read(incoming.clips[i].bytes, item->clips[i]);
                if (item->clips[i].num_tracks() != item->skeleton.num_joints())
                    throw std::runtime_error("Ozz clip does not match skin skeleton");
            }
            item->context.Resize(item->skeleton.num_joints());
            item->locals.resize(item->skeleton.num_soa_joints());
            item->models.resize(item->skeleton.num_joints());
            skins().push_back(std::move(item));
        }
    }

    std::uint32_t joints(std::uint32_t skin_id)
    {
        const auto* item = find(skin_id);
        return item ? std::uint32_t(item->skeleton.num_joints()) : 0;
    }

    bool sample(std::uint32_t skin_id, std::uint32_t clip,
                float seconds, std::vector<matrix>& palette)
    {
        auto* item = find(skin_id);
        if (!item || !std::isfinite(seconds)) return false;
        if (!item->clips.empty())
        {
            if (clip >= item->clips.size()) return false;
            const auto& animation = item->clips[clip];
            if (animation.duration() <= 0) return false;
            float t = std::fmod(seconds, animation.duration());
            if (t < 0) t += animation.duration();
            ozz::animation::SamplingJob job;
            job.animation = &animation;
            job.context = &item->context;
            job.ratio = t / animation.duration();
            job.output = ozz::make_span(item->locals);
            if (!job.Run()) return false;
        }
        else
        {
            std::copy(item->skeleton.joint_rest_poses().begin(),
                      item->skeleton.joint_rest_poses().end(), item->locals.begin());
        }

        ozz::animation::LocalToModelJob job;
        job.skeleton = &item->skeleton;
        job.input = ozz::make_span(item->locals);
        job.output = ozz::make_span(item->models);
        if (!job.Run()) return false;

        palette.resize(item->models.size());
        for (std::size_t i = 0; i < item->models.size(); ++i)
        {
            matrix world{};
            for (int c = 0; c < 4; ++c)
                ozz::math::StorePtrU(item->models[i].cols[c], world.data() + c * 4);
            palette[i] = multiply(world, item->inverse_bind[i]);
        }
        return true;
    }

    void reset()
    {
        skins().clear();
    }
} // namespace clt::animation
