/**
 * Host-side unit tests for the pure cloud-config structural invariants
 * (config_invariants.h). These run via `pio test -e native` and pin down
 * exactly the checks api_client.cpp performs before committing a downloaded
 * network configuration: collection limits, duplicate ids, and group-member
 * references. Mock structs mirror only the fields the validator reads, so no
 * Arduino dependency is needed.
 */

#include <unity.h>
#include <vector>
#include <cstdint>

#include "cloud/config_invariants.h"

using cloudval::CloudLimits;
using cloudval::CloudInvariantResult;
using cloudval::validateStructure;

// ---------------------------------------------------------------------------
// Mocks: only the fields validateStructure() reads.
// ---------------------------------------------------------------------------

struct MockKey   { uint8_t id; };
struct MockUnit  { uint8_t deviceId; };
struct MockGroup { uint8_t groupId; std::vector<uint8_t> unitIds; };
struct MockScene { uint8_t sceneId; };

struct MockConfig {
    std::vector<MockKey>   keys;
    std::vector<MockUnit>  units;
    std::vector<MockGroup> groups;
    std::vector<MockScene> scenes;
};

// Small limits so the over-limit cases stay readable.
static const CloudLimits LIM = { /*maxKeys*/ 4, /*maxUnits*/ 8,
                                 /*maxGroups*/ 4, /*maxScenes*/ 4,
                                 /*maxGroupMembers*/ 3 };

// A well-formed config touching every collection.
static MockConfig makeValid() {
    MockConfig c;
    c.keys   = { {1}, {2} };
    c.units  = { {10}, {11}, {12} };
    c.groups = { {1, {10, 11}}, {2, {12}} };
    c.scenes = { {5}, {6} };
    return c;
}

void setUp(void) {}
void tearDown(void) {}

// ---------------------------------------------------------------------------

void test_valid_config_ok(void) {
    MockConfig c = makeValid();
    TEST_ASSERT_EQUAL(cloudval::CLOUD_OK, validateStructure(c, LIM));
}

void test_empty_config_ok(void) {
    MockConfig c;   // Classic network without keys, empty network
    TEST_ASSERT_EQUAL(cloudval::CLOUD_OK, validateStructure(c, LIM));
}

// Group ids may collide with unit/scene ids — only WITHIN a collection must
// ids be unique (they address different namespaces).
void test_cross_collection_id_collision_ok(void) {
    MockConfig c = makeValid();
    c.scenes.push_back({10});   // same value as unit 10 — fine
    c.groups.push_back({3, {}});
    TEST_ASSERT_EQUAL(cloudval::CLOUD_OK, validateStructure(c, LIM));
}

// ---- duplicates -------------------------------------------------------------

void test_duplicate_key_id(void) {
    MockConfig c = makeValid();
    c.keys.push_back({2});
    uint8_t bad = 0;
    TEST_ASSERT_EQUAL(cloudval::CLOUD_DUP_KEY_ID, validateStructure(c, LIM, &bad));
    TEST_ASSERT_EQUAL(2, bad);
}

void test_duplicate_unit_id(void) {
    MockConfig c = makeValid();
    c.units.push_back({11});
    uint8_t bad = 0;
    TEST_ASSERT_EQUAL(cloudval::CLOUD_DUP_UNIT_ID, validateStructure(c, LIM, &bad));
    TEST_ASSERT_EQUAL(11, bad);
}

void test_duplicate_group_id(void) {
    MockConfig c = makeValid();
    c.groups.push_back({2, {}});
    uint8_t bad = 0;
    TEST_ASSERT_EQUAL(cloudval::CLOUD_DUP_GROUP_ID, validateStructure(c, LIM, &bad));
    TEST_ASSERT_EQUAL(2, bad);
}

void test_duplicate_scene_id(void) {
    MockConfig c = makeValid();
    c.scenes.push_back({5});
    uint8_t bad = 0;
    TEST_ASSERT_EQUAL(cloudval::CLOUD_DUP_SCENE_ID, validateStructure(c, LIM, &bad));
    TEST_ASSERT_EQUAL(5, bad);
}

// id 0 must be treated as a regular id, not as a sentinel
void test_duplicate_id_zero_detected(void) {
    MockConfig c;
    c.units = { {0}, {0} };
    TEST_ASSERT_EQUAL(cloudval::CLOUD_DUP_UNIT_ID, validateStructure(c, LIM));
}

// ---- limits -----------------------------------------------------------------

void test_too_many_keys(void) {
    MockConfig c = makeValid();
    c.keys = { {1}, {2}, {3}, {4}, {5} };   // 5 > 4
    TEST_ASSERT_EQUAL(cloudval::CLOUD_TOO_MANY_KEYS, validateStructure(c, LIM));
}

void test_too_many_units(void) {
    MockConfig c;
    for (int i = 0; i < 9; i++) c.units.push_back({(uint8_t)i});   // 9 > 8
    TEST_ASSERT_EQUAL(cloudval::CLOUD_TOO_MANY_UNITS, validateStructure(c, LIM));
}

void test_too_many_groups(void) {
    MockConfig c = makeValid();
    c.groups = { {1, {}}, {2, {}}, {3, {}}, {4, {}}, {5, {}} };
    TEST_ASSERT_EQUAL(cloudval::CLOUD_TOO_MANY_GROUPS, validateStructure(c, LIM));
}

void test_too_many_scenes(void) {
    MockConfig c = makeValid();
    c.scenes = { {1}, {2}, {3}, {4}, {5} };
    TEST_ASSERT_EQUAL(cloudval::CLOUD_TOO_MANY_SCENES, validateStructure(c, LIM));
}

void test_at_limit_ok(void) {
    MockConfig c;
    c.keys   = { {1}, {2}, {3}, {4} };            // exactly maxKeys
    c.scenes = { {1}, {2}, {3}, {4} };            // exactly maxScenes
    c.units  = { {10}, {11}, {12} };
    c.groups = { {1, {10, 11, 12}} };             // exactly maxGroupMembers
    TEST_ASSERT_EQUAL(cloudval::CLOUD_OK, validateStructure(c, LIM));
}

void test_group_too_large(void) {
    MockConfig c;
    c.units  = { {10}, {11}, {12}, {13} };
    c.groups = { {7, {10, 11, 12, 13}} };         // 4 > 3 members
    uint8_t bad = 0;
    TEST_ASSERT_EQUAL(cloudval::CLOUD_GROUP_TOO_LARGE, validateStructure(c, LIM, &bad));
    TEST_ASSERT_EQUAL(7, bad);                    // offending groupId
}

// ---- group-member references --------------------------------------------------

void test_unknown_group_member(void) {
    MockConfig c = makeValid();
    c.groups.push_back({3, {99}});                // unit 99 does not exist
    uint8_t bad = 0;
    TEST_ASSERT_EQUAL(cloudval::CLOUD_UNKNOWN_GROUP_MEMBER,
                      validateStructure(c, LIM, &bad));
    TEST_ASSERT_EQUAL(99, bad);                   // referenced unit id
}

void test_empty_group_ok(void) {
    MockConfig c = makeValid();
    c.groups.push_back({3, {}});
    TEST_ASSERT_EQUAL(cloudval::CLOUD_OK, validateStructure(c, LIM));
}

// ---- reason names -------------------------------------------------------------

void test_reason_names_nonempty(void) {
    for (int r = cloudval::CLOUD_OK; r <= cloudval::CLOUD_UNKNOWN_GROUP_MEMBER; r++) {
        const char* name = cloudval::cloudInvariantName((CloudInvariantResult)r);
        TEST_ASSERT_NOT_NULL(name);
        TEST_ASSERT_TRUE(name[0] != '\0');
    }
}

// ---------------------------------------------------------------------------

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_valid_config_ok);
    RUN_TEST(test_empty_config_ok);
    RUN_TEST(test_cross_collection_id_collision_ok);
    RUN_TEST(test_duplicate_key_id);
    RUN_TEST(test_duplicate_unit_id);
    RUN_TEST(test_duplicate_group_id);
    RUN_TEST(test_duplicate_scene_id);
    RUN_TEST(test_duplicate_id_zero_detected);
    RUN_TEST(test_too_many_keys);
    RUN_TEST(test_too_many_units);
    RUN_TEST(test_too_many_groups);
    RUN_TEST(test_too_many_scenes);
    RUN_TEST(test_at_limit_ok);
    RUN_TEST(test_group_too_large);
    RUN_TEST(test_unknown_group_member);
    RUN_TEST(test_empty_group_ok);
    RUN_TEST(test_reason_names_nonempty);
    return UNITY_END();
}
