module;

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <span>

export module vortex.physics:terrain;

import vortex.core;
import vortex.math;
import vortex.voxel;
import :world;

export namespace vortex::physics {

    struct TerrainColliderStats {
        uint32_t colliders = 0;      ///< Static bodies currently alive.
        uint32_t pending = 0;        ///< Chunks being cooked on worker threads.
        uint32_t built = 0;          ///< Total colliders built so far.
        uint32_t frozenBodies = 0;   ///< Dynamic bodies currently held in place (PhysicsWorld::FrozenBodyCount).
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
     * resting on them are woken up. It must be the only consumer of that list. An edited chunk
     * keeps its old collider until the new one is installed (both happen in the same call), so
     * nothing falls through while a chunk is being re-cooked. A finished mesh is installed even
     * if the chunk was edited again meanwhile (the newest state follows), so a chunk edited every
     * frame still gets colliders, one frame late. A chunk whose *neighbour* was carved so that
     * faces of it were uncovered counts as not ready until it is re-cooked (its collider has no
     * faces there); neighbours whose touching layer did not change are not re-cooked at all.
     *
     * **Bodies never simulate without terrain.** The object also guards every dynamic body of the
     * physics world (GuardBodies):
     *  - Awake bodies ask for colliders themselves: the chunks their bounds touch, extended along
     *    their velocity, are cooked whether or not a focus point is near, with priority, and kept
     *    for a while after the body left or fell asleep. The camera is not needed for this, and
     *    there is no limit on the number of bodies.
     *  - A body is *frozen* (PhysicsWorld::SetFrozen) while any chunk its bounds touch (plus what
     *    it travels in the next few steps) is not ready: not loaded, outside the world window, or
     *    its collider not built yet. A frozen body stays exactly where it is, keeps its velocity
     *    for later and cannot be woken; it resumes as soon as the terrain is ready again.
     *  - A frozen body that was awake, or that something tried to wake or push, keeps asking for
     *    its colliders and so resumes by itself within a few frames. One that was asleep (rubble
     *    left behind by the camera) asks for nothing and costs nothing: it is parked until a focus
     *    point comes back or something wakes it.
     * Kinematic and static bodies are not guarded, nor are character controllers (use IsReady).
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

        /**
         * @brief Freezes dynamic bodies whose terrain is not ready, resumes those whose terrain
         * is, and requests colliders for bodies that move (see the class description).
         * @details Update() does this once. Call it again right before every PhysicsWorld::Step()
         * that follows game code which may have created or moved bodies (the engine does), so
         * that a body spawned in a fixed update is never stepped without ground under it.
         */
        void GuardBodies();

        /// @brief True if the terrain around `worldPos` is ready to be collided with: the chunk
        /// containing it is loaded and either has its collider or needs none.
        bool IsReady(const glm::dvec3& worldPos) const;
        /// @brief The same for a box (the bounds of a character, say): every chunk it touches, grown
        /// by one voxel, is ready. This is the test dynamic bodies are guarded with; prefer it to
        /// the point version for anything that stands on the ground - the ground is often the
        /// chunk *below* the one that contains the feet. False for an empty or non-finite box.
        bool IsReady(const math::AABB& worldBounds) const;
        /// @brief Blocks until every collider requested by the last Update() and by the bodies exists,
        /// then guards the bodies again (tests, loading screens).
        void Flush();

        TerrainColliderStats Stats() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> m_Impl;
    };
}
