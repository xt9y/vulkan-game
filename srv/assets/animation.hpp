#pragma once

#include <ozz/animation/runtime/animation.h>
#include <ozz/animation/runtime/skeleton.h>
#include <ozz/animation/runtime/sampling_job.h>
#include <ozz/base/maths/simd_math.h>
#include <ozz/base/maths/soa_transform.h>

#include <filesystem>
#include <vector>

namespace srv::assets
{
    // One animation track and its reference skeleton. Import/conversion to
    // .ozz is an offline operation; sampling is CPU-only and thread-confined.
    struct animation
    {
        ozz::animation::Skeleton skeleton;
        ozz::animation::Animation clip;
        ozz::animation::SamplingJob::Context context;
        std::vector<ozz::math::SoaTransform> locals;
        std::vector<ozz::math::Float4x4> matrices;

        void load(const std::filesystem::path& skeleton_file,
                  const std::filesystem::path& animation_file);
        bool sample(float seconds);
    };
} // namespace srv::assets
