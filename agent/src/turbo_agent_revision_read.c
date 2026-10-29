#include "turbo_agent_revision_read.h"

#include <string.h>

static int valid_id(ta_revision_id_t id) {
  return id.data && id.size && id.size <= TA_REVISION_ID_MAX_BYTES;
}

static int valid_query(ta_revision_id_t query) {
  size_t i;
  if (!query.data || !query.size || query.size > TA_REVISION_QUERY_MAX_BYTES)
    return 0;
  for (i = 0; i < query.size; ++i) if (!query.data[i]) return 0;
  return 1;
}

static int valid_budget(const ta_revision_read_budget_t *budget) {
  return budget && budget->max_items && budget->max_result_bytes &&
         budget->max_vm_steps;
}

static int valid_backend(const ta_revision_read_backend_t *backend) {
  const ta_revision_read_backend_ops_t *ops;
  if (!backend || !backend->read_owner || !backend->ops) return 0;
  ops = backend->ops;
  return ops->get && ops->open_search_text && ops->open_scan_prefix &&
         ops->cursor_next && ops->cursor_close;
}

static void release_value(ta_revision_backend_value_t *value) {
  if (value && value->release && value->owner) value->release(value->owner);
  if (value) memset(value, 0, sizeof(*value));
}

static ta_revision_read_status_t backend_status(ta_revision_backend_status_t rc) {
  switch (rc) {
    case TA_REVISION_BACKEND_OK: return TA_REVISION_READ_OK;
    case TA_REVISION_BACKEND_NOT_FOUND: return TA_REVISION_READ_NOT_FOUND;
    case TA_REVISION_BACKEND_LIMIT: return TA_REVISION_READ_LIMIT;
    default: return TA_REVISION_READ_BACKEND_ERROR;
  }
}

static ta_revision_read_status_t generation_status(ta_revision_generation_status_t rc) {
  switch (rc) {
    case TA_REVISION_GENERATION_OK: return TA_REVISION_READ_OK;
    case TA_REVISION_GENERATION_NOT_FOUND: return TA_REVISION_READ_NOT_FOUND;
    case TA_REVISION_GENERATION_LIMIT: return TA_REVISION_READ_LIMIT;
    case TA_REVISION_GENERATION_SCOPE_MISMATCH:
      return TA_REVISION_READ_SCOPE_MISMATCH;
    case TA_REVISION_GENERATION_MALFORMED: return TA_REVISION_READ_MALFORMED;
    default: return TA_REVISION_READ_INVALID;
  }
}

static ta_revision_read_status_t load_generation(
    const ta_revision_read_backend_t *backend,
    const ta_revision_scope_t *scope,
    ta_revision_backend_value_t *value) {
  ta_revision_key_t key;
  ta_revision_backend_status_t brc;
  memset(value, 0, sizeof(*value));
  if (ta_revision_generation_key(scope, &key) != TA_REVISION_KEY_OK)
    return TA_REVISION_READ_INVALID;
  brc = backend->ops->get(backend->read_owner, TA_REVISION_NS_GENERATION,
                          (ta_revision_id_t){key.data, key.size}, value);
  if (brc != TA_REVISION_BACKEND_OK) {
    release_value(value);
    return backend_status(brc);
  }
  if (!value->bytes.data || !value->bytes.size) {
    release_value(value);
    return TA_REVISION_READ_BACKEND_CONTRACT;
  }
  return TA_REVISION_READ_OK;
}

static int has_prefix(ta_revision_id_t key, ta_revision_id_t prefix) {
  return key.data && prefix.data && prefix.size && key.size >= prefix.size &&
         !memcmp(key.data, prefix.data, prefix.size);
}

static ta_revision_read_status_t add_result_bytes(
    uint64_t *total, ta_revision_id_t key, ta_revision_id_t value,
    const ta_revision_read_budget_t *budget) {
  uint64_t key_bytes = (uint64_t)key.size;
  uint64_t value_bytes = (uint64_t)value.size;
  if (key_bytes > UINT64_MAX - value_bytes ||
      *total > UINT64_MAX - key_bytes - value_bytes)
    return TA_REVISION_READ_LIMIT;
  *total += key_bytes + value_bytes;
  return *total > budget->max_result_bytes ? TA_REVISION_READ_LIMIT
                                            : TA_REVISION_READ_OK;
}

static ta_revision_read_status_t walk_search_cursor(
    const ta_revision_read_backend_t *backend, void *cursor,
    const ta_revision_generation_selection_t *selection,
    const ta_revision_read_budget_t *budget,
    ta_revision_read_visit_fn visitor, void *visitor_context) {
  uint32_t items = 0;
  uint64_t result_bytes = 0;
  ta_revision_read_status_t result = TA_REVISION_READ_OK;
  for (;;) {
    ta_revision_id_t key = {NULL, 0};
    ta_revision_backend_value_t value;
    ta_revision_ref_t decoded;
    uint64_t ordinal = 0;
    ta_revision_backend_status_t brc;
    memset(&value, 0, sizeof(value));
    brc = backend->ops->cursor_next(cursor, &key, &value);
    if (brc == TA_REVISION_BACKEND_NOT_FOUND) {
      release_value(&value);
      break;
    }
    if (brc != TA_REVISION_BACKEND_OK) {
      result = backend_status(brc);
      release_value(&value);
      break;
    }
    if (!has_prefix(key, (ta_revision_id_t){selection->chunk_prefix.data,
                                            selection->chunk_prefix.size}) ||
        ta_revision_chunk_decode(key, &decoded, &ordinal) != TA_REVISION_KEY_OK ||
        ordinal >= selection->chunk_count) {
      result = TA_REVISION_READ_BACKEND_CONTRACT;
      release_value(&value);
      break;
    }
    if (++items > budget->max_items) {
      result = TA_REVISION_READ_LIMIT;
      release_value(&value);
      break;
    }
    result = add_result_bytes(&result_bytes, key, value.bytes, budget);
    if (result != TA_REVISION_READ_OK) {
      release_value(&value);
      break;
    }
    if (visitor(visitor_context, key, value.bytes)) {
      result = TA_REVISION_READ_VISITOR_ABORTED;
      release_value(&value);
      break;
    }
    release_value(&value);
  }
  backend->ops->cursor_close(cursor);
  return result;
}

ta_revision_read_status_t ta_revision_read_search_text(
    const ta_revision_read_backend_t *backend,
    const ta_revision_scope_t *authorized_scope,
    ta_revision_id_t authorized_source, ta_revision_id_t query,
    const ta_revision_read_budget_t *budget,
    ta_revision_read_visit_fn visitor, void *visitor_context,
    uint64_t *out_generation_id) {
  ta_revision_backend_value_t generation;
  ta_revision_generation_selection_t selection;
  ta_revision_generation_status_t grc;
  ta_revision_backend_status_t brc;
  ta_revision_read_status_t rc;
  void *cursor = NULL;
  if (out_generation_id) *out_generation_id = 0;
  if (!valid_backend(backend) || !authorized_scope || !valid_id(authorized_source) ||
      !valid_query(query) || !valid_budget(budget) || !visitor ||
      !out_generation_id)
    return TA_REVISION_READ_INVALID;
  rc = load_generation(backend, authorized_scope, &generation);
  if (rc != TA_REVISION_READ_OK) return rc;
  memset(&selection, 0, sizeof(selection));
  grc = ta_revision_generation_resolve(generation.bytes, authorized_scope,
                                       authorized_source, &selection);
  if (grc != TA_REVISION_GENERATION_OK) {
    rc = generation_status(grc);
    release_value(&generation);
    return rc;
  }
  /* selection.ref borrows generation bytes. Only the owned prefix/count/id are
   * used after this point, so the DB value can be released before opening the
   * cursor and no backend call can invalidate borrowed membership bytes. */
  release_value(&generation);
  brc = backend->ops->open_search_text(
      backend->read_owner, TA_REVISION_NS_CHUNK,
      (ta_revision_id_t){selection.chunk_prefix.data, selection.chunk_prefix.size},
      query, budget, &cursor);
  if (brc != TA_REVISION_BACKEND_OK) return backend_status(brc);
  if (!cursor) return TA_REVISION_READ_BACKEND_CONTRACT;
  rc = walk_search_cursor(backend, cursor, &selection, budget, visitor,
                          visitor_context);
  if (rc == TA_REVISION_READ_OK) *out_generation_id = selection.generation_id;
  return rc;
}

static ta_revision_read_status_t walk_edge_cursor(
    const ta_revision_read_backend_t *backend, void *cursor,
    ta_revision_id_t generation_bytes, const ta_revision_scope_t *scope,
    ta_revision_id_t expected_prefix, const ta_revision_read_budget_t *budget,
    ta_revision_read_visit_fn visitor, void *visitor_context) {
  uint32_t items = 0;
  uint64_t result_bytes = 0;
  ta_revision_read_status_t result = TA_REVISION_READ_OK;
  for (;;) {
    ta_revision_id_t key = {NULL, 0};
    ta_revision_backend_value_t value;
    ta_revision_backend_status_t brc;
    int member = 0;
    memset(&value, 0, sizeof(value));
    brc = backend->ops->cursor_next(cursor, &key, &value);
    if (brc == TA_REVISION_BACKEND_NOT_FOUND) {
      release_value(&value);
      break;
    }
    if (brc != TA_REVISION_BACKEND_OK) {
      result = backend_status(brc);
      release_value(&value);
      break;
    }
    /* Count every physical row, including stale historical rows that will be
     * filtered, so retained history cannot bypass the adapter's finite budget. */
    if (++items > budget->max_items) {
      result = TA_REVISION_READ_LIMIT;
      release_value(&value);
      break;
    }
    result = add_result_bytes(&result_bytes, key, value.bytes, budget);
    if (result != TA_REVISION_READ_OK) {
      release_value(&value);
      break;
    }
    if (!has_prefix(key, expected_prefix)) {
      result = TA_REVISION_READ_BACKEND_CONTRACT;
      release_value(&value);
      break;
    }
    {
      ta_revision_generation_status_t grc =
          ta_revision_generation_accept_edge(generation_bytes, scope, key,
                                              &member);
      if (grc != TA_REVISION_GENERATION_OK) {
        result = generation_status(grc);
        release_value(&value);
        break;
      }
    }
    if (!member) {
      release_value(&value);
      continue; /* retained historical edge; not in selected generation */
    }
    if (result != TA_REVISION_READ_OK) {
      release_value(&value);
      break;
    }
    if (visitor(visitor_context, key, value.bytes)) {
      result = TA_REVISION_READ_VISITOR_ABORTED;
      release_value(&value);
      break;
    }
    release_value(&value);
  }
  backend->ops->cursor_close(cursor);
  return result;
}

ta_revision_read_status_t ta_revision_read_scan_edges(
    const ta_revision_read_backend_t *backend,
    const ta_revision_scope_t *authorized_scope,
    ta_revision_id_t authorized_source, ta_revision_id_t relation,
    ta_revision_edge_direction_t direction,
    const ta_revision_read_budget_t *budget,
    ta_revision_read_visit_fn visitor, void *visitor_context,
    uint64_t *out_generation_id) {
  ta_revision_backend_value_t generation;
  ta_revision_generation_selection_t selection;
  ta_revision_key_t edge_prefix;
  ta_revision_generation_status_t grc;
  ta_revision_backend_status_t brc;
  ta_revision_read_status_t rc;
  void *cursor = NULL;
  if (out_generation_id) *out_generation_id = 0;
  if (!valid_backend(backend) || !authorized_scope || !valid_id(authorized_source) ||
      !valid_id(relation) || !valid_budget(budget) || !visitor ||
      !out_generation_id ||
      (direction != TA_REVISION_EDGE_OUT && direction != TA_REVISION_EDGE_IN))
    return TA_REVISION_READ_INVALID;
  rc = load_generation(backend, authorized_scope, &generation);
  if (rc != TA_REVISION_READ_OK) return rc;
  memset(&selection, 0, sizeof(selection));
  grc = ta_revision_generation_resolve(generation.bytes, authorized_scope,
                                       authorized_source, &selection);
  if (grc != TA_REVISION_GENERATION_OK) {
    rc = generation_status(grc);
    release_value(&generation);
    return rc;
  }
  if (ta_revision_edge_prefix(authorized_scope, direction,
                              authorized_source, selection.ref.revision,
                              relation, &edge_prefix) != TA_REVISION_KEY_OK) {
    release_value(&generation);
    return TA_REVISION_READ_INVALID;
  }
  brc = backend->ops->open_scan_prefix(
      backend->read_owner, TA_REVISION_NS_GRAPH,
      (ta_revision_id_t){edge_prefix.data, edge_prefix.size}, budget, &cursor);
  if (brc != TA_REVISION_BACKEND_OK) {
    release_value(&generation);
    return backend_status(brc);
  }
  if (!cursor) {
    release_value(&generation);
    return TA_REVISION_READ_BACKEND_CONTRACT;
  }
  rc = walk_edge_cursor(backend, cursor, generation.bytes, authorized_scope,
                        (ta_revision_id_t){edge_prefix.data, edge_prefix.size},
                        budget, visitor, visitor_context);
  if (rc == TA_REVISION_READ_OK) *out_generation_id = selection.generation_id;
  release_value(&generation);
  return rc;
}
