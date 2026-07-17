/**
 * Pure structural-invariant validation for a parsed cloud network config.
 *
 * Deliberately free of Arduino dependencies (only the C++ standard library)
 * so the exact invariants the firmware enforces can be exercised by
 * host-side unit tests (`pio test -e native`, test/test_cloud_invariants) —
 * same pattern as config_validation.h and packet_parse.h.
 *
 * Checked over the FULLY parsed structure (api_client.cpp runs this before
 * committing anything):
 *   - collection sizes within their limits (heap bound on this device),
 *   - no duplicate key/unit/group/scene ids (duplicates make the
 *     getXById() lookups ambiguous — commands would hit the wrong entry),
 *   - every group member references a known unit (the parser drops stale
 *     references with a warning; this re-verifies that filter held).
 *
 * Templated on the config type: it only needs the four collections with
 * their id fields (keys[].id, units[].deviceId, groups[].groupId/.unitIds,
 * scenes[].sceneId), so the host tests use small mock structs and never pull
 * in Arduino String.
 */

#ifndef CONFIG_INVARIANTS_H
#define CONFIG_INVARIANTS_H

#include <cstddef>
#include <cstdint>

namespace cloudval {

// Collection limits, filled from the CLOUD_MAX_* firmware constants.
struct CloudLimits {
    size_t maxKeys;
    size_t maxUnits;
    size_t maxGroups;
    size_t maxScenes;
    size_t maxGroupMembers;
};

enum CloudInvariantResult : uint8_t {
    CLOUD_OK = 0,
    CLOUD_TOO_MANY_KEYS,
    CLOUD_TOO_MANY_UNITS,
    CLOUD_TOO_MANY_GROUPS,
    CLOUD_TOO_MANY_SCENES,
    CLOUD_GROUP_TOO_LARGE,        // badId = offending groupId
    CLOUD_DUP_KEY_ID,             // badId = duplicated id
    CLOUD_DUP_UNIT_ID,
    CLOUD_DUP_GROUP_ID,
    CLOUD_DUP_SCENE_ID,
    CLOUD_UNKNOWN_GROUP_MEMBER,   // badId = referenced unit id
};

inline const char* cloudInvariantName(CloudInvariantResult r) {
    switch (r) {
        case CLOUD_OK:                   return "ok";
        case CLOUD_TOO_MANY_KEYS:        return "too many keys";
        case CLOUD_TOO_MANY_UNITS:       return "too many units";
        case CLOUD_TOO_MANY_GROUPS:      return "too many groups";
        case CLOUD_TOO_MANY_SCENES:      return "too many scenes";
        case CLOUD_GROUP_TOO_LARGE:      return "group exceeds member limit";
        case CLOUD_DUP_KEY_ID:           return "duplicate key id";
        case CLOUD_DUP_UNIT_ID:          return "duplicate unit deviceID";
        case CLOUD_DUP_GROUP_ID:         return "duplicate groupID";
        case CLOUD_DUP_SCENE_ID:         return "duplicate sceneID";
        case CLOUD_UNKNOWN_GROUP_MEMBER: return "group references unknown unit";
    }
    return "unknown";
}

namespace detail {

// Membership set over the full uint8 id space. 256 B of stack per instance;
// at most two are alive at a time in validateStructure.
class IdSet {
public:
    IdSet() {
        for (size_t i = 0; i < 256; i++) _seen[i] = false;
    }
    // Returns false if the id was already present (duplicate).
    bool add(uint8_t id) {
        if (_seen[id]) return false;
        _seen[id] = true;
        return true;
    }
    bool contains(uint8_t id) const { return _seen[id]; }

private:
    bool _seen[256];
};

}  // namespace detail

/**
 * Validate the structural invariants of a parsed config. Returns CLOUD_OK or
 * the first violation; `badId` (optional) receives the offending id where
 * one exists (duplicates, oversized group, unknown member).
 */
template <typename TConfig>
inline CloudInvariantResult validateStructure(const TConfig& cfg,
                                              const CloudLimits& lim,
                                              uint8_t* badId = nullptr) {
    if (cfg.keys.size()   > lim.maxKeys)   return CLOUD_TOO_MANY_KEYS;
    if (cfg.units.size()  > lim.maxUnits)  return CLOUD_TOO_MANY_UNITS;
    if (cfg.groups.size() > lim.maxGroups) return CLOUD_TOO_MANY_GROUPS;
    if (cfg.scenes.size() > lim.maxScenes) return CLOUD_TOO_MANY_SCENES;

    {
        detail::IdSet ids;
        for (const auto& k : cfg.keys) {
            if (!ids.add(k.id)) {
                if (badId) *badId = k.id;
                return CLOUD_DUP_KEY_ID;
            }
        }
    }

    // Unit ids stay alive for the group-member check below.
    detail::IdSet unitIds;
    for (const auto& u : cfg.units) {
        if (!unitIds.add(u.deviceId)) {
            if (badId) *badId = u.deviceId;
            return CLOUD_DUP_UNIT_ID;
        }
    }

    {
        detail::IdSet ids;
        for (const auto& g : cfg.groups) {
            if (!ids.add(g.groupId)) {
                if (badId) *badId = g.groupId;
                return CLOUD_DUP_GROUP_ID;
            }
            if (g.unitIds.size() > lim.maxGroupMembers) {
                if (badId) *badId = g.groupId;
                return CLOUD_GROUP_TOO_LARGE;
            }
            for (size_t i = 0; i < g.unitIds.size(); i++) {
                if (!unitIds.contains(g.unitIds[i])) {
                    if (badId) *badId = g.unitIds[i];
                    return CLOUD_UNKNOWN_GROUP_MEMBER;
                }
            }
        }
    }

    {
        detail::IdSet ids;
        for (const auto& s : cfg.scenes) {
            if (!ids.add(s.sceneId)) {
                if (badId) *badId = s.sceneId;
                return CLOUD_DUP_SCENE_ID;
            }
        }
    }

    return CLOUD_OK;
}

}  // namespace cloudval

#endif  // CONFIG_INVARIANTS_H
