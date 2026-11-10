module;

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <span>

export module vortex.physics:terrain;

import vortex.core;
import vortex.voxel;
import :world;

export namespace vortex::physics {

    struct TerrainColliderStats {
        uint32_t colliders = 0;      ///< Static bodies currently alive.
        uint32_t pending = 0;        ///< Chunks being cooked on worker threads.
        uint32_t built = 0;          ///< Total colliders built so far.
    };

    /**
     * @brief Keeps static collision meshes for the terrain near the things that need them.
     * @details Colliders are streamed just like the world itself: each Update() is given the
     * points that need ground under them (players, cameras, moving bodies) and a radius; chunks
     * in range get a triangle-mesh body, chunks out of range lose theirs (with hysteresis).
     * Meshes are cooked on worker threads from a snapshot of the chunk taken on the main thread
     * (the world itself is main-thread only), and installed on the main thread.
     *
     * The object consumes World::TakeChangedChunks(): edited chunks are re-cooked and bodies
     * resting on them are woken up. It must be the only consumer of that list.
     */
    class TerrainColliders {
    public:
        TerrainColliders(PhysicsWorld& physics, voxel::World& world, JobSystem& jobs,
                         const voxel::MaterialLibrary& materials);
        ~TerrainColliders();
        TerrainColliders(const TerrainColliders&) = delete;
        TerrainColliders& operator=(const TerrainColliders&) = delete;

        /**
         * @brief Call once per frame on the main thread.
         * @param focusPoints World positions (metres) that need collision around them.
         * @param radius Metres around each focus point to keep colliders for.
         */
        void Update(std::span<const glm::dvec3> focusPoints, float radius);

        /// @brief True if the terrain around `worldPos` is ready to be collided with: the chunk
        /// containing it is loaded and either has its collider or needs none.
        bool IsReady(const glm::dvec3& worldPos) const;
        /// @brief Blocks until every collider requested by the last Update() exists (tests, loading screens).
        void Flush();

        TerrainColliderStats Stats() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> m_Impl;
    };
}
