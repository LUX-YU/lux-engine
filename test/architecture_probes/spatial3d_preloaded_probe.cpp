#include "DeviceRenderFixture.hpp"

#include <lux/engine/function/render/client/genops/ForwardMeshOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/LightOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/MaterialOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/MeshShadowOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/MeshStackOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/RenderClusterOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/ShadowMapOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/ViewCameraOperation.ops.hpp>
#include <lux/engine/function/render/client/resources/material/GraphMaterialData.hpp>
#include <lux/engine/description/Mesh.hpp>
#include <lux/engine/description/ShaderInfo.hpp>
#include <lux/engine/scene/LatestSpscExchange.hpp>
#include <lux/engine/scene/Scene.hpp>
#include <lux/engine/process/world/WorldPartitionLoadSender.hpp>
#include <lux/engine/scene/WorldMaterializer.hpp>
#include <lux/engine/serialization/BinaryReader.hpp>
#include <lux/engine/serialization/BinaryWriter.hpp>
#include <lux/engine/serialization/Serialization.hpp>
#include <lux/engine/serialization/external_support/Eigen.hpp>
#include <lux/engine/simulation/SimulationBuilder.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/simulation/ecs/ComponentSchemaSet.hpp>
#include <lux/engine/simulation/ecs/Transform.hpp>
#include <lux/engine/simulation/ecs/TransformSchema.hpp>
#include <lux/engine/world/WorldDescriptionBuilder.hpp>
#include <lux/engine/world/storage/detail/WorldStorageCodec.hpp>

#include <Jolt/Jolt.h>

#ifndef JPH_DOUBLE_PRECISION
#error "Lux Physics3D requires double-position Jolt"
#endif

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <Eigen/Core>
#include <Eigen/Geometry>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <stdexec/execution.hpp>
#include <string_view>
#include <utility>
#include <vector>

static_assert(std::same_as<JPH::Real, double>);
static_assert(std::same_as<JPH::RVec3, JPH::DVec3>);

namespace
{
    using namespace lux::simulation;
    using namespace lux::simulation::ecs;
    using namespace lux::domain;
    using namespace lux::partition;
    using namespace lux::world;
    using namespace lux::world::detail;

    inline constexpr std::string_view RenderableSchemaName = "lux.test.spatial3d.PreloadedRenderable";
    inline constexpr SystemInstanceId SpatialSystemInstance{1U};

    template <class Type>
    [[nodiscard]] Type id(std::uint8_t tail)
    {
        std::array<std::uint8_t, 16U> bytes{};
        bytes.back() = tail;
        return Type{uuids::uuid(bytes)};
    }

    struct PreloadedRenderable final
    {
        std::uint32_t mesh{};
        std::uint32_t material{};
        std::uint32_t texture{};
    };

    [[nodiscard]] ComponentDecodeFailure decodeFailure(
        EComponentDecodeError code,
        std::size_t offset = 0U
    ) noexcept
    {
        return ComponentDecodeFailure{code, offset};
    }

    lux::cxx::expected<void, ComponentDecodeFailure> decodePreloadedRenderable(
        Registry& registry,
        Entity entity,
        std::uint32_t version,
        std::span<const std::byte> payload
    ) noexcept
    {
        if (!registry.valid(entity))
            return lux::cxx::unexpected(decodeFailure(EComponentDecodeError::INVALID_ENTITY));
        if (version != 1U)
            return lux::cxx::unexpected(decodeFailure(EComponentDecodeError::UNSUPPORTED_VERSION));

        lux::serialization::BinaryReader reader(payload);
        const lux::serialization::SerializationBudget budget{payload.size(), payload.size(), 8U};
        auto mesh = lux::serialization::read<std::uint32_t>(reader, budget);
        auto material = lux::serialization::read<std::uint32_t>(reader, budget);
        auto texture = lux::serialization::read<std::uint32_t>(reader, budget);
        if (!mesh || !material || !texture || reader.remaining() != 0U)
            return lux::cxx::unexpected(decodeFailure(EComponentDecodeError::MALFORMED_PAYLOAD, reader.offset()));

        try
        {
            registry.emplace_or_replace<PreloadedRenderable>(entity, PreloadedRenderable{*mesh, *material, *texture});
            return {};
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(decodeFailure(EComponentDecodeError::ALLOCATION_FAILURE));
        }
        catch (...)
        {
            return lux::cxx::unexpected(decodeFailure(EComponentDecodeError::COMPONENT_CONSTRUCTION_FAILURE));
        }
    }

    [[nodiscard]] std::vector<std::byte> encodeRenderable(const PreloadedRenderable& value)
    {
        std::vector<std::byte> payload;
        lux::serialization::BinaryWriter writer(payload);
        const lux::serialization::SerializationBudget budget{64U, 64U, 8U};
        if (!lux::serialization::write(writer, value.mesh, budget) ||
            !lux::serialization::write(writer, value.material, budget) ||
            !lux::serialization::write(writer, value.texture, budget))
        {
            throw std::runtime_error("preloaded renderable encode failed");
        }
        return payload;
    }

    [[nodiscard]] std::vector<std::byte> encodeTransform(const Transform3D& value)
    {
        std::vector<std::byte> payload;
        lux::serialization::BinaryWriter writer(payload);
        const lux::serialization::SerializationBudget budget{1024U, 1024U, 16U};
        if (!lux::serialization::write(writer, value, budget))
            throw std::runtime_error("Transform3D encode failed");
        return payload;
    }

    struct CookedWorld final
    {
        std::shared_ptr<const WorldDescription> description;
        std::vector<std::byte> volume;
    };

    [[nodiscard]] CookedWorld makeCookedWorld(const Transform3D& transform, const PreloadedRenderable& renderable)
    {
        const auto transform_payload = encodeTransform(transform);
        const auto renderable_payload = encodeRenderable(renderable);
        const WorldDataSchemaId transform_schema = worldDataSchemaId("lux.ecs.Transform3D");
        const WorldDataSchemaId renderable_schema = worldDataSchemaId(RenderableSchemaName);
        const bool transform_first = WorldDataSchemaIdLess{}(transform_schema, renderable_schema);
        std::array data{
            WorldEncodedDataRecord{transform_first ? 0U : 1U, 1U, transform_payload},
            WorldEncodedDataRecord{transform_first ? 1U : 0U, 1U, renderable_payload}
        };
        std::sort(data.begin(), data.end(), [](const auto& left, const auto& right) {
            return left.schema_ordinal < right.schema_ordinal;
        });
        const std::array objects{
            WorldEncodedObjectRecord{id<WorldObjectId>(3U), data},
            WorldEncodedObjectRecord{id<WorldObjectId>(4U), data}
        };
        auto partition_wire = encodeWorldPartitionData(PartitionOrdinal{0U}, objects);
        if (!partition_wire)
            throw std::runtime_error("World partition encode failed");

        const std::array records{
            WorldPartitionRecord{id<WorldPartitionId>(4U), 0U, 1U}
        };
        const std::array extents{
            WorldPartitionExtent{0U, 1U, 1U}
        };
        auto page_wire = encodeWorldPartitionTablePage(PartitionOrdinal{0U}, records, extents);
        if (!page_wire)
            throw std::runtime_error("World table page encode failed");

        const std::array chunks{
            WorldStorageChunkInput{
                EWorldStorageChunkKind::PARTITION_TABLE_PAGE,
                EWorldStorageCodec::NONE,
                *page_wire
            },
            WorldStorageChunkInput{
                EWorldStorageChunkKind::WORLD_PARTITION_DATA,
                EWorldStorageCodec::NONE,
                *partition_wire
            }
        };
        const WorldBundleId bundle = id<WorldBundleId>(10U);
        const WorldBundleGeneration generation = id<WorldBundleGeneration>(11U);
        auto volume = encodeWorldStorageVolume(bundle, generation, 0U, chunks);
        if (!volume)
            throw std::runtime_error("World sidecar encode failed");

        WorldDescriptionBuilder builder;
        if (!builder.setIdentity(bundle, generation, "preloaded-spatial3d") ||
            !builder.addSchema(worldDataSchemaId("lux.ecs.Transform3D")) ||
            !builder.addSchema(worldDataSchemaId(RenderableSchemaName)) ||
            !builder.setPartitioner({worldPartitionerId("lux.test.spatial3d.index"), 1U}, 1U) ||
            !builder.addStorageVolume({"preloaded.wvol0", 1U, 2U, volume->size()}) ||
            !builder.addPartitionTablePage({PartitionOrdinal{0U}, 1U, {0U, 0U}}))
        {
            throw std::runtime_error("World description build input failed");
        }
        auto description = std::move(builder).build();
        if (!description)
            throw std::runtime_error("World description validation failed");
        return CookedWorld{
            std::make_shared<WorldDescription>(std::move(*description)),
            std::move(*volume)
        };
    }

    class MemoryEndpoint final
        : public lux::async::OperationPort<lux::process::world::ReadWorldStorageRange>::Endpoint
    {
    public:
        explicit MemoryEndpoint(std::vector<std::byte> volume) : volume_(std::move(volume))
        {
        }

        [[nodiscard]] lux::async::SubmitResult submit(
            lux::process::world::ReadWorldStorageRange operation,
            void* state,
            void (*complete)(void*, Outcome&&) noexcept,
            lux::async::SubmitOptions options
        ) noexcept override
        {
            accounting_ok = accounting_ok && operation.size == options.accounted_bytes;
            ++submits;
            const bool invalid_volume = operation.volume != 0U;
            const bool invalid_offset = operation.offset > volume_.size();
            const bool invalid_size = !invalid_offset && operation.size > volume_.size() - operation.offset;
            if (invalid_volume || invalid_offset || invalid_size)
            {
                complete(
                    state,
                    lux::cxx::unexpected(
                        lux::async::OperationFailure<lux::process::world::WorldStorageRuntimeFailure>::domain(
                            {lux::process::world::EWorldStorageRuntimeError::RANGE_OVERFLOW}
                        )
                    )
                );
                return {};
            }

            try
            {
                complete(
                    state,
                    Outcome{lux::cxx::SharedBytes<>::copyOf(
                        std::span<const std::byte>(volume_).subspan(
                            static_cast<std::size_t>(operation.offset),
                            static_cast<std::size_t>(operation.size)
                        )
                    )}
                );
            }
            catch (...)
            {
                complete(
                    state,
                    lux::cxx::unexpected(
                        lux::async::OperationFailure<lux::process::world::WorldStorageRuntimeFailure>::domain(
                            {lux::process::world::EWorldStorageRuntimeError::ALLOCATION_FAILURE}
                        )
                    )
                );
            }
            return {};
        }

        std::size_t submits{};
        bool accounting_ok{true};

    private:
        std::vector<std::byte> volume_;
    };

    struct LoadReceiver final
    {
        using receiver_concept = stdexec::receiver_t;

        void set_value(WorldPartitionData value) && noexcept
        {
            result->emplace(std::move(value));
        }

        void set_error(lux::process::world::WorldStorageRuntimeFailure value) && noexcept
        {
            error->emplace(value);
        }

        void set_stopped() && noexcept
        {
            *stopped = true;
        }

        [[nodiscard]] stdexec::empty_env get_env() const noexcept
        {
            return {};
        }

        std::optional<WorldPartitionData>* result{};
        std::optional<lux::process::world::WorldStorageRuntimeFailure>* error{};
        bool* stopped{};
    };

    [[nodiscard]] WorldPartitionData loadPartition(
        const CookedWorld& cooked,
        const std::shared_ptr<MemoryEndpoint>& endpoint,
        std::stop_token stop
    )
    {
        auto source = lux::process::world::WorldStorageSource::create(
            cooked.description,
            lux::async::OperationPort<lux::process::world::ReadWorldStorageRange>{endpoint}
        );
        if (!source)
            throw std::runtime_error("WorldStorageSource creation failed");

        std::optional<WorldPartitionData> loaded;
        std::optional<lux::process::world::WorldStorageRuntimeFailure> error;
        bool stopped{};
        auto state = stdexec::connect(
            lux::process::world::loadWorldPartition(
                *source,
                PartitionOrdinal{0U},
                cooked.volume.size(),
                stop
            ),
            LoadReceiver{&loaded, &error, &stopped}
        );
        stdexec::start(state);
        if (!loaded || error || stopped)
            throw std::runtime_error("World partition runtime load failed");
        return std::move(*loaded);
    }

    namespace ObjectLayers
    {
        inline constexpr JPH::ObjectLayer STATIC = 0U;
        inline constexpr JPH::ObjectLayer DYNAMIC = 1U;
        inline constexpr JPH::ObjectLayer COUNT = 2U;
    }

    namespace BroadLayers
    {
        inline constexpr JPH::BroadPhaseLayer STATIC{0U};
        inline constexpr JPH::BroadPhaseLayer DYNAMIC{1U};
        inline constexpr JPH::uint COUNT = 2U;
    }

    class BroadLayerInterface final : public JPH::BroadPhaseLayerInterface
    {
    public:
        BroadLayerInterface() noexcept
        {
            layers_[ObjectLayers::STATIC] = BroadLayers::STATIC;
            layers_[ObjectLayers::DYNAMIC] = BroadLayers::DYNAMIC;
        }

        [[nodiscard]] JPH::uint GetNumBroadPhaseLayers() const override
        {
            return BroadLayers::COUNT;
        }

        [[nodiscard]] JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override
        {
            return layers_[layer];
        }

    private:
        JPH::BroadPhaseLayer layers_[ObjectLayers::COUNT];
    };

    class ObjectPairFilter final : public JPH::ObjectLayerPairFilter
    {
    public:
        [[nodiscard]] bool ShouldCollide(JPH::ObjectLayer first, JPH::ObjectLayer second) const override
        {
            return first == ObjectLayers::DYNAMIC || second == ObjectLayers::DYNAMIC;
        }
    };

    class ObjectBroadFilter final : public JPH::ObjectVsBroadPhaseLayerFilter
    {
    public:
        [[nodiscard]] bool ShouldCollide(JPH::ObjectLayer object, JPH::BroadPhaseLayer broad) const override
        {
            return object == ObjectLayers::DYNAMIC || broad == BroadLayers::DYNAMIC;
        }
    };

    class JoltRuntime final
    {
    public:
        JoltRuntime()
        {
            JPH::RegisterDefaultAllocator();
            JPH::Factory::sInstance = new JPH::Factory();
            JPH::RegisterTypes();
        }

        ~JoltRuntime() noexcept
        {
            JPH::UnregisterTypes();
            delete JPH::Factory::sInstance;
            JPH::Factory::sInstance = nullptr;
        }
    };

    struct PhysicsStepResult final
    {
        Eigen::Vector3d absolute;
        bool contact{};
    };

    class PhysicsWorld final
    {
    public:
        explicit PhysicsWorld(const Eigen::Vector3d& dynamic_position)
            : temp_(16U * 1024U * 1024U), jobs_(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, 2)
        {
            physics_.Init(2U, 0U, 64U, 64U, broad_layers_, object_broad_, object_pairs_);
            physics_.SetGravity(JPH::Vec3::sZero());
            auto& bodies = physics_.GetBodyInterface();
            static_body_ = bodies.CreateAndAddBody(
                JPH::BodyCreationSettings{
                    new JPH::BoxShape(JPH::Vec3(0.25F, 0.25F, 0.25F)),
                    JPH::RVec3(1.0e12, 0.0, -5.0),
                    JPH::Quat::sIdentity(),
                    JPH::EMotionType::Static,
                    ObjectLayers::STATIC
                },
                JPH::EActivation::DontActivate
            );
            dynamic_body_ = bodies.CreateAndAddBody(
                JPH::BodyCreationSettings{
                    new JPH::SphereShape(0.25F),
                    JPH::RVec3(dynamic_position.x(), dynamic_position.y(), dynamic_position.z()),
                    JPH::Quat::sIdentity(),
                    JPH::EMotionType::Dynamic,
                    ObjectLayers::DYNAMIC
                },
                JPH::EActivation::Activate
            );
            if (static_body_.IsInvalid() || dynamic_body_.IsInvalid())
                throw std::runtime_error("Jolt body creation failed");
            physics_.OptimizeBroadPhase();
        }

        ~PhysicsWorld() noexcept
        {
            auto& bodies = physics_.GetBodyInterface();
            for (const JPH::BodyID body : {dynamic_body_, static_body_})
            {
                if (!body.IsInvalid())
                {
                    bodies.RemoveBody(body);
                    bodies.DestroyBody(body);
                }
            }
        }

        [[nodiscard]] std::optional<PhysicsStepResult> step() noexcept
        {
            constexpr float Step = 1.0F / 60.0F;
            if (physics_.Update(Step, 1, &temp_, &jobs_) != JPH::EPhysicsUpdateError::None)
                return std::nullopt;
            const JPH::RVec3 position = physics_.GetBodyInterface().GetPosition(dynamic_body_);
            return PhysicsStepResult{
                Eigen::Vector3d{position.GetX(), position.GetY(), position.GetZ()},
                physics_.WereBodiesInContact(static_body_, dynamic_body_)
            };
        }

    private:
        BroadLayerInterface broad_layers_;
        ObjectBroadFilter object_broad_;
        ObjectPairFilter object_pairs_;
        JPH::TempAllocatorImpl temp_;
        JPH::JobSystemThreadPool jobs_;
        JPH::PhysicsSystem physics_;
        JPH::BodyID static_body_;
        JPH::BodyID dynamic_body_;
    };

    struct Spatial3DIndex final
    {
        [[nodiscard]] PartitionOrdinal query(const Eigen::Vector3d& point) const noexcept
        {
            return point.x() >= 1.0e12 - 1.0 && point.x() <= 1.0e12 + 1.0
                ? PartitionOrdinal{0U}
                : PartitionOrdinal{std::numeric_limits<std::uint32_t>::max()};
        }
    };

    struct Spatial3DProbeState final
    {
        bool ran{};
        bool contact{};
        PartitionOrdinal partition{};
        Eigen::Vector3d physics_position{};
        PreloadedRenderable renderable;
    };

    class PreloadedSpatial3DSystem final
    {
    public:
        inline static constexpr auto Access = makeSystemAccessSpec<
            ComponentRead<Transform3D>,
            ComponentRead<PreloadedRenderable>
        >();
        inline static constexpr SystemDescription Description{
            .canonical_name = "lux.test.spatial3d.preloaded-system",
            .version = 1U
        };

        explicit PreloadedSpatial3DSystem(Registry& registry) : registry_(&registry)
        {
            registry.ctx().emplace<Spatial3DProbeState>();
        }

        ~PreloadedSpatial3DSystem() noexcept = default;

        [[nodiscard]] bool run() noexcept
        {
            auto view = registry_->view<const Transform3D, const PreloadedRenderable>();
            if (view.begin() == view.end())
                return false;
            const Entity entity = *view.begin();
            const auto& transform = view.get<const Transform3D>(entity);
            const auto& renderable = view.get<const PreloadedRenderable>(entity);
            const PartitionOrdinal partition = index_.query(transform.translation);
            if (partition.value != 0U)
                return false;

            try
            {
                PhysicsWorld physics(transform.translation);
                auto result = physics.step();
                if (!result || !result->contact)
                    return false;
                auto& state = registry_->ctx().get<Spatial3DProbeState>();
                state.ran = true;
                state.contact = result->contact;
                state.partition = partition;
                state.physics_position = result->absolute;
                state.renderable = renderable;
                return true;
            }
            catch (...)
            {
                return false;
            }
        }

    private:
        Registry* registry_{};
        Spatial3DIndex index_;
    };

    lux::cxx::expected<void, SystemBuildFailure> installSpatial3D(
        SimulationBuilder& builder,
        SimulationSystemView description
    ) noexcept
    {
        auto system = builder.emplaceSystem<PreloadedSpatial3DSystem>(description.instanceId(), builder.registry());
        if (!system)
            return lux::cxx::unexpected(system.error());
        return builder.addSystemTask<PreloadedSpatial3DSystem>(
            description.instanceId(),
            [](PreloadedSpatial3DSystem& value) noexcept { return value.run(); }
        );
    }

    [[nodiscard]] std::shared_ptr<const SimulationDescription> makeSimulationDescription()
    {
        SimulationDescriptionBuilder builder;
        if (!builder.addSystem(
                SpatialSystemInstance,
                PreloadedSpatial3DSystem::Description.canonical_name,
                PreloadedSpatial3DSystem::Description))
        {
            throw std::runtime_error("Spatial3D Simulation description rejected System");
        }
        auto description = std::move(builder).build();
        if (!description)
            throw std::runtime_error("Spatial3D Simulation description build failed");
        return std::make_shared<SimulationDescription>(std::move(*description));
    }

    [[nodiscard]] ComponentSchemaSet makeComponentSchemas()
    {
        std::vector<ComponentSchema> schemas(
            transformComponentSchemas().begin(),
            transformComponentSchemas().end()
        );
        schemas.push_back(makeComponentSchema<PreloadedRenderable>(
            componentSchemaId(RenderableSchemaName),
            1U,
            EComponentSnapshotPolicy::COPY,
            {},
            &decodePreloadedRenderable
        ));
        auto set = ComponentSchemaSet::build(std::move(schemas));
        if (!set)
            throw std::runtime_error("Spatial3D ComponentSchemaSet build failed");
        return std::move(*set);
    }

    struct PresentationState final
    {
        Eigen::Vector3f render_relative{};
        PreloadedRenderable renderable;
        std::uint64_t revision{};
    };

    [[nodiscard]] std::vector<std::uint32_t> loadSpirv(const char* path)
    {
        std::ifstream input(path, std::ios::binary | std::ios::ate);
        if (!input)
            return {};
        const std::streamsize bytes = input.tellg();
        if (bytes <= 0 || bytes % static_cast<std::streamsize>(sizeof(std::uint32_t)) != 0)
            return {};
        std::vector<std::uint32_t> result(static_cast<std::size_t>(bytes) / sizeof(std::uint32_t));
        input.seekg(0, std::ios::beg);
        input.read(reinterpret_cast<char*>(result.data()), bytes);
        return input ? std::move(result) : std::vector<std::uint32_t>{};
    }

    [[nodiscard]] lux::rdesc::Mesh makeTriangle()
    {
        lux::rdesc::Mesh mesh;
        mesh.vertices.resize(3U);
        mesh.vertices[0].position = {-1.0F, -1.0F, 0.0F};
        mesh.vertices[1].position = {1.0F, -1.0F, 0.0F};
        mesh.vertices[2].position = {0.0F, 1.0F, 0.0F};
        for (auto& vertex : mesh.vertices)
        {
            vertex.normal = {0.0F, 0.0F, 1.0F};
            vertex.tangent = {1.0F, 0.0F, 0.0F};
            vertex.bitangent = {0.0F, 1.0F, 0.0F};
        }
        mesh.indices = {0U, 1U, 2U};
        return mesh;
    }

    struct RenderResult final
    {
        bool gpu{};
        bool texture_ready{};
        bool material_ready{};
        bool mesh_ready{};
        bool instance_ready{};
        bool duplicate_instance_ready{};
        bool survived_first_release{};
        bool final_release_empty{};
        bool texture_generation_changed{};
        std::uint32_t alive_after_first_release{};
        std::uint32_t texture_index{};
        std::uint32_t texture_generation{};
        std::uint32_t recreated_texture_index{};
        std::uint32_t recreated_texture_generation{};
        std::uint32_t lit_pixels{};
    };

    [[nodiscard]] RenderResult renderPreloaded(
        const PresentationState& state,
        std::atomic_int& validation_errors
    )
    {
        using namespace lux::render;

        lux::rendertest::DeviceRenderFixture fixture(
            128U,
            128U,
            "architecture_probe_spatial3d_preloaded",
            {.enable_validation = true, .validation_errors = &validation_errors}
        );
        if (!fixture.ok())
            return {};

        RenderResult result{.gpu = true};
        const auto scene = fixture.makeSceneWithView("PreloadedSpatial3D", "PreloadedSpatial3DView");
        const auto fragment = loadSpirv(LUX_SPATIAL3D_FRAGMENT_SPV);
        const auto gbuffer_fragment = loadSpirv(LUX_SPATIAL3D_GBUFFER_FRAGMENT_SPV);
        if (fragment.empty() || gbuffer_fragment.empty())
            throw std::runtime_error("Spatial3D probe shader artifact missing");

        lux::rdesc::ShaderInfo shader_info{};
        shader_info.entry_points.push_back({"main", lux::rdesc::EShaderType::FRAGMENT});
        const auto shader_info_bytes = lux::rdesc::ShaderInfo::serialize(shader_info);
        const auto compile = [&](std::span<const std::uint32_t> spirv) {
            return fixture.awaitControl(fixture.control().compileShader(
                std::span<const std::byte>{
                    reinterpret_cast<const std::byte*>(spirv.data()),
                    spirv.size_bytes()
                },
                shader_info_bytes
            ));
        };
        const auto forward_shader = compile(fragment);
        const auto gbuffer_shader = compile(gbuffer_fragment);
        if (forward_shader.status != 0U || gbuffer_shader.status != 0U)
            throw std::runtime_error("Spatial3D probe shader compile failed");

        const auto camera_registration =
            fixture.awaitControl(fixture.control().registerFeatureType(kViewCameraFeatureFactory));
        const auto camera_feature = fixture.awaitControl(
            fixture.control().addFeature(
                scene.scene_id,
                camera_registration.feature_type_id,
                ViewCameraCommTag{}
            )
        );
        const auto camera_ops = ViewCameraOperationIds::fromOps(
            camera_registration.ops,
            camera_registration.op_count
        );
        if (!camera_feature.feature.isValid() || !camera_ops.valid())
            throw std::runtime_error("Spatial3D camera feature failed");

        const auto material_registration =
            fixture.awaitControl(fixture.control().registerFeatureType(kMaterialFeatureFactory));
        const auto material_feature = fixture.awaitControl(
            fixture.control().addFeature(scene.scene_id, material_registration.feature_type_id, MaterialCommTag{})
        );
        const auto material_ops = MaterialOperationIds::fromOps(
            material_registration.ops,
            material_registration.op_count
        );
        if (!material_feature.feature.isValid() || !material_ops.valid())
            throw std::runtime_error("Spatial3D material feature failed");

        const auto mesh_registration =
            fixture.awaitControl(fixture.control().registerFeatureType(kMeshStackFeatureFactory));
        const auto mesh_feature = fixture.awaitControl(
            fixture.control().addFeature(scene.scene_id, mesh_registration.feature_type_id, MeshStackCommTag{})
        );
        const auto mesh_ops = MeshStackOperationIds::fromOps(mesh_registration.ops, mesh_registration.op_count);
        if (!mesh_feature.feature.isValid() || !mesh_ops.valid())
            throw std::runtime_error("Spatial3D mesh feature failed");

        const auto cluster_registration =
            fixture.awaitControl(fixture.control().registerFeatureType(kRenderClusterFeatureFactory));
        const auto cluster_feature = fixture.awaitControl(
            fixture.control().addFeature(
                scene.scene_id,
                cluster_registration.feature_type_id,
                RenderClusterCommTag{}
            )
        );
        if (!cluster_feature.feature.isValid())
            throw std::runtime_error("Spatial3D render-cluster feature failed");

        const auto light_registration =
            fixture.awaitControl(fixture.control().registerFeatureType(kLightFeatureFactory));
        const auto light_feature = fixture.awaitControl(
            fixture.control().addFeature(scene.scene_id, light_registration.feature_type_id, LightCommTag{})
        );
        if (!light_feature.feature.isValid())
            throw std::runtime_error("Spatial3D light feature failed");

        const auto shadow_registration =
            fixture.awaitControl(fixture.control().registerFeatureType(kShadowMapFeatureFactory));
        ShadowMapCommConfig shadow_config{};
        shadow_config.atlas_page_resolution = 256U;
        shadow_config.atlas_page_count = 1U;
        const auto shadow_feature = fixture.awaitControl(
            fixture.control().addFeature(scene.scene_id, shadow_registration.feature_type_id, shadow_config)
        );
        if (!shadow_feature.feature.isValid())
            throw std::runtime_error("Spatial3D shadow-map feature failed");

        const auto mesh_shadow_registration =
            fixture.awaitControl(fixture.control().registerFeatureType(kMeshShadowFeatureFactory));
        const auto mesh_shadow_feature = fixture.awaitControl(
            fixture.control().addFeature(
                scene.scene_id,
                mesh_shadow_registration.feature_type_id,
                MeshShadowCommConfig{}
            )
        );
        if (!mesh_shadow_feature.feature.isValid())
            throw std::runtime_error("Spatial3D mesh-shadow feature failed");

        const auto forward_registration =
            fixture.awaitControl(fixture.control().registerFeatureType(kForwardMeshFeatureFactory));
        ForwardMeshCommConfig forward_config{};
        forward_config.graph_fragment = forward_shader.shader;
        const auto forward_feature = fixture.awaitControl(
            fixture.control().addFeature(scene.scene_id, forward_registration.feature_type_id, forward_config)
        );
        if (!forward_feature.feature.isValid())
            throw std::runtime_error("Spatial3D forward feature failed");

        std::vector<std::byte> texture_bytes(16U * 16U * 4U, std::byte{0xFF});
        auto texture_request = fixture.uploadClientForTest().tryCreateTexture2DCopy(
            texture_bytes,
            16,
            16,
            4,
            EPixelFormat::RGBA8_UNORM,
            true
        );
        if (!texture_request)
            throw std::runtime_error("Spatial3D texture upload admission failed");
        const auto texture = fixture.awaitUpload(std::move(*texture_request));
        result.texture_ready = texture.status == 0U && !texture.handle.isNull();
        if (!result.texture_ready)
            throw std::runtime_error("Spatial3D texture upload failed");
        result.texture_index = texture.handle.index;
        result.texture_generation = texture.handle.gen;

        GraphMaterialData graph_material{};
        graph_material.tex_bindless[0] = texture.handle.index;
        graph_material.tex_mask = 1U;
        auto material_request = uploadGraphMaterial(
            MaterialUploadClient{fixture.uploadClientForTest(), material_ops},
            graph_material,
            gbuffer_shader.shader,
            forward_shader.shader
        );
        if (!material_request)
            throw std::runtime_error("Spatial3D material upload admission failed");
        const auto material = fixture.awaitUpload(std::move(*material_request));
        result.material_ready = material.status == 0U && !material.handle.isNull();
        if (!result.material_ready)
            throw std::runtime_error("Spatial3D material upload failed");

        auto mesh_request = uploadMesh(
            MeshStackUploadClient{fixture.uploadClientForTest(), mesh_ops},
            makeTriangle()
        );
        if (!mesh_request)
            throw std::runtime_error("Spatial3D mesh upload admission failed");
        const auto mesh = fixture.awaitUpload(std::move(*mesh_request));
        result.mesh_ready = mesh.status == 0U && !mesh.handle.isNull();
        if (!result.mesh_ready)
            throw std::runtime_error("Spatial3D mesh upload failed");

        if (state.renderable.mesh != 1U || state.renderable.material != 2U || state.renderable.texture != 3U)
            throw std::runtime_error("materialized preloaded asset references changed");

        const float transform[16] = {
            1.0F, 0.0F, 0.0F, 0.0F,
            0.0F, 1.0F, 0.0F, 0.0F,
            0.0F, 0.0F, 1.0F, 0.0F,
            state.render_relative.x(), state.render_relative.y(), state.render_relative.z(), 1.0F
        };
        const auto instance = fixture.await(addTransientMeshInstance(
            MeshStackProxy{fixture.session(), mesh_ops},
            scene.scene_id,
            mesh.handle,
            material.handle,
            transform,
            kInstanceFlagCastShadow | kInstanceFlagReceiveShadow | kInstanceFlagVisible | (1U << 31U)
        ));
        result.instance_ready = instance.status == MeshInstanceCreateStatus::Ok && instance.object;
        if (!result.instance_ready)
            throw std::runtime_error("Spatial3D mesh instance failed");

        float duplicate_transform[16];
        std::copy(std::begin(transform), std::end(transform), std::begin(duplicate_transform));
        duplicate_transform[12] = state.render_relative.x() - 1.0F;
        const auto duplicate_instance = fixture.await(addTransientMeshInstance(
            MeshStackProxy{fixture.session(), mesh_ops},
            scene.scene_id,
            mesh.handle,
            material.handle,
            duplicate_transform,
            kInstanceFlagCastShadow | kInstanceFlagReceiveShadow | kInstanceFlagVisible | (1U << 31U)
        ));
        result.duplicate_instance_ready =
            duplicate_instance.status == MeshInstanceCreateStatus::Ok && duplicate_instance.object;
        if (!result.duplicate_instance_ready)
            throw std::runtime_error("Spatial3D duplicate-interest instance failed");

        const float identity[16] = {
            1.0F, 0.0F, 0.0F, 0.0F,
            0.0F, 1.0F, 0.0F, 0.0F,
            0.0F, 0.0F, 1.0F, 0.0F,
            0.0F, 0.0F, 0.0F, 1.0F
        };
        const float projection[16] = {
            1.0F, 0.0F, 0.0F, 0.0F,
            0.0F, -1.0F, 0.0F, 0.0F,
            0.0F, 0.0F, -1.001001F, -1.0F,
            0.0F, 0.0F, -0.1001001F, 0.0F
        };
        const float camera[3] = {0.0F, 0.0F, 0.0F};
        viewCameraUpdateTransient(
            ViewCameraProxy{fixture.session(), camera_ops},
            scene.scene_id,
            scene.view,
            identity,
            projection,
            camera
        );
        fixture.flush(8);
        const auto pixels = fixture.readback(scene);
        if (fixture.lastReadback().status != 0U)
            throw std::runtime_error("Spatial3D GPU readback failed");
        for (std::size_t offset = 0U; offset + 3U < pixels.size(); offset += 4U)
        {
            if ((pixels[offset] | pixels[offset + 1U] | pixels[offset + 2U]) != 0U)
                ++result.lit_pixels;
        }
        if (result.lit_pixels == 0U)
        {
            std::vector<char> graph(256U * 1024U, '\0');
            const auto dumped = fixture.awaitControl(
                fixture.control().dumpRenderGraph(scene.scene_id, graph.data(), graph.size())
            );
            std::fprintf(
                stderr,
                "graph_status=%u,needed=%u,written=%u\n%.*s\n",
                dumped.status,
                dumped.needed,
                dumped.written,
                static_cast<int>(std::min<std::size_t>(dumped.written, graph.size())),
                graph.data()
            );
        }
        MeshStackProxy{fixture.session(), mesh_ops}.removeMeshInstance({scene.scene_id, instance.object});
        fixture.flush(4);
        const auto after_first = fixture.awaitControl(
            MeshStackControlClient{fixture.control(), mesh_ops}.stats({scene.scene_id})
        );
        result.alive_after_first_release = after_first.alive_instances;
        const auto surviving_pixels = fixture.readback(scene);
        std::uint32_t surviving_lit{};
        for (std::size_t offset = 0U; offset + 3U < surviving_pixels.size(); offset += 4U)
        {
            if ((surviving_pixels[offset] | surviving_pixels[offset + 1U] | surviving_pixels[offset + 2U]) != 0U)
                ++surviving_lit;
        }
        result.survived_first_release = after_first.alive_instances == 1U && surviving_lit > 0U;

        MeshStackProxy{fixture.session(), mesh_ops}.removeMeshInstance({scene.scene_id, duplicate_instance.object});
        fixture.flush(4);
        const auto after_final = fixture.awaitControl(
            MeshStackControlClient{fixture.control(), mesh_ops}.stats({scene.scene_id})
        );
        result.final_release_empty = after_final.alive_instances == 0U;

        MeshStackControlClient{fixture.control(), mesh_ops}.destroyMesh({mesh.handle});
        MaterialControlClient{fixture.control(), material_ops}.destroyMaterial({material.handle});
        fixture.control().destroyTexture(texture.handle);
        fixture.flush(4);

        auto recreated_request = fixture.uploadClientForTest().tryCreateTexture2DCopy(
            texture_bytes,
            16,
            16,
            4,
            EPixelFormat::RGBA8_UNORM,
            true
        );
        if (!recreated_request)
            throw std::runtime_error("Spatial3D texture generation probe admission failed");
        const auto recreated_texture = fixture.awaitUpload(std::move(*recreated_request));
        if (recreated_texture.status != 0U || recreated_texture.handle.isNull())
            throw std::runtime_error("Spatial3D texture generation probe failed");
        result.recreated_texture_index = recreated_texture.handle.index;
        result.recreated_texture_generation = recreated_texture.handle.gen;
        result.texture_generation_changed = recreated_texture.handle.index == texture.handle.index &&
            recreated_texture.handle.gen != texture.handle.gen;
        fixture.control().destroyTexture(recreated_texture.handle);
        return result;
    }
}

int main()
{
    try
    {
        JoltRuntime jolt;
        const Transform3D transform{
            Eigen::Vector3d{1.0e12 + 0.125, 0.0, -5.0},
            Eigen::Quaterniond::Identity(),
            Eigen::Vector3d::Ones()
        };
        const PreloadedRenderable renderable{1U, 2U, 3U};
        const CookedWorld cooked = makeCookedWorld(transform, renderable);

        SystemRegistry system_types;
        const SystemRegistration registration{
            systemTypeId(PreloadedSpatial3DSystem::Description.canonical_name),
            PreloadedSpatial3DSystem::Description.version,
            &installSpatial3D
        };
        if (!system_types.add(registration))
            return 2;
        auto scene = lux::scene::Scene::create(cooked.description, makeSimulationDescription(), system_types);
        if (!scene)
            return 3;

        auto endpoint = std::make_shared<MemoryEndpoint>(cooked.volume);
        WorldPartitionData partition = loadPartition(cooked, endpoint, (*scene)->stopToken());
        ComponentSchemaSet schemas = makeComponentSchemas();
        auto materializer = lux::scene::WorldMaterializer::create(cooked.description, schemas);
        if (!materializer)
            return 4;
        std::vector<Entity> created;
        auto materialized = materializer->partition((*scene)->registry(), partition, &created);
        if (!materialized || created.size() != 2U)
        {
            std::fprintf(
                stderr,
                "materialize=%u,error=%u,component=%u,offset=%llu,data=%llu,created=%llu,objects=%llu\n",
                materialized ? 1U : 0U,
                materialized ? 0U : static_cast<unsigned>(materialized.error().code),
                materialized ? 0U : static_cast<unsigned>(materialized.error().component.code),
                materialized ? 0ULL
                             : static_cast<unsigned long long>(materialized.error().component.offset),
                materialized ? 0ULL : static_cast<unsigned long long>(materialized.error().data),
                static_cast<unsigned long long>(created.size()),
                static_cast<unsigned long long>(partition.objectCount())
            );
            return 5;
        }

        auto executor = lux::task::TaskExecutor::create({0U, 1U});
        if (!executor || !(*scene)->simulation().execute(*executor))
            return 6;
        const auto& state = (*scene)->registry().ctx().get<Spatial3DProbeState>();
        if (!state.ran || !state.contact || state.partition.value != 0U)
            return 7;

        lux::scene::LatestSpscExchange<PresentationState> exchange;
        const Eigen::Vector3d render_origin{1.0e12, 0.0, 0.0};
        const Eigen::Vector3d render_relative = state.physics_position - render_origin;
        exchange.write() = PresentationState{render_relative.cast<float>(), state.renderable, 1U};
        exchange.publish();
        if (!exchange.acquireLatest())
            return 8;

        std::atomic_int validation_errors{};
        const RenderResult rendered = renderPreloaded(exchange.read(), validation_errors);
        if (!rendered.gpu)
        {
            std::puts("SKIP: Vulkan device or validation layer unavailable");
            return 77;
        }
        const bool assets_ready = rendered.texture_ready && rendered.material_ready && rendered.mesh_ready;
        const bool lifecycle_ok = rendered.duplicate_instance_ready && rendered.survived_first_release &&
            rendered.final_release_empty && rendered.texture_generation_changed;
        const bool gpu_ok = assets_ready && rendered.instance_ready && rendered.lit_pixels > 0U && lifecycle_ok;
        const bool validation_ok = validation_errors.load(std::memory_order_relaxed) == 0;
        std::printf(
            "io_submits=%llu,accounting=%u,entities=%llu,partition=%u,jolt_contact=%u,"
            "physics_x=%.9f,render_relative_x=%.9f,texture=%u,material=%u,mesh=%u,instance=%u,"
            "duplicate_instance=%u,alive_after_first_release=%u,survived_first_release=%u,"
            "final_release_empty=%u,texture_slot=%u,texture_generation=%u,recreated_texture_slot=%u,"
            "recreated_texture_generation=%u,texture_generation_changed=%u,lit_pixels=%u,validation_errors=%d\n",
            static_cast<unsigned long long>(endpoint->submits),
            endpoint->accounting_ok ? 1U : 0U,
            static_cast<unsigned long long>(created.size()),
            state.partition.value,
            state.contact ? 1U : 0U,
            state.physics_position.x(),
            static_cast<double>(exchange.read().render_relative.x()),
            rendered.texture_ready ? 1U : 0U,
            rendered.material_ready ? 1U : 0U,
            rendered.mesh_ready ? 1U : 0U,
            rendered.instance_ready ? 1U : 0U,
            rendered.duplicate_instance_ready ? 1U : 0U,
            rendered.alive_after_first_release,
            rendered.survived_first_release ? 1U : 0U,
            rendered.final_release_empty ? 1U : 0U,
            rendered.texture_index,
            rendered.texture_generation,
            rendered.recreated_texture_index,
            rendered.recreated_texture_generation,
            rendered.texture_generation_changed ? 1U : 0U,
            rendered.lit_pixels,
            validation_errors.load(std::memory_order_relaxed)
        );
        return endpoint->accounting_ok && gpu_ok && validation_ok ? 0 : 9;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "spatial3d_preloaded_failure=%s\n", error.what());
        return 10;
    }
}
