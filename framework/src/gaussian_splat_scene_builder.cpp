/**
 * @file gaussian_splat_scene_builder.cpp
 * @brief GaussianSplatSceneBuilder implementation.
 */

#include <himalaya/framework/gaussian_splat_scene_builder.h>

#include <glm/gtc/quaternion.hpp>
#include <glm/mat3x3.hpp>
#include <glm/vec3.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

namespace himalaya::framework {
    namespace {
        /** @brief Minimum accepted positive node-transform column scale. */
        constexpr float kTransformScaleEpsilon = 1.0e-8f;

        /** @brief Maximum allowed absolute dot product between normalized node axes. */
        constexpr float kTransformOrthonormalTolerance = 1.0e-3f;

        /** @brief Maximum allowed deviation from a proper-rotation determinant of +1. */
        constexpr float kTransformDeterminantTolerance = 1.0e-3f;

        /** @brief Maximum accepted deviation from a unit per-splat quaternion. */
        constexpr float kUnitQuaternionTolerance = 1.0e-3f;

        /** @brief Mathematical pi constant used by the symmetric eigenvalue solver. */
        constexpr double kPi = 3.141592653589793238462643383279502884;

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

        /** @brief Returns true when every component is finite. */
        bool is_finite_vec4(const glm::vec4 &value) {
            return std::isfinite(value.x) && std::isfinite(value.y)
                   && std::isfinite(value.z) && std::isfinite(value.w);
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

        /** @brief Validates one per-splat unit quaternion and returns it as a GLM quaternion. */
        glm::quat validate_splat_rotation(const glm::vec4 &rotation,
                                          const size_t primitive_index,
                                          const size_t splat_index) {
            if (!is_finite_vec4(rotation)) {
                throw std::runtime_error("GS primitive " + std::to_string(primitive_index)
                                         + " has non-finite ROTATION at splat "
                                         + std::to_string(splat_index));
            }

            const float length_sq = rotation.x * rotation.x
                                    + rotation.y * rotation.y
                                    + rotation.z * rotation.z
                                    + rotation.w * rotation.w;
            if (!std::isfinite(length_sq) || std::abs(length_sq - 1.0f) > kUnitQuaternionTolerance) {
                throw std::runtime_error("GS primitive " + std::to_string(primitive_index)
                                         + " has non-unit ROTATION at splat "
                                         + std::to_string(splat_index));
            }

            return {rotation.w, rotation.x, rotation.y, rotation.z};
        }

        /** @brief Validates one non-negative per-splat sigma scale. */
        glm::vec3 validate_splat_scale(const glm::vec3 &scale,
                                       const size_t primitive_index,
                                       const size_t splat_index) {
            if (!is_finite_vec3(scale) || scale.x < 0.0f || scale.y < 0.0f || scale.z < 0.0f) {
                throw std::runtime_error("GS primitive " + std::to_string(primitive_index)
                                         + " has invalid SCALE at splat "
                                         + std::to_string(splat_index));
            }
            return scale;
        }

        /** @brief Validates one opacity value before packing it with covariance data. */
        float validate_splat_opacity(const float opacity,
                                     const size_t primitive_index,
                                     const size_t splat_index) {
            if (!std::isfinite(opacity) || opacity < 0.0f || opacity > 1.0f) {
                throw std::runtime_error("GS primitive " + std::to_string(primitive_index)
                                         + " has invalid OPACITY at splat "
                                         + std::to_string(splat_index));
            }
            return opacity;
        }

        /** @brief Computes the largest eigenvalue of a symmetric 3x3 matrix. */
        double largest_symmetric_eigenvalue(const double xx,
                                            const double xy,
                                            const double xz,
                                            const double yy,
                                            const double yz,
                                            const double zz) {
            const double p1 = xy * xy + xz * xz + yz * yz;
            if (p1 == 0.0) {
                return std::max(xx, std::max(yy, zz));
            }

            const double q = (xx + yy + zz) / 3.0;
            const double dx = xx - q;
            const double dy = yy - q;
            const double dz = zz - q;
            const double p2 = dx * dx + dy * dy + dz * dz + 2.0 * p1;
            const double p = std::sqrt(p2 / 6.0);
            if (p == 0.0) {
                return q;
            }

            const double b00 = dx / p;
            const double b01 = xy / p;
            const double b02 = xz / p;
            const double b11 = dy / p;
            const double b12 = yz / p;
            const double b22 = dz / p;
            const double det_b = b00 * (b11 * b22 - b12 * b12)
                                 - b01 * (b01 * b22 - b12 * b02)
                                 + b02 * (b01 * b12 - b11 * b02);
            const double r = det_b * 0.5;

            double phi = 0.0;
            if (r <= -1.0) {
                phi = kPi / 3.0;
            } else if (r >= 1.0) {
                phi = 0.0;
            } else {
                phi = std::acos(r) / 3.0;
            }

            return q + 2.0 * p * std::cos(phi);
        }

        /** @brief Bakes local splat rotation/scale through the node transform into world covariance. */
        GaussianSplatCovarianceOpacity bake_covariance_opacity(const ValidatedNodeTransform &node_transform,
                                                               const glm::vec4 &rotation,
                                                               const glm::vec3 &scale,
                                                               const float opacity,
                                                               const size_t primitive_index,
                                                               const size_t splat_index,
                                                               float &world_radius_3sigma) {
            const glm::quat quaternion = validate_splat_rotation(rotation, primitive_index, splat_index);
            const glm::vec3 sigma = validate_splat_scale(scale, primitive_index, splat_index);
            const float validated_opacity = validate_splat_opacity(opacity, primitive_index, splat_index);

            glm::mat3 sigma_squared(0.0f);
            sigma_squared[0][0] = sigma.x * sigma.x;
            sigma_squared[1][1] = sigma.y * sigma.y;
            sigma_squared[2][2] = sigma.z * sigma.z;

            const glm::mat3 local_rotation = glm::mat3_cast(quaternion);
            const glm::mat3 covariance_local = local_rotation * sigma_squared * glm::transpose(local_rotation);
            const glm::mat3 covariance_world = node_transform.linear
                                               * covariance_local
                                               * glm::transpose(node_transform.linear);

            const double xx = static_cast<double>(covariance_world[0][0]);
            const double xy = 0.5 * (static_cast<double>(covariance_world[1][0])
                                     + static_cast<double>(covariance_world[0][1]));
            const double xz = 0.5 * (static_cast<double>(covariance_world[2][0])
                                     + static_cast<double>(covariance_world[0][2]));
            const double yy = static_cast<double>(covariance_world[1][1]);
            const double yz = 0.5 * (static_cast<double>(covariance_world[2][1])
                                     + static_cast<double>(covariance_world[1][2]));
            const double zz = static_cast<double>(covariance_world[2][2]);

            if (!std::isfinite(xx) || !std::isfinite(xy) || !std::isfinite(xz)
                || !std::isfinite(yy) || !std::isfinite(yz) || !std::isfinite(zz)) {
                throw std::runtime_error("GS primitive " + std::to_string(primitive_index)
                                         + " produced non-finite world covariance at splat "
                                         + std::to_string(splat_index));
            }

            const double lambda_max = largest_symmetric_eigenvalue(xx, xy, xz, yy, yz, zz);
            if (!std::isfinite(lambda_max)) {
                throw std::runtime_error("GS primitive " + std::to_string(primitive_index)
                                         + " produced non-finite covariance eigenvalue at splat "
                                         + std::to_string(splat_index));
            }

            world_radius_3sigma = static_cast<float>(3.0 * std::sqrt(std::max(lambda_max, 0.0)));
            if (!std::isfinite(world_radius_3sigma)) {
                throw std::runtime_error("GS primitive " + std::to_string(primitive_index)
                                         + " produced non-finite cull radius at splat "
                                         + std::to_string(splat_index));
            }

            return {
                .covariance0 = glm::vec4(
                    static_cast<float>(xx),
                    static_cast<float>(xy),
                    static_cast<float>(xz),
                    static_cast<float>(yy)),
                .covariance1_opacity = glm::vec4(
                    static_cast<float>(yz),
                    static_cast<float>(zz),
                    validated_opacity,
                    0.0f),
            };
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
            gpu_scene_.metadata = scene.metadata;

            baked_position_radius_.reserve(scene.total_splat_count);
            baked_covariance_opacity_.reserve(scene.total_splat_count);

            for (size_t primitive_index = 0; primitive_index < scene.primitives.size(); ++primitive_index) {
                const auto &primitive = scene.primitives[primitive_index];
                const auto node_transform = validate_node_transform(primitive.transform, primitive_index);

                if (primitive.positions.size() != primitive.splat_count
                    || primitive.rotations.size() != primitive.splat_count
                    || primitive.scales.size() != primitive.splat_count
                    || primitive.opacities.size() != primitive.splat_count) {
                    throw std::runtime_error("GS primitive " + std::to_string(primitive_index)
                                             + " attribute counts do not match splat_count");
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

                    float world_radius_3sigma = 0.0f;
                    baked_covariance_opacity_.push_back(bake_covariance_opacity(
                        node_transform,
                        primitive.rotations[splat_index],
                        primitive.scales[splat_index],
                        primitive.opacities[splat_index],
                        primitive_index,
                        splat_index,
                        world_radius_3sigma));

                    baked_position_radius_.push_back({
                        .position_radius = glm::vec4(world_position, world_radius_3sigma),
                    });
                }
            }

            if (baked_position_radius_.size() != scene.total_splat_count
                || baked_covariance_opacity_.size() != scene.total_splat_count) {
                throw std::runtime_error("GS baked static attribute count does not match total_splat_count");
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
        baked_covariance_opacity_.clear();
        valid_ = false;
    }

    bool GaussianSplatSceneBuilder::valid() const {
        return valid_;
    }

    const GaussianSplatGpuScene &GaussianSplatSceneBuilder::gpu_scene() const {
        return gpu_scene_;
    }
} // namespace himalaya::framework
