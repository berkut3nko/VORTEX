module;

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

export module vortex.net:entities;

export namespace vortex::net {

    /**
     * @brief One replicated object as a side holds it (docs/design/networking.md stage 6: the
     * entity check of the drill and the tests; the wire `EntityCheck` of the design is stage 7).
     * The server's are its bodies with a scene::NetId; a client's are its copies of them.
     */
    struct EntityInfo {
        uint32_t netId = 0;
        uint64_t modelHash = 0;           ///< net::VolumeHash of its model
        glm::vec3 position{0.0f};         ///< model origin, world metres (a client: where its copy is now)
        glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
        bool resting = false;             ///< server: asleep (EntityRest sent); client: its copy rests
    };

    /// @brief What CompareEntities found between the server's objects and a client's copies.
    struct EntityComparison {
        size_t server = 0, client = 0;
        uint32_t missing = 0;             ///< on the server, not on the client
        uint32_t extra = 0;               ///< on the client, not on the server
        uint32_t modelMismatches = 0;     ///< same NetId, another model hash
        uint32_t restingCompared = 0;     ///< resting on both sides
        float maxRestError = 0.0f;        ///< metres between the two poses' origins, resting ones
        uint32_t movingCompared = 0;      ///< awake on either side
        float maxMovingError = 0.0f;
        /// Same NetIds and models, and every object resting on both sides within `restTolerance`.
        bool Agree(float restTolerance = 0.05f) const {
            return missing == 0 && extra == 0 && modelMismatches == 0 && maxRestError <= restTolerance;
        }
        std::string Describe() const;
    };

    /// @brief Compares two lists (any order): ids, model hashes, rest poses, and how far the copies
    /// of moving objects are from the server's (they are drawn in the past: logged, not judged).
    EntityComparison CompareEntities(std::span<const EntityInfo> server, std::span<const EntityInfo> client);
    /// @brief A hash of the NetIds and model hashes of a list (any order): what two processes log to
    /// compare (tools/session-check.sh).
    uint64_t EntityListHash(std::span<const EntityInfo> entities);
}
