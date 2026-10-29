#include "turbo_agent_revision_keys.h"

#include <stdio.h>
#include <string.h>

#define REQUIRE(x) do { if (!(x)) { \
  fprintf(stderr, "revision key contract failed at %d: %s\n", __LINE__, #x); \
  return 1; } } while (0)
#define OK(x) REQUIRE((x) == TA_REVISION_KEY_OK)

static ta_revision_id_t bytes(const void *p, size_t n) {
  ta_revision_id_t id = {(const unsigned char *)p, n};
  return id;
}
static ta_revision_id_t text(const char *s) { return bytes(s, strlen(s)); }
static ta_revision_id_t encoded(const ta_revision_key_t *key) {
  return bytes(key->data, key->size);
}
static int equal_id(ta_revision_id_t a, ta_revision_id_t b) {
  return a.size == b.size && a.data && b.data && !memcmp(a.data, b.data, a.size);
}
static int prefix_of(const ta_revision_key_t *prefix, const ta_revision_key_t *key) {
  return prefix->size && prefix->size <= key->size &&
         !memcmp(prefix->data, key->data, prefix->size);
}
static ta_revision_ref_t reference(void) {
  ta_revision_ref_t ref;
  ref.scope.authority = text("tenant-A");
  ref.scope.collection = text("workspace");
  ref.source = text("source-A");
  ref.revision = text("revision-1");
  ref.extraction = text("extractor-1");
  return ref;
}

static int chunk_identity(void) {
  ta_revision_ref_t ref = reference(), other, decoded_ref;
  ta_revision_key_t key, prefix, changed, head, generation;
  uint64_t ordinal = 0;
  OK(ta_revision_chunk_key(&ref, 17, &key));
  OK(ta_revision_chunk_prefix(&ref, &prefix));
  REQUIRE(prefix_of(&prefix, &key));
  REQUIRE(key.size == prefix.size + 8);
  OK(ta_revision_chunk_decode(encoded(&key), &decoded_ref, &ordinal));
  REQUIRE(ordinal == 17 && equal_id(decoded_ref.source, ref.source));
  REQUIRE(equal_id(decoded_ref.revision, ref.revision));
  REQUIRE(equal_id(decoded_ref.extraction, ref.extraction));
  REQUIRE(ta_revision_chunk_matches(encoded(&key), &ref));
  /* Every identity component is exact, not a character-prefix grant. */
  other = ref; other.source = text("source-AA");
  OK(ta_revision_chunk_key(&other, 17, &changed));
  REQUIRE(!prefix_of(&prefix, &changed));
  REQUIRE(!ta_revision_chunk_matches(encoded(&changed), &ref));
  other = ref; other.scope.authority = text("tenant-AA");
  OK(ta_revision_chunk_key(&other, 17, &changed));
  REQUIRE(!prefix_of(&prefix, &changed));
  other = ref; other.scope.collection = text("workspace/sub");
  OK(ta_revision_chunk_key(&other, 17, &changed));
  REQUIRE(!prefix_of(&prefix, &changed));
  other = ref; other.revision = text("revision-2");
  OK(ta_revision_chunk_key(&other, 17, &changed));
  REQUIRE(!prefix_of(&prefix, &changed));
  REQUIRE(!ta_revision_chunk_matches(encoded(&changed), &ref));
  other = ref; other.extraction = text("extractor-2");
  OK(ta_revision_chunk_key(&other, 17, &changed));
  REQUIRE(!prefix_of(&prefix, &changed));
  OK(ta_revision_source_head_key(&ref.scope, ref.source, &head));
  OK(ta_revision_generation_key(&ref.scope, &generation));
  REQUIRE(!prefix_of(&prefix, &head) && !prefix_of(&head, &key));
  REQUIRE(!prefix_of(&generation, &key) && !prefix_of(&generation, &head));
  REQUIRE(ta_revision_chunk_decode(encoded(&head), &decoded_ref, &ordinal) ==
          TA_REVISION_KEY_MALFORMED);
  REQUIRE(ordinal == 0 && decoded_ref.source.data == NULL);
  return 0;
}

static int binary_and_order(void) {
  static const unsigned char identity[] = {0, ':', '/', 255, 'x'};
  static const unsigned char source1[] = {'A', 0, 'B'};
  static const unsigned char source2[] = {'A', 0, 'B', 0};
  static const uint64_t ordinals[] = {
    0, 1, 255, 256, 65535, UINT64_C(0x100000000), UINT64_MAX
  };
  ta_revision_ref_t ref = reference(), roundtrip;
  ta_revision_key_t before = {{0}, 0}, key, prefix;
  uint64_t ordinal;
  size_t i;
  ref.scope.authority = bytes(identity, sizeof(identity));
  ref.source = bytes(source1, sizeof(source1));
  OK(ta_revision_chunk_prefix(&ref, &prefix));
  for (i = 0; i < sizeof(ordinals) / sizeof(ordinals[0]); ++i) {
    OK(ta_revision_chunk_key(&ref, ordinals[i], &key));
    REQUIRE(prefix_of(&prefix, &key));
    OK(ta_revision_chunk_decode(encoded(&key), &roundtrip, &ordinal));
    REQUIRE(ordinal == ordinals[i]);
    REQUIRE(equal_id(roundtrip.scope.authority, ref.scope.authority));
    REQUIRE(equal_id(roundtrip.source, ref.source));
    if (i) REQUIRE(before.size == key.size &&
                   memcmp(before.data, key.data, key.size) < 0);
    before = key;
  }
  ref.source = bytes(source2, sizeof(source2));
  OK(ta_revision_chunk_key(&ref, 0, &key));
  REQUIRE(!prefix_of(&prefix, &key));
  /* Delimiter-shaped bytes cannot resegment a length-prefixed tuple. */
  ref = reference(); ref.scope.authority = text("a/b"); ref.scope.collection = text("c");
  OK(ta_revision_chunk_key(&ref, 0, &before));
  ref.scope.authority = text("a"); ref.scope.collection = text("b/c");
  OK(ta_revision_chunk_key(&ref, 0, &key));
  REQUIRE(before.size == key.size && memcmp(before.data, key.data, key.size));
  return 0;
}

static int graph_directions(void) {
  ta_revision_ref_t ref = reference();
  ta_revision_edge_t edge, parsed;
  ta_revision_edge_direction_t direction;
  ta_revision_key_t out, in, prefix, changed;
  edge.scope = ref.scope;
  edge.from_source = ref.source; edge.from_revision = ref.revision;
  edge.to_source = text("source-B"); edge.to_revision = text("revision-9");
  edge.relation = text("DERIVED_FROM"); edge.edge_id = text("edge-1");
  OK(ta_revision_edge_key(&edge, TA_REVISION_EDGE_OUT, &out));
  OK(ta_revision_edge_key(&edge, TA_REVISION_EDGE_IN, &in));
  REQUIRE(out.size == in.size && memcmp(out.data, in.data, out.size));
  OK(ta_revision_edge_prefix(&edge.scope, TA_REVISION_EDGE_OUT,
      edge.from_source, edge.from_revision, edge.relation, &prefix));
  REQUIRE(prefix_of(&prefix, &out) && !prefix_of(&prefix, &in));
  OK(ta_revision_edge_decode(encoded(&out), &parsed, &direction));
  REQUIRE(direction == TA_REVISION_EDGE_OUT);
  REQUIRE(equal_id(parsed.from_source, edge.from_source));
  REQUIRE(equal_id(parsed.to_revision, edge.to_revision));
  OK(ta_revision_edge_decode(encoded(&in), &parsed, &direction));
  REQUIRE(direction == TA_REVISION_EDGE_IN);
  REQUIRE(equal_id(parsed.from_source, edge.from_source));
  REQUIRE(equal_id(parsed.to_source, edge.to_source));
  REQUIRE(equal_id(parsed.from_revision, edge.from_revision));
  REQUIRE(equal_id(parsed.edge_id, edge.edge_id));
  OK(ta_revision_edge_prefix(&edge.scope, TA_REVISION_EDGE_IN,
      edge.to_source, edge.to_revision, edge.relation, &prefix));
  REQUIRE(prefix_of(&prefix, &in) && !prefix_of(&prefix, &out));
  edge.to_revision = text("revision-10");
  OK(ta_revision_edge_key(&edge, TA_REVISION_EDGE_IN, &changed));
  REQUIRE(!prefix_of(&prefix, &changed));
  /* Encoding only one direction is not a DB transaction. The future adapter
   * must put OUT, IN and evidence/generation records into the same batch. */
  return 0;
}

static int reject_malformed(void) {
  ta_revision_ref_t ref = reference(), parsed;
  ta_revision_key_t key, invalid, prefix;
  ta_revision_edge_t edge;
  ta_revision_edge_direction_t direction;
  unsigned char large[TA_REVISION_ID_MAX_BYTES + 1u];
  uint64_t ordinal;
  size_t i;
  memset(large, 255, sizeof(large));
  OK(ta_revision_chunk_key(&ref, UINT64_MAX, &key));
  for (i = 0; i < key.size; ++i) {
    memset(&parsed, 0x5a, sizeof(parsed)); ordinal = 99;
    REQUIRE(ta_revision_chunk_decode(bytes(key.data, i), &parsed, &ordinal) ==
            TA_REVISION_KEY_MALFORMED);
    REQUIRE(!parsed.source.data && ordinal == 0);
  }
  invalid = key; invalid.data[3] = 2;
  REQUIRE(ta_revision_chunk_decode(encoded(&invalid), &parsed, &ordinal) ==
          TA_REVISION_KEY_MALFORMED);
  invalid = key; invalid.data[invalid.size++] = 0;
  REQUIRE(ta_revision_chunk_decode(encoded(&invalid), &parsed, &ordinal) ==
          TA_REVISION_KEY_MALFORMED);
  invalid = key; invalid.data[4] = 255; invalid.data[5] = 255;
  REQUIRE(ta_revision_chunk_decode(encoded(&invalid), &parsed, &ordinal) ==
          TA_REVISION_KEY_MALFORMED);
  OK(ta_revision_chunk_prefix(&ref, &prefix));
  REQUIRE(ta_revision_chunk_decode(encoded(&prefix), &parsed, &ordinal) ==
          TA_REVISION_KEY_MALFORMED);
  REQUIRE(ta_revision_edge_decode(encoded(&key), &edge, &direction) ==
          TA_REVISION_KEY_MALFORMED);
  REQUIRE(!edge.from_source.data && direction == (ta_revision_edge_direction_t)0);
  memset(&invalid, 0xa5, sizeof(invalid));
  ref.source = bytes(NULL, 1);
  REQUIRE(ta_revision_chunk_key(&ref, 0, &invalid) == TA_REVISION_KEY_INVALID);
  REQUIRE(invalid.size == 0 && invalid.data[0] == 0 && invalid.data[4095] == 0);
  ref.source = bytes(large, sizeof(large));
  REQUIRE(ta_revision_chunk_key(&ref, 0, &invalid) == TA_REVISION_KEY_LIMIT);
  REQUIRE(invalid.size == 0);
  ref.source = bytes(large, sizeof(large) - 1u);
  OK(ta_revision_chunk_key(&ref, 0, &key));
  REQUIRE(ta_revision_chunk_decode(encoded(&key), NULL, &ordinal) == TA_REVISION_KEY_INVALID);
  REQUIRE(ta_revision_edge_prefix(&ref.scope, (ta_revision_edge_direction_t)99,
      ref.source, ref.revision, text("reads"), &invalid) == TA_REVISION_KEY_INVALID);
  REQUIRE(invalid.size == 0);
  return 0;
}

static uint32_t next_random(uint32_t *seed) {
  *seed = *seed * UINT32_C(1664525) + UINT32_C(1013904223);
  return *seed;
}
static int randomized_roundtrips(void) {
  uint32_t seed = UINT32_C(0x6a221cc2);
  unsigned char fields[5][TA_REVISION_ID_MAX_BYTES];
  ta_revision_id_t ids[5];
  ta_revision_ref_t ref, parsed;
  ta_revision_key_t key, again, prefix;
  uint64_t ordinal, decoded;
  size_t iteration, i, j;
  for (iteration = 0; iteration < 10000u; ++iteration) {
    for (i = 0; i < 5u; ++i) {
      size_t length = (size_t)(next_random(&seed) % TA_REVISION_ID_MAX_BYTES) + 1u;
      for (j = 0; j < length; ++j) fields[i][j] = (unsigned char)(next_random(&seed) >> 24);
      ids[i] = bytes(fields[i], length);
    }
    ref.scope.authority = ids[0]; ref.scope.collection = ids[1];
    ref.source = ids[2]; ref.revision = ids[3]; ref.extraction = ids[4];
    ordinal = ((uint64_t)next_random(&seed) << 32) | next_random(&seed);
    OK(ta_revision_chunk_key(&ref, ordinal, &key));
    OK(ta_revision_chunk_decode(encoded(&key), &parsed, &decoded));
    REQUIRE(decoded == ordinal && ta_revision_chunk_matches(encoded(&key), &ref));
    OK(ta_revision_chunk_key(&parsed, decoded, &again));
    REQUIRE(key.size == again.size && !memcmp(key.data, again.data, key.size));
    OK(ta_revision_chunk_prefix(&parsed, &prefix));
    REQUIRE(prefix_of(&prefix, &key));
    /* Encoder supports reusing prior key bytes with independent descriptors. */
    OK(ta_revision_chunk_key(&parsed, decoded, &key));
    REQUIRE(key.size == again.size && !memcmp(key.data, again.data, key.size));
  }
  return 0;
}

int main(void) {
  if (chunk_identity() || binary_and_order() || graph_directions() ||
      reject_malformed() || randomized_roundtrips()) return 1;
  puts("TurboAgent revision-key contract PASS (10000 deterministic roundtrips)");
  return 0;
}
