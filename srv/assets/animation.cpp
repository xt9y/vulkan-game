#include "animation.hpp"

#include <ozz/animation/runtime/local_to_model_job.h>
#include <ozz/base/io/archive.h>
#include <ozz/base/io/stream.h>

#include <cmath>
#include <stdexcept>

namespace srv::assets
{
    namespace
    {
        template <typename T>
        void read(const std::filesystem::path& path, T& value)
        {
            ozz::io::File file(path.string().c_str(), "rb");
            if (!file.opened())
                throw std::runtime_error("Cannot open Ozz file: " + path.string());
            ozz::io::IArchive archive(&file);
            if (!archive.TestTag<T>())
                throw std::runtime_error("Wrong Ozz asset type: " + path.string());
            archive >> value;
        }
    } // namespace

    void animation::load(const std::filesystem::path& skeleton_file,
                         const std::filesystem::path& animation_file)
    {
        read(skeleton_file, skeleton);
        read(animation_file, clip);
        if (skeleton.num_joints() != clip.num_tracks())
            throw std::runtime_error("Ozz skeleton and clip have different joint counts");
        context.Resize(skeleton.num_joints());
        locals.resize(skeleton.num_soa_joints());
        matrices.resize(skeleton.num_joints());
    }

    bool animation::sample(float seconds)
    {
        if (locals.empty() || !std::isfinite(seconds) || clip.duration() <= 0)
            return false;
        auto time = std::fmod(seconds, clip.duration());
        if (time < 0) time += clip.duration();
        ozz::animation::SamplingJob sampler;
        sampler.animation = &clip;
        sampler.context = &context;
        sampler.ratio = time / clip.duration();
        sampler.output = ozz::make_span(locals);
        if (!sampler.Run()) return false;

        ozz::animation::LocalToModelJob joints;
        joints.skeleton = &skeleton;
        joints.input = ozz::make_span(locals);
        joints.output = ozz::make_span(matrices);
        return joints.Run();
    }
} // namespace srv::assets
