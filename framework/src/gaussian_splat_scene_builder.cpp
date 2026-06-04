/**
 * @file gaussian_splat_scene_builder.cpp
 * @brief GaussianSplatSceneBuilder implementation.
 */

#include <himalaya/framework/gaussian_splat_scene_builder.h>

#include <glm/mat3x3.hpp>
#include <glm/vec3.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace himalaya::framework {
    namespace {
        /** @brief Minimum accepted positive node-transform column scale. */
        constexpr float kTransformScaleEpsilon = 1.0e-8f;

        /** @brief Maximum allowed absolute dot product between normalized node axes. */
        constexpr float kTransformOrthonormalTolerance = 1.0e-3f;

        /** @brief Maximum allowed deviation from a proper-rotation determinant of +1. */
        constexpr float kTransformDeterminantTolerance = 1.0e-3f;

        /** @brief Validated decomposition data for a GS node transform. */
        struct ValidatedNodeTransform {
            /** @brief Original upper-left 3x3 linear transform. */
            glm::mat3 linear{1.0f};

            /** @brief Proper rotation extracted by normalizing linear columns. */
            glm::mat3 rotation{1.0f};

            /** @brief Positive per-axis scale extracted from linear column lengths. */
            glm::vec3 scale{1.0f};
        };

        /** @brief Returns true when every component is finite. */
        bool is_finite_vec3(const glm::vec3 &value) {
            return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
        }

        /** @brief Returns true when every relevant affine transform component is finite. */
        bool is_finite_affine_transform(const glm::mat4 &transform) {
            for (int col = 0; col < 4; ++col) {
                for (int row = 0; row < 3; ++row) {
                    if (!std::isfinite(transform[col][row])) {
                        return false;
                    }
                }
            }
            return true;
        }

        /** @brief Computes the next power-of-two capacity for GS sort buffers. */
        uint32_t next_power_of_two(const uint32_t value) {
            if (value <= 1) {
                return 1;
            }

            uint64_t capacity = 1;
            while (capacity < value) {
                capacity <<= 1;
            }
            if (capacity > std::numeric_limits<uint32_t>::max()) {
                throw std::runtime_error("GS sort capacity exceeds uint32_t range");
            }
            return static_cast<uint32_t>(capacity);
        }

        /**
         * @brief Validates that a node transform is decomposable into T * R * S.
         *
         * The KHR ellipse kernel leaves non-decomposable transforms undefined.
         * The renderer rejects them before baking so invalid scenes do not
         * produce silently wrong splat positions, covariance, or SH orientation.
         */
        ValidatedNodeTransform validate_node_transform(const glm::mat4 &transform,
                                                       const size_t primitive_index) {
            if (!is_finite_affine_transform(transform)) {
                throw std::runtime_error("GS primitive " + std::to_string(primitive_index)
                                         + " has non-finite node transform");
            }

            const glm::vec3 translation(transform[3]);
            if (!is_finite_vec3(translation)) {
                throw std::runtime_error("GS primitive " + std::to_string(primitive_index)
                                         + " has non-finite node translation");
            }

            ValidatedNodeTransform result{};
            result.linear = glm::mat3(transform);

            const glm::vec3 col0(result.linear[0]);
            const glm::vec3 col1(result.linear[1]);
            const glm::vec3 col2(result.linear[2]);
            result.scale = {glm::length(col0), glm::length(col1), glm::length(col2)};

            if (!is_finite_vec3(result.scale)
                || result.scale.x <= kTransformScaleEpsilon
                || result.scale.y <= kTransformScaleEpsilon
                || result.scale.z <= kTransformScaleEpsilon) {
                throw std::runtime_error("GS primitive " + std::to_string(primitive_index)
                                         + " has invalid node scale; expected finite positive columns");
            }

            const glm::vec3 axis0 = col0 / result.scale.x;
            const glm::vec3 axis1 = col1 / result.scale.y;
            const glm::vec3 axis2 = col2 / result.scale.z;

            const float dot01 = std::abs(glm::dot(axis0, axis1));
            const float dot02 = std::abs(glm::dot(axis0, axis2));
            const float dot12 = std::abs(glm::dot(axis1, axis2));
            if (dot01 > kTransformOrthonormalTolerance
                || dot02 > kTransformOrthonormalTolerance
                || dot12 > kTransformOrthonormalTolerance) {
                throw std::runtime_error("GS primitive " + std::to_string(primitive_index)
                                         + " has shear or non-orthogonal node axes");
            }

            result.rotation = glm::mat3(axis0, axis1, axis2);
            const float determinant = glm::determinant(result.rotation);
            if (!std::isfinite(determinant)
                || std::abs(determinant - 1.0f) > kTransformDeterminantTolerance) {
                throw std::runtime_error("GS primitive " + std::to_string(primitive_index)
                                         + " has reflection, negative determinant, or invalid node rotation");
            }

            return result;
        }
    } // namespace

    bool GaussianSplatSceneBuilder::build(const GaussianSplatScene &scene,
                                          std::string &error_message) {
        destroy();
        error_message.clear();

        try {
            if (scene.total_splat_count == 0) {
                throw std::runtime_error("GS scene contains no splats");
            }

            gpu_scene_.total_splat_count = scene.total_splat_count;
            gpu_scene_.sort_capacity = next_power_of_two(scene.total_splat_count);
            gpu_scene_.primitive_ranges = scene.primitive_ranges;
            gpu_scene_.metadata = scene.metadata;

            baked_position_radius_.reserve(scene.total_splat_count);

            for (size_t primitive_index = 0; primitive_index < scene.primitives.size(); ++primitive_index) {
                const auto &primitive = scene.primitives[primitive_index];
                validate_node_transform(primitive.transform, primitive_index);

                if (primitive.positions.size() != primitive.splat_count) {
                    throw std::runtime_error("GS primitive " + std::to_string(primitive_index)
                                             + " position count does not match splat_count");
                }

                for (size_t splat_index = 0; splat_index < primitive.positions.size(); ++splat_index) {
                    const glm::vec3 local_position = primitive.positions[splat_index];
                    if (!is_finite_vec3(local_position)) {
                        throw std::runtime_error("GS primitive " + std::to_string(primitive_index)
                                                 + " has non-finite local position at splat "
                                                 + std::to_string(splat_index));
                    }

                    const glm::vec3 world_position = glm::vec3(
                        primitive.transform * glm::vec4(local_position, 1.0f));
                    if (!is_finite_vec3(world_position)) {
                        throw std::runtime_error("GS primitive " + std::to_string(primitive_index)
                                                 + " produced non-finite world position at splat "
                                                 + std::to_string(splat_index));
                    }

                    baked_position_radius_.push_back({
                        .position_radius = glm::vec4(world_position, 0.0f),
                    });
                }
            }

            if (baked_position_radius_.size() != scene.total_splat_count) {
                throw std::runtime_error("GS baked position count does not match total_splat_count");
            }

            valid_ = true;
            return true;
        } catch (const std::exception &e) {
            destroy();
            error_message = e.what();
            return false;
        }
    }

    void GaussianSplatSceneBuilder::destroy() {
        gpu_scene_ = {};
        baked_position_radius_.clear();
        valid_ = false;
    }

    bool GaussianSplatSceneBuilder::valid() const {
        return valid_;
    }

    const GaussianSplatGpuScene &GaussianSplatSceneBuilder::gpu_scene() const {
        return gpu_scene_;
    }
} // namespace himalaya::framework
