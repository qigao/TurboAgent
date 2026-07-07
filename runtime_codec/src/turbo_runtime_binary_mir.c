#include "turbo_runtime_binary_mir.h"
#include "turbo_runtime_binary_reader.h"

#include <turbo_str.h>
#include <mir.h>
#include <mir-gen.h>
#include <stdlib.h>
#include <string.h>

static char *turbo_runtime_binary_mir_strdup(const char *value) {
  if (!value) {
    return NULL;
  }
  return (char *)tstr_dup(value);
}

typedef struct turbo_runtime_binary_mir_symbol_meta_s {
  turbo_runtime_binary_mir_symbol_id_t id;
  const char *name;
  turbo_runtime_binary_mir_abi_type_t result_type;
  size_t argument_count;
  turbo_runtime_binary_mir_abi_type_t argument_types[4];
} turbo_runtime_binary_mir_symbol_meta_t;

struct turbo_runtime_binary_mir_plan_s {
  turbo_runtime_binary_mir_symbol_descriptor_t *symbols;
  size_t symbol_count;
};

struct turbo_runtime_binary_mir_compiler_s {
  char *entry_name;
  turbo_runtime_binary_schema_t *schema;
  turbo_runtime_binary_mir_plan_t *plan;
  MIR_context_t ctx;
  MIR_module_t module;
  MIR_item_t func_item;
  turbo_runtime_binary_mir_parser_fn parser;
  int gen_initialized;
};

typedef enum {
  TURBO_RUNTIME_BINARY_MIR_FAST_PATH_SCALAR = 0,
  TURBO_RUNTIME_BINARY_MIR_FAST_PATH_REPEATED = 1,
  TURBO_RUNTIME_BINARY_MIR_FAST_PATH_MAP = 2
} turbo_runtime_binary_mir_fast_path_kind_t;

typedef struct turbo_runtime_binary_mir_field_codegen_ctx_s {
  MIR_context_t ctx;
  MIR_item_t func_item;
  MIR_item_t reader_remaining_proto_item;
  MIR_item_t reader_remaining_import_item;
  MIR_item_t read_bool_proto_item;
  MIR_item_t read_bool_import_item;
  MIR_item_t read_i32_proto_item;
  MIR_item_t read_i32_import_item;
  MIR_item_t read_i64_proto_item;
  MIR_item_t read_i64_import_item;
  MIR_item_t read_f32_proto_item;
  MIR_item_t read_f32_import_item;
  MIR_item_t read_f64_proto_item;
  MIR_item_t read_f64_import_item;
  MIR_item_t create_bool_proto_item;
  MIR_item_t create_bool_import_item;
  MIR_item_t create_int64_proto_item;
  MIR_item_t create_int64_import_item;
  MIR_item_t create_double_proto_item;
  MIR_item_t create_double_import_item;
  MIR_item_t string_value_proto_item;
  MIR_item_t string_value_import_item;
  MIR_item_t bytes_value_proto_item;
  MIR_item_t bytes_value_import_item;
  MIR_item_t repeated_value_proto_item;
  MIR_item_t repeated_value_import_item;
  MIR_item_t map_value_proto_item;
  MIR_item_t map_value_import_item;
  MIR_reg_t status_reg;
  MIR_reg_t reader_reg;
  MIR_reg_t bool_reg;
  MIR_reg_t int64_reg;
  MIR_reg_t double_reg;
  MIR_reg_t field_value_reg;
  MIR_reg_t builder_user_data_reg;
  MIR_label_t fail_label;
  const turbo_runtime_data_bind_value_api_t *api;
} turbo_runtime_binary_mir_field_codegen_ctx_t;

typedef struct turbo_runtime_binary_mir_item_pair_s {
  MIR_item_t import_item;
  MIR_item_t proto_item;
} turbo_runtime_binary_mir_item_pair_t;

typedef struct turbo_runtime_binary_mir_fast_path_items_s {
  turbo_runtime_binary_mir_item_pair_t reader_create;
  turbo_runtime_binary_mir_item_pair_t reader_destroy;
  turbo_runtime_binary_mir_item_pair_t reader_remaining;
  turbo_runtime_binary_mir_item_pair_t read_bool;
  turbo_runtime_binary_mir_item_pair_t read_i32;
  turbo_runtime_binary_mir_item_pair_t read_i64;
  turbo_runtime_binary_mir_item_pair_t read_f32;
  turbo_runtime_binary_mir_item_pair_t read_f64;
  turbo_runtime_binary_mir_item_pair_t create_bool;
  turbo_runtime_binary_mir_item_pair_t create_int64;
  turbo_runtime_binary_mir_item_pair_t create_double;
  turbo_runtime_binary_mir_item_pair_t create_object;
  turbo_runtime_binary_mir_item_pair_t object_set;
  turbo_runtime_binary_mir_item_pair_t destroy_value;
  turbo_runtime_binary_mir_item_pair_t string_value;
  turbo_runtime_binary_mir_item_pair_t bytes_value;
  turbo_runtime_binary_mir_item_pair_t repeated_value;
  turbo_runtime_binary_mir_item_pair_t map_value;
} turbo_runtime_binary_mir_fast_path_items_t;

static const turbo_runtime_binary_mir_symbol_meta_t
    turbo_runtime_binary_mir_symbol_table[] = {
        {TURBO_RUNTIME_BINARY_MIR_SYMBOL_CREATE_BOOL, "db_create_bool",
         TURBO_RUNTIME_BINARY_MIR_ABI_PTR, 2,
         {TURBO_RUNTIME_BINARY_MIR_ABI_PTR, TURBO_RUNTIME_BINARY_MIR_ABI_I32}},
        {TURBO_RUNTIME_BINARY_MIR_SYMBOL_CREATE_INT64, "db_create_int64",
         TURBO_RUNTIME_BINARY_MIR_ABI_PTR, 2,
         {TURBO_RUNTIME_BINARY_MIR_ABI_PTR, TURBO_RUNTIME_BINARY_MIR_ABI_I64}},
        {TURBO_RUNTIME_BINARY_MIR_SYMBOL_CREATE_DOUBLE, "db_create_double",
         TURBO_RUNTIME_BINARY_MIR_ABI_PTR, 2,
         {TURBO_RUNTIME_BINARY_MIR_ABI_PTR,
          TURBO_RUNTIME_BINARY_MIR_ABI_DOUBLE}},
        {TURBO_RUNTIME_BINARY_MIR_SYMBOL_CREATE_STRING, "db_create_string",
         TURBO_RUNTIME_BINARY_MIR_ABI_PTR, 2,
         {TURBO_RUNTIME_BINARY_MIR_ABI_PTR, TURBO_RUNTIME_BINARY_MIR_ABI_PTR}},
        {TURBO_RUNTIME_BINARY_MIR_SYMBOL_CREATE_BYTES, "db_create_bytes",
         TURBO_RUNTIME_BINARY_MIR_ABI_PTR, 3,
         {TURBO_RUNTIME_BINARY_MIR_ABI_PTR, TURBO_RUNTIME_BINARY_MIR_ABI_PTR,
          TURBO_RUNTIME_BINARY_MIR_ABI_I64}},
        {TURBO_RUNTIME_BINARY_MIR_SYMBOL_CREATE_OBJECT, "db_create_object",
         TURBO_RUNTIME_BINARY_MIR_ABI_PTR, 1,
         {TURBO_RUNTIME_BINARY_MIR_ABI_PTR}},
        {TURBO_RUNTIME_BINARY_MIR_SYMBOL_CREATE_ARRAY, "db_create_array",
         TURBO_RUNTIME_BINARY_MIR_ABI_PTR, 1,
         {TURBO_RUNTIME_BINARY_MIR_ABI_PTR}},
        {TURBO_RUNTIME_BINARY_MIR_SYMBOL_OBJECT_SET, "db_object_set",
         TURBO_RUNTIME_BINARY_MIR_ABI_I32, 4,
         {TURBO_RUNTIME_BINARY_MIR_ABI_PTR, TURBO_RUNTIME_BINARY_MIR_ABI_PTR,
          TURBO_RUNTIME_BINARY_MIR_ABI_PTR,
          TURBO_RUNTIME_BINARY_MIR_ABI_PTR}},
        {TURBO_RUNTIME_BINARY_MIR_SYMBOL_ARRAY_APPEND, "db_array_append",
         TURBO_RUNTIME_BINARY_MIR_ABI_I32, 3,
         {TURBO_RUNTIME_BINARY_MIR_ABI_PTR, TURBO_RUNTIME_BINARY_MIR_ABI_PTR,
          TURBO_RUNTIME_BINARY_MIR_ABI_PTR}},
        {TURBO_RUNTIME_BINARY_MIR_SYMBOL_DESTROY_VALUE, "db_destroy_value",
         TURBO_RUNTIME_BINARY_MIR_ABI_VOID, 2,
         {TURBO_RUNTIME_BINARY_MIR_ABI_PTR, TURBO_RUNTIME_BINARY_MIR_ABI_PTR}}};

static const turbo_runtime_binary_mir_symbol_meta_t *
turbo_runtime_binary_mir_symbol_meta(turbo_runtime_binary_mir_symbol_id_t id) {
  size_t i;

  for (i = 0; i < sizeof(turbo_runtime_binary_mir_symbol_table) /
                      sizeof(turbo_runtime_binary_mir_symbol_table[0]);
       ++i) {
    if (turbo_runtime_binary_mir_symbol_table[i].id == id) {
      return &turbo_runtime_binary_mir_symbol_table[i];
    }
  }
  return NULL;
}



static void turbo_runtime_binary_mir_write_error(char *buffer, size_t buffer_size,
                                                 const char *message) {
  if (!buffer || buffer_size == 0) {
    return;
  }
  if (!message) {
    buffer[0] = '\0';
    return;
  }
  strncpy(buffer, message, buffer_size - 1);
  buffer[buffer_size - 1] = '\0';
}

static turbo_runtime_binary_mir_status_t
turbo_runtime_binary_mir_plan_append(
    turbo_runtime_binary_mir_plan_t *plan,
    const turbo_runtime_binary_mir_symbol_meta_t *meta) {
  turbo_runtime_binary_mir_symbol_descriptor_t *symbols;
  size_t next_count;

  if (!plan || !meta) {
    return TURBO_RUNTIME_BINARY_MIR_INVALID_ARGUMENT;
  }

  next_count = plan->symbol_count + 1;
  symbols = (turbo_runtime_binary_mir_symbol_descriptor_t *)realloc(
      plan->symbols, next_count * sizeof(*symbols));
  if (!symbols) {
    return TURBO_RUNTIME_BINARY_MIR_OUT_OF_MEMORY;
  }

  plan->symbols = symbols;
  plan->symbols[plan->symbol_count].id = meta->id;
  plan->symbols[plan->symbol_count].name = meta->name;
  plan->symbols[plan->symbol_count].result_type = meta->result_type;
  plan->symbols[plan->symbol_count].argument_count = meta->argument_count;
  memset(plan->symbols[plan->symbol_count].argument_types, 0,
         sizeof(plan->symbols[plan->symbol_count].argument_types));
  memcpy(plan->symbols[plan->symbol_count].argument_types, meta->argument_types,
         meta->argument_count * sizeof(meta->argument_types[0]));
  plan->symbol_count = next_count;
  return TURBO_RUNTIME_BINARY_MIR_OK;
}

static turbo_runtime_binary_mir_status_t
turbo_runtime_binary_mir_plan_add_if_needed(
    turbo_runtime_binary_mir_plan_t *plan, int needed,
    turbo_runtime_binary_mir_symbol_id_t id) {
  const turbo_runtime_binary_mir_symbol_meta_t *meta;

  if (!needed) {
    return TURBO_RUNTIME_BINARY_MIR_OK;
  }

  meta = turbo_runtime_binary_mir_symbol_meta(id);
  if (!meta) {
    return TURBO_RUNTIME_BINARY_MIR_ERROR;
  }

  return turbo_runtime_binary_mir_plan_append(plan, meta);
}

static MIR_type_t turbo_runtime_binary_mir_to_mir_type(
    turbo_runtime_binary_mir_abi_type_t type) {
  switch (type) {
    case TURBO_RUNTIME_BINARY_MIR_ABI_VOID:
      return MIR_T_UNDEF;
    case TURBO_RUNTIME_BINARY_MIR_ABI_PTR:
      return MIR_T_P;
    case TURBO_RUNTIME_BINARY_MIR_ABI_I32:
      return MIR_T_I32;
    case TURBO_RUNTIME_BINARY_MIR_ABI_I64:
      return MIR_T_I64;
    case TURBO_RUNTIME_BINARY_MIR_ABI_DOUBLE:
      return MIR_T_D;
  }
  return MIR_T_UNDEF;
}

static MIR_type_t turbo_runtime_binary_mir_size_type(void) {
  return sizeof(size_t) == 4 ? MIR_T_I32 : MIR_T_I64;
}

static void *turbo_runtime_binary_mir_api_symbol_address(
    const turbo_runtime_data_bind_value_api_t *api,
    turbo_runtime_binary_mir_symbol_id_t id) {
  switch (id) {
    case TURBO_RUNTIME_BINARY_MIR_SYMBOL_CREATE_BOOL:
      return (void *)api->create_bool;
    case TURBO_RUNTIME_BINARY_MIR_SYMBOL_CREATE_INT64:
      return (void *)api->create_int64;
    case TURBO_RUNTIME_BINARY_MIR_SYMBOL_CREATE_DOUBLE:
      return (void *)api->create_double;
    case TURBO_RUNTIME_BINARY_MIR_SYMBOL_CREATE_STRING:
      return (void *)api->create_string;
    case TURBO_RUNTIME_BINARY_MIR_SYMBOL_CREATE_BYTES:
      return (void *)api->create_bytes;
    case TURBO_RUNTIME_BINARY_MIR_SYMBOL_CREATE_OBJECT:
      return (void *)api->create_object;
    case TURBO_RUNTIME_BINARY_MIR_SYMBOL_CREATE_ARRAY:
      return (void *)api->create_array;
    case TURBO_RUNTIME_BINARY_MIR_SYMBOL_OBJECT_SET:
      return (void *)api->object_set;
    case TURBO_RUNTIME_BINARY_MIR_SYMBOL_ARRAY_APPEND:
      return (void *)api->array_append;
    case TURBO_RUNTIME_BINARY_MIR_SYMBOL_DESTROY_VALUE:
      return (void *)api->destroy_value;
  }
  return NULL;
}

static turbo_runtime_binary_mir_status_t
turbo_runtime_binary_mir_declare_externals(
    MIR_context_t ctx, const turbo_runtime_binary_mir_plan_t *plan) {
  size_t i;

  for (i = 0; i < plan->symbol_count; ++i) {
    MIR_type_t result_type;
    MIR_var_t arguments[4];
    size_t j;
    MIR_item_t import_item;
    MIR_item_t proto_item;
    char proto_name[128];

    result_type = turbo_runtime_binary_mir_to_mir_type(
        plan->symbols[i].result_type);
    for (j = 0; j < plan->symbols[i].argument_count; ++j) {
      arguments[j].type = turbo_runtime_binary_mir_to_mir_type(
          plan->symbols[i].argument_types[j]);
      arguments[j].name = "";
      arguments[j].size = 0;
    }

    import_item = MIR_new_import(ctx, plan->symbols[i].name);
    strcpy(proto_name, "p_");
    strncat(proto_name, plan->symbols[i].name, sizeof(proto_name) - 3);
    proto_item = MIR_new_proto_arr(
        ctx, proto_name,
        plan->symbols[i].result_type == TURBO_RUNTIME_BINARY_MIR_ABI_VOID ? 0 : 1,
        plan->symbols[i].result_type == TURBO_RUNTIME_BINARY_MIR_ABI_VOID
            ? NULL
            : &result_type,
        plan->symbols[i].argument_count,
        plan->symbols[i].argument_count == 0 ? NULL : arguments);
    if (!import_item || !proto_item) {
      return TURBO_RUNTIME_BINARY_MIR_ERROR;
    }
  }

  return TURBO_RUNTIME_BINARY_MIR_OK;
}

static turbo_runtime_binary_mir_status_t
turbo_runtime_binary_mir_declare_symbol(MIR_context_t ctx,
                                        turbo_runtime_binary_mir_symbol_id_t id,
                                        MIR_item_t *out_import_item,
                                        MIR_item_t *out_proto_item) {
  const turbo_runtime_binary_mir_symbol_meta_t *meta;
  MIR_type_t result_type;
  MIR_var_t arguments[4];
  MIR_item_t import_item;
  MIR_item_t proto_item;
  char proto_name[128];
  size_t i;

  meta = turbo_runtime_binary_mir_symbol_meta(id);
  if (!ctx || !meta) {
    return TURBO_RUNTIME_BINARY_MIR_INVALID_ARGUMENT;
  }

  result_type = turbo_runtime_binary_mir_to_mir_type(meta->result_type);
  for (i = 0; i < meta->argument_count; ++i) {
    arguments[i].type = turbo_runtime_binary_mir_to_mir_type(meta->argument_types[i]);
    arguments[i].name = "";
    arguments[i].size = 0;
  }

  import_item = MIR_new_import(ctx, meta->name);
  strcpy(proto_name, "p_");
  strncat(proto_name, meta->name, sizeof(proto_name) - 3);
  proto_item = MIR_new_proto_arr(
      ctx, proto_name, meta->result_type == TURBO_RUNTIME_BINARY_MIR_ABI_VOID ? 0 : 1,
      meta->result_type == TURBO_RUNTIME_BINARY_MIR_ABI_VOID ? NULL : &result_type,
      meta->argument_count, meta->argument_count == 0 ? NULL : arguments);
  if (!import_item || !proto_item) {
    return TURBO_RUNTIME_BINARY_MIR_ERROR;
  }
  if (out_import_item) {
    *out_import_item = import_item;
  }
  if (out_proto_item) {
    *out_proto_item = proto_item;
  }
  return TURBO_RUNTIME_BINARY_MIR_OK;
}

static int turbo_runtime_binary_mir_schema_is_all_scalar(
    const turbo_runtime_binary_schema_t *schema) {
  size_t i;

  if (!schema) {
    return 0;
  }

  for (i = 0; i < turbo_runtime_binary_schema_field_count(schema); ++i) {
    turbo_runtime_binary_field_descriptor_t field = {0};

    if (turbo_runtime_binary_schema_get_field(schema, i, &field) !=
            TURBO_RUNTIME_BINARY_SCHEMA_OK ||
        field.shape != TURBO_RUNTIME_BINARY_FIELD_SCALAR) {
      return 0;
    }
  }

  return 1;
}

static int turbo_runtime_binary_mir_schema_is_all_repeated_scalar(
    const turbo_runtime_binary_schema_t *schema) {
  size_t i;

  if (!schema) {
    return 0;
  }

  for (i = 0; i < turbo_runtime_binary_schema_field_count(schema); ++i) {
    turbo_runtime_binary_field_descriptor_t field = {0};

    if (turbo_runtime_binary_schema_get_field(schema, i, &field) !=
            TURBO_RUNTIME_BINARY_SCHEMA_OK ||
        field.shape != TURBO_RUNTIME_BINARY_FIELD_REPEATED) {
      return 0;
    }
  }

  return 1;
}

static int turbo_runtime_binary_mir_schema_is_all_string_key_map_scalar(
    const turbo_runtime_binary_schema_t *schema) {
  size_t i;

  if (!schema) {
    return 0;
  }

  for (i = 0; i < turbo_runtime_binary_schema_field_count(schema); ++i) {
    turbo_runtime_binary_field_descriptor_t field = {0};

    if (turbo_runtime_binary_schema_get_field(schema, i, &field) !=
            TURBO_RUNTIME_BINARY_SCHEMA_OK ||
        field.shape != TURBO_RUNTIME_BINARY_FIELD_STRING_KEY_MAP) {
      return 0;
    }
  }

  return 1;
}

static int64_t turbo_runtime_binary_mir_reader_read_bool_value_bridge(
    turbo_runtime_binary_reader_t *reader);
static int64_t turbo_runtime_binary_mir_reader_read_i32_value_bridge(
    turbo_runtime_binary_reader_t *reader);
static int64_t turbo_runtime_binary_mir_reader_read_i64_value_bridge(
    turbo_runtime_binary_reader_t *reader);
static double turbo_runtime_binary_mir_reader_read_f32_value_bridge(
    turbo_runtime_binary_reader_t *reader);
static double turbo_runtime_binary_mir_reader_read_f64_value_bridge(
    turbo_runtime_binary_reader_t *reader);

static turbo_runtime_data_bind_value_t *
turbo_runtime_binary_mir_parse_string_value_bridge(
    turbo_runtime_binary_reader_t *reader,
    const turbo_runtime_data_bind_value_api_t *api, void *builder_user_data) {
  char *string_value = NULL;

  if (!reader || !api) {
    return NULL;
  }
  if (turbo_runtime_binary_reader_read_var_string16_copy(reader, &string_value, NULL) !=
      TURBO_RUNTIME_BINARY_READER_OK) {
    return NULL;
  }
  {
    turbo_runtime_data_bind_value_t *value =
        api->create_string(builder_user_data, string_value);
    free(string_value);
    return value;
  }
}

static turbo_runtime_data_bind_value_t *
turbo_runtime_binary_mir_parse_bytes_value_bridge(
    turbo_runtime_binary_reader_t *reader,
    const turbo_runtime_data_bind_value_api_t *api, void *builder_user_data) {
  const uint8_t *bytes_value;
  size_t bytes_size;

  if (!reader || !api) {
    return NULL;
  }
  if (turbo_runtime_binary_reader_read_var_bytes16(reader, &bytes_value, &bytes_size) !=
      TURBO_RUNTIME_BINARY_READER_OK) {
    return NULL;
  }
  return api->create_bytes(builder_user_data, bytes_value, bytes_size);
}

static turbo_runtime_data_bind_value_t *
turbo_runtime_binary_mir_parse_repeated_value_bridge(
    turbo_runtime_binary_value_type_t value_type, turbo_runtime_binary_reader_t *reader,
    const turbo_runtime_data_bind_value_api_t *api, void *builder_user_data) {
  uint32_t count;
  size_t i;
  turbo_runtime_data_bind_value_t *array_value;

  if (!reader || !api) {
    return NULL;
  }
  if (turbo_runtime_binary_reader_read_u32_le(reader, &count) !=
      TURBO_RUNTIME_BINARY_READER_OK) {
    return NULL;
  }

  array_value = api->create_array(builder_user_data);
  if (!array_value) {
    return NULL;
  }

  for (i = 0; i < (size_t)count; ++i) {
    turbo_runtime_data_bind_value_t *element_value = NULL;

    switch (value_type) {
      case TURBO_RUNTIME_BINARY_VALUE_BOOL:
        if (turbo_runtime_binary_reader_remaining(reader) < 1) {
          api->destroy_value(builder_user_data, array_value);
          return NULL;
        }
        element_value = api->create_bool(
            builder_user_data, turbo_runtime_binary_mir_reader_read_bool_value_bridge(reader));
        break;
      case TURBO_RUNTIME_BINARY_VALUE_I32:
        if (turbo_runtime_binary_reader_remaining(reader) < 4) {
          api->destroy_value(builder_user_data, array_value);
          return NULL;
        }
        element_value = api->create_int64(
            builder_user_data, turbo_runtime_binary_mir_reader_read_i32_value_bridge(reader));
        break;
      case TURBO_RUNTIME_BINARY_VALUE_I64:
        if (turbo_runtime_binary_reader_remaining(reader) < 8) {
          api->destroy_value(builder_user_data, array_value);
          return NULL;
        }
        element_value = api->create_int64(
            builder_user_data, turbo_runtime_binary_mir_reader_read_i64_value_bridge(reader));
        break;
      case TURBO_RUNTIME_BINARY_VALUE_F32:
        if (turbo_runtime_binary_reader_remaining(reader) < 4) {
          api->destroy_value(builder_user_data, array_value);
          return NULL;
        }
        element_value = api->create_double(
            builder_user_data, turbo_runtime_binary_mir_reader_read_f32_value_bridge(reader));
        break;
      case TURBO_RUNTIME_BINARY_VALUE_F64:
        if (turbo_runtime_binary_reader_remaining(reader) < 8) {
          api->destroy_value(builder_user_data, array_value);
          return NULL;
        }
        element_value = api->create_double(
            builder_user_data, turbo_runtime_binary_mir_reader_read_f64_value_bridge(reader));
        break;
      case TURBO_RUNTIME_BINARY_VALUE_STRING16:
        element_value = turbo_runtime_binary_mir_parse_string_value_bridge(
            reader, api, builder_user_data);
        break;
      case TURBO_RUNTIME_BINARY_VALUE_BYTES16:
        element_value = turbo_runtime_binary_mir_parse_bytes_value_bridge(
            reader, api, builder_user_data);
        break;
    }

    if (!element_value ||
        api->array_append(builder_user_data, array_value, element_value) !=
            TURBO_RUNTIME_DATA_BIND_OK) {
      if (element_value) {
        api->destroy_value(builder_user_data, element_value);
      }
      api->destroy_value(builder_user_data, array_value);
      return NULL;
    }
  }

  return array_value;
}

static turbo_runtime_data_bind_value_t *
turbo_runtime_binary_mir_parse_map_value_bridge(
    turbo_runtime_binary_value_type_t value_type, turbo_runtime_binary_reader_t *reader,
    const turbo_runtime_data_bind_value_api_t *api, void *builder_user_data) {
  uint32_t count;
  size_t i;
  turbo_runtime_data_bind_value_t *map_value;

  if (!reader || !api) {
    return NULL;
  }
  if (turbo_runtime_binary_reader_read_u32_le(reader, &count) !=
      TURBO_RUNTIME_BINARY_READER_OK) {
    return NULL;
  }

  map_value = api->create_object(builder_user_data);
  if (!map_value) {
    return NULL;
  }

  for (i = 0; i < (size_t)count; ++i) {
    char *key = NULL;
    turbo_runtime_data_bind_value_t *element_value = NULL;

    if (turbo_runtime_binary_reader_read_var_string16_copy(reader, &key, NULL) !=
        TURBO_RUNTIME_BINARY_READER_OK) {
      api->destroy_value(builder_user_data, map_value);
      return NULL;
    }

    switch (value_type) {
      case TURBO_RUNTIME_BINARY_VALUE_BOOL:
        if (turbo_runtime_binary_reader_remaining(reader) < 1) {
          free(key);
          api->destroy_value(builder_user_data, map_value);
          return NULL;
        }
        element_value = api->create_bool(
            builder_user_data, turbo_runtime_binary_mir_reader_read_bool_value_bridge(reader));
        break;
      case TURBO_RUNTIME_BINARY_VALUE_I32:
        if (turbo_runtime_binary_reader_remaining(reader) < 4) {
          free(key);
          api->destroy_value(builder_user_data, map_value);
          return NULL;
        }
        element_value = api->create_int64(
            builder_user_data, turbo_runtime_binary_mir_reader_read_i32_value_bridge(reader));
        break;
      case TURBO_RUNTIME_BINARY_VALUE_I64:
        if (turbo_runtime_binary_reader_remaining(reader) < 8) {
          free(key);
          api->destroy_value(builder_user_data, map_value);
          return NULL;
        }
        element_value = api->create_int64(
            builder_user_data, turbo_runtime_binary_mir_reader_read_i64_value_bridge(reader));
        break;
      case TURBO_RUNTIME_BINARY_VALUE_F32:
        if (turbo_runtime_binary_reader_remaining(reader) < 4) {
          free(key);
          api->destroy_value(builder_user_data, map_value);
          return NULL;
        }
        element_value = api->create_double(
            builder_user_data, turbo_runtime_binary_mir_reader_read_f32_value_bridge(reader));
        break;
      case TURBO_RUNTIME_BINARY_VALUE_F64:
        if (turbo_runtime_binary_reader_remaining(reader) < 8) {
          free(key);
          api->destroy_value(builder_user_data, map_value);
          return NULL;
        }
        element_value = api->create_double(
            builder_user_data, turbo_runtime_binary_mir_reader_read_f64_value_bridge(reader));
        break;
      case TURBO_RUNTIME_BINARY_VALUE_STRING16:
        element_value = turbo_runtime_binary_mir_parse_string_value_bridge(
            reader, api, builder_user_data);
        break;
      case TURBO_RUNTIME_BINARY_VALUE_BYTES16:
        element_value = turbo_runtime_binary_mir_parse_bytes_value_bridge(
            reader, api, builder_user_data);
        break;
    }

    if (!element_value ||
        api->object_set(builder_user_data, map_value, key, element_value) !=
            TURBO_RUNTIME_DATA_BIND_OK) {
      free(key);
      if (element_value) {
        api->destroy_value(builder_user_data, element_value);
      }
      api->destroy_value(builder_user_data, map_value);
      return NULL;
    }
    free(key);
  }

  return map_value;
}

static int64_t turbo_runtime_binary_mir_reader_read_bool_value_bridge(
    turbo_runtime_binary_reader_t *reader) {
  uint8_t value = 0;

  if (!reader) {
    return 0;
  }
  turbo_runtime_binary_reader_read_u8(reader, &value);
  return value != 0 ? 1 : 0;
}

static int64_t turbo_runtime_binary_mir_reader_read_i32_value_bridge(
    turbo_runtime_binary_reader_t *reader) {
  int32_t value = 0;

  if (!reader) {
    return 0;
  }
  turbo_runtime_binary_reader_read_i32_le(reader, &value);
  return (int64_t)value;
}

static int64_t turbo_runtime_binary_mir_reader_read_i64_value_bridge(
    turbo_runtime_binary_reader_t *reader) {
  int64_t value = 0;

  if (!reader) {
    return 0;
  }
  turbo_runtime_binary_reader_read_i64_le(reader, &value);
  return value;
}

static double turbo_runtime_binary_mir_reader_read_f32_value_bridge(
    turbo_runtime_binary_reader_t *reader) {
  float value = 0.0f;

  if (!reader) {
    return 0.0;
  }
  turbo_runtime_binary_reader_read_f32_le(reader, &value);
  return (double)value;
}

static double turbo_runtime_binary_mir_reader_read_f64_value_bridge(
    turbo_runtime_binary_reader_t *reader) {
  double value = 0.0;

  if (!reader) {
    return 0.0;
  }
  turbo_runtime_binary_reader_read_f64_le(reader, &value);
  return value;
}

static turbo_runtime_data_bind_value_t *turbo_runtime_binary_mir_parse_bridge(
    const turbo_runtime_binary_schema_t *schema,
    const turbo_runtime_data_bind_value_api_t *api, const uint8_t *data,
    size_t size, void *builder_user_data) {
  if (!schema || !api) {
    return NULL;
  }

  return turbo_runtime_binary_schema_parse(schema, data, size, api,
                                           builder_user_data, NULL, 0);
}

static turbo_runtime_binary_reader_t *turbo_runtime_binary_mir_reader_create_bridge(
    const uint8_t *data, size_t size) { 
  turbo_runtime_binary_reader_t *heap_reader;

  heap_reader = (turbo_runtime_binary_reader_t *)malloc(sizeof(*heap_reader));
  if (!heap_reader) {
    return NULL;
  }
  turbo_runtime_binary_reader_init(heap_reader, data, size);
  return heap_reader;
}

static void turbo_runtime_binary_mir_reader_destroy_bridge(
    turbo_runtime_binary_reader_t *reader) {
  free(reader);
}

static size_t turbo_runtime_binary_mir_reader_remaining_bridge(
    const turbo_runtime_binary_reader_t *reader) {
  return turbo_runtime_binary_reader_remaining(reader);
}

static void turbo_runtime_binary_mir_append_reader_remaining_guard(
    MIR_context_t ctx, MIR_item_t func_item, MIR_item_t remaining_proto_item,
    MIR_item_t remaining_import_item, MIR_reg_t status_reg, MIR_reg_t reader_reg,
    MIR_label_t fail_label, int64_t minimum_bytes) {
  MIR_append_insn(ctx, func_item,
                  MIR_new_call_insn(ctx, 4,
                                    MIR_new_ref_op(ctx, remaining_proto_item),
                                    MIR_new_ref_op(ctx, remaining_import_item),
                                    MIR_new_reg_op(ctx, status_reg),
                                    MIR_new_reg_op(ctx, reader_reg)));
  MIR_append_insn(ctx, func_item,
                  MIR_new_insn(ctx, MIR_UBLT, MIR_new_label_op(ctx, fail_label),
                               MIR_new_reg_op(ctx, status_reg),
                               MIR_new_int_op(ctx, minimum_bytes)));
}

static void turbo_runtime_binary_mir_append_fixed_scalar_value(
    MIR_context_t ctx, MIR_item_t func_item, MIR_item_t remaining_proto_item,
    MIR_item_t remaining_import_item, MIR_item_t read_proto_item,
    MIR_item_t read_import_item, MIR_item_t create_proto_item,
    MIR_item_t create_import_item, MIR_reg_t status_reg, MIR_reg_t reader_reg,
    MIR_reg_t value_reg, MIR_reg_t field_value_reg, MIR_reg_t builder_user_data_reg,
    MIR_label_t fail_label, int64_t minimum_bytes) {
  turbo_runtime_binary_mir_append_reader_remaining_guard(
      ctx, func_item, remaining_proto_item, remaining_import_item, status_reg,
      reader_reg, fail_label, minimum_bytes);
  MIR_append_insn(ctx, func_item,
                  MIR_new_call_insn(ctx, 4, MIR_new_ref_op(ctx, read_proto_item),
                                    MIR_new_ref_op(ctx, read_import_item),
                                    MIR_new_reg_op(ctx, value_reg),
                                    MIR_new_reg_op(ctx, reader_reg)));
  MIR_append_insn(ctx, func_item,
                  MIR_new_call_insn(ctx, 5, MIR_new_ref_op(ctx, create_proto_item),
                                    MIR_new_ref_op(ctx, create_import_item),
                                    MIR_new_reg_op(ctx, field_value_reg),
                                    MIR_new_reg_op(ctx, builder_user_data_reg),
                                    MIR_new_reg_op(ctx, value_reg)));
}

static void turbo_runtime_binary_mir_append_variable_scalar_value(
    MIR_context_t ctx, MIR_item_t func_item, MIR_item_t parse_proto_item,
    MIR_item_t parse_import_item, MIR_reg_t field_value_reg, MIR_reg_t reader_reg,
    const turbo_runtime_data_bind_value_api_t *api, MIR_reg_t builder_user_data_reg) {
  MIR_append_insn(ctx, func_item,
                  MIR_new_call_insn(ctx, 6, MIR_new_ref_op(ctx, parse_proto_item),
                                    MIR_new_ref_op(ctx, parse_import_item),
                                    MIR_new_reg_op(ctx, field_value_reg),
                                    MIR_new_reg_op(ctx, reader_reg),
                                    MIR_new_uint_op(ctx, (uint64_t)(uintptr_t)api),
                                    MIR_new_reg_op(ctx, builder_user_data_reg)));
}

static void turbo_runtime_binary_mir_append_repeated_scalar_value(
    MIR_context_t ctx, MIR_item_t func_item, MIR_item_t parse_proto_item,
    MIR_item_t parse_import_item, MIR_reg_t field_value_reg,
    turbo_runtime_binary_value_type_t value_type, MIR_reg_t reader_reg,
    const turbo_runtime_data_bind_value_api_t *api, MIR_reg_t builder_user_data_reg) {
  MIR_append_insn(ctx, func_item,
                  MIR_new_call_insn(ctx, 7, MIR_new_ref_op(ctx, parse_proto_item),
                                    MIR_new_ref_op(ctx, parse_import_item),
                                    MIR_new_reg_op(ctx, field_value_reg),
                                    MIR_new_int_op(ctx, (int64_t)value_type),
                                    MIR_new_reg_op(ctx, reader_reg),
                                    MIR_new_uint_op(ctx, (uint64_t)(uintptr_t)api),
                                    MIR_new_reg_op(ctx, builder_user_data_reg)));
}

static void turbo_runtime_binary_mir_append_map_scalar_value(
    MIR_context_t ctx, MIR_item_t func_item, MIR_item_t parse_proto_item,
    MIR_item_t parse_import_item, MIR_reg_t field_value_reg,
    turbo_runtime_binary_value_type_t value_type, MIR_reg_t reader_reg,
    const turbo_runtime_data_bind_value_api_t *api, MIR_reg_t builder_user_data_reg) {
  MIR_append_insn(ctx, func_item,
                  MIR_new_call_insn(ctx, 7, MIR_new_ref_op(ctx, parse_proto_item),
                                    MIR_new_ref_op(ctx, parse_import_item),
                                    MIR_new_reg_op(ctx, field_value_reg),
                                    MIR_new_int_op(ctx, (int64_t)value_type),
                                    MIR_new_reg_op(ctx, reader_reg),
                                    MIR_new_uint_op(ctx, (uint64_t)(uintptr_t)api),
                                    MIR_new_reg_op(ctx, builder_user_data_reg)));
}

static void turbo_runtime_binary_mir_append_object_set_or_cleanup(
    MIR_context_t ctx, MIR_item_t func_item, MIR_item_t object_set_proto_item,
    MIR_item_t object_set_import_item, MIR_item_t destroy_value_proto_item,
    MIR_item_t destroy_value_import_item, MIR_reg_t status_reg,
    MIR_reg_t builder_user_data_reg, MIR_reg_t object_reg, const char *field_name,
    MIR_reg_t field_value_reg, MIR_label_t next_label, MIR_label_t fail_label) {
  MIR_append_insn(ctx, func_item,
                  MIR_new_call_insn(ctx, 7,
                                    MIR_new_ref_op(ctx, object_set_proto_item),
                                    MIR_new_ref_op(ctx, object_set_import_item),
                                    MIR_new_reg_op(ctx, status_reg),
                                    MIR_new_reg_op(ctx, builder_user_data_reg),
                                    MIR_new_reg_op(ctx, object_reg),
                                    MIR_new_uint_op(ctx, (uint64_t)(uintptr_t)field_name),
                                    MIR_new_reg_op(ctx, field_value_reg)));
  MIR_append_insn(ctx, func_item,
                  MIR_new_insn(ctx, MIR_BEQ, MIR_new_label_op(ctx, next_label),
                               MIR_new_reg_op(ctx, status_reg),
                               MIR_new_int_op(ctx, 0)));
  MIR_append_insn(ctx, func_item,
                  MIR_new_call_insn(ctx, 4,
                                    MIR_new_ref_op(ctx, destroy_value_proto_item),
                                    MIR_new_ref_op(ctx, destroy_value_import_item),
                                    MIR_new_reg_op(ctx, builder_user_data_reg),
                                    MIR_new_reg_op(ctx, field_value_reg)));
  MIR_append_insn(ctx, func_item,
                  MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, fail_label)));
}

static void turbo_runtime_binary_mir_append_fast_path_prologue(
    MIR_context_t ctx, MIR_item_t func_item, MIR_item_t reader_create_proto_item,
    MIR_item_t reader_create_import_item, MIR_item_t create_object_proto_item,
    MIR_item_t create_object_import_item, MIR_reg_t reader_reg, MIR_reg_t data_reg,
    MIR_reg_t size_reg, MIR_reg_t object_reg, MIR_reg_t builder_user_data_reg,
    MIR_label_t fail_return_label, MIR_label_t fail_label) {
  MIR_append_insn(ctx, func_item,
                  MIR_new_call_insn(ctx, 5, MIR_new_ref_op(ctx, reader_create_proto_item),
                                    MIR_new_ref_op(ctx, reader_create_import_item),
                                    MIR_new_reg_op(ctx, reader_reg),
                                    MIR_new_reg_op(ctx, data_reg),
                                    MIR_new_reg_op(ctx, size_reg)));
  MIR_append_insn(ctx, func_item,
                  MIR_new_insn(ctx, MIR_BF, MIR_new_label_op(ctx, fail_return_label),
                               MIR_new_reg_op(ctx, reader_reg)));
  MIR_append_insn(ctx, func_item,
                  MIR_new_call_insn(ctx, 4, MIR_new_ref_op(ctx, create_object_proto_item),
                                    MIR_new_ref_op(ctx, create_object_import_item),
                                    MIR_new_reg_op(ctx, object_reg),
                                    MIR_new_reg_op(ctx, builder_user_data_reg)));
  MIR_append_insn(ctx, func_item,
                  MIR_new_insn(ctx, MIR_BF, MIR_new_label_op(ctx, fail_label),
                               MIR_new_reg_op(ctx, object_reg)));
}

static void turbo_runtime_binary_mir_append_fast_path_epilogue(
    MIR_context_t ctx, MIR_item_t func_item, MIR_item_t reader_remaining_proto_item,
    MIR_item_t reader_remaining_import_item, MIR_item_t reader_destroy_proto_item,
    MIR_item_t reader_destroy_import_item, MIR_item_t destroy_value_proto_item,
    MIR_item_t destroy_value_import_item, MIR_reg_t status_reg, MIR_reg_t reader_reg,
    MIR_reg_t builder_user_data_reg, MIR_reg_t object_reg, MIR_label_t fail_label,
    MIR_label_t fail_return_label) {
  MIR_append_insn(ctx, func_item,
                  MIR_new_call_insn(ctx, 4,
                                    MIR_new_ref_op(ctx, reader_remaining_proto_item),
                                    MIR_new_ref_op(ctx, reader_remaining_import_item),
                                    MIR_new_reg_op(ctx, status_reg),
                                    MIR_new_reg_op(ctx, reader_reg)));
  MIR_append_insn(ctx, func_item,
                  MIR_new_insn(ctx, MIR_BNE, MIR_new_label_op(ctx, fail_label),
                               MIR_new_reg_op(ctx, status_reg),
                               MIR_new_int_op(ctx, 0)));
  MIR_append_insn(ctx, func_item,
                  MIR_new_call_insn(ctx, 3, MIR_new_ref_op(ctx, reader_destroy_proto_item),
                                    MIR_new_ref_op(ctx, reader_destroy_import_item),
                                    MIR_new_reg_op(ctx, reader_reg)));
  MIR_append_insn(ctx, func_item,
                  MIR_new_ret_insn(ctx, 1, MIR_new_reg_op(ctx, object_reg)));
  MIR_append_insn(ctx, func_item, fail_label);
  MIR_append_insn(ctx, func_item,
                  MIR_new_call_insn(ctx, 3, MIR_new_ref_op(ctx, reader_destroy_proto_item),
                                    MIR_new_ref_op(ctx, reader_destroy_import_item),
                                    MIR_new_reg_op(ctx, reader_reg)));
  MIR_append_insn(ctx, func_item,
                  MIR_new_call_insn(ctx, 4, MIR_new_ref_op(ctx, destroy_value_proto_item),
                                    MIR_new_ref_op(ctx, destroy_value_import_item),
                                    MIR_new_reg_op(ctx, builder_user_data_reg),
                                    MIR_new_reg_op(ctx, object_reg)));
  MIR_append_insn(ctx, func_item, fail_return_label);
  MIR_append_insn(ctx, func_item, MIR_new_ret_insn(ctx, 1, MIR_new_int_op(ctx, 0)));
}

static void turbo_runtime_binary_mir_append_field_value(
    const turbo_runtime_binary_mir_field_codegen_ctx_t *codegen,
    turbo_runtime_binary_mir_fast_path_kind_t fast_path_kind,
    turbo_runtime_binary_value_type_t value_type) {
  if (fast_path_kind == TURBO_RUNTIME_BINARY_MIR_FAST_PATH_REPEATED) {
    turbo_runtime_binary_mir_append_repeated_scalar_value(
        codegen->ctx, codegen->func_item, codegen->repeated_value_proto_item,
        codegen->repeated_value_import_item, codegen->field_value_reg, value_type,
        codegen->reader_reg, codegen->api, codegen->builder_user_data_reg);
    return;
  }
  if (fast_path_kind == TURBO_RUNTIME_BINARY_MIR_FAST_PATH_MAP) {
    turbo_runtime_binary_mir_append_map_scalar_value(
        codegen->ctx, codegen->func_item, codegen->map_value_proto_item,
        codegen->map_value_import_item, codegen->field_value_reg, value_type,
        codegen->reader_reg, codegen->api, codegen->builder_user_data_reg);
    return;
  }

  switch (value_type) {
    case TURBO_RUNTIME_BINARY_VALUE_BOOL:
      turbo_runtime_binary_mir_append_fixed_scalar_value(
          codegen->ctx, codegen->func_item, codegen->reader_remaining_proto_item,
          codegen->reader_remaining_import_item, codegen->read_bool_proto_item,
          codegen->read_bool_import_item, codegen->create_bool_proto_item,
          codegen->create_bool_import_item, codegen->status_reg, codegen->reader_reg,
          codegen->bool_reg, codegen->field_value_reg, codegen->builder_user_data_reg,
          codegen->fail_label, 1);
      break;
    case TURBO_RUNTIME_BINARY_VALUE_I32:
      turbo_runtime_binary_mir_append_fixed_scalar_value(
          codegen->ctx, codegen->func_item, codegen->reader_remaining_proto_item,
          codegen->reader_remaining_import_item, codegen->read_i32_proto_item,
          codegen->read_i32_import_item, codegen->create_int64_proto_item,
          codegen->create_int64_import_item, codegen->status_reg, codegen->reader_reg,
          codegen->int64_reg, codegen->field_value_reg, codegen->builder_user_data_reg,
          codegen->fail_label, 4);
      break;
    case TURBO_RUNTIME_BINARY_VALUE_I64:
      turbo_runtime_binary_mir_append_fixed_scalar_value(
          codegen->ctx, codegen->func_item, codegen->reader_remaining_proto_item,
          codegen->reader_remaining_import_item, codegen->read_i64_proto_item,
          codegen->read_i64_import_item, codegen->create_int64_proto_item,
          codegen->create_int64_import_item, codegen->status_reg, codegen->reader_reg,
          codegen->int64_reg, codegen->field_value_reg, codegen->builder_user_data_reg,
          codegen->fail_label, 8);
      break;
    case TURBO_RUNTIME_BINARY_VALUE_F32:
      turbo_runtime_binary_mir_append_fixed_scalar_value(
          codegen->ctx, codegen->func_item, codegen->reader_remaining_proto_item,
          codegen->reader_remaining_import_item, codegen->read_f32_proto_item,
          codegen->read_f32_import_item, codegen->create_double_proto_item,
          codegen->create_double_import_item, codegen->status_reg, codegen->reader_reg,
          codegen->double_reg, codegen->field_value_reg, codegen->builder_user_data_reg,
          codegen->fail_label, 4);
      break;
    case TURBO_RUNTIME_BINARY_VALUE_F64:
      turbo_runtime_binary_mir_append_fixed_scalar_value(
          codegen->ctx, codegen->func_item, codegen->reader_remaining_proto_item,
          codegen->reader_remaining_import_item, codegen->read_f64_proto_item,
          codegen->read_f64_import_item, codegen->create_double_proto_item,
          codegen->create_double_import_item, codegen->status_reg, codegen->reader_reg,
          codegen->double_reg, codegen->field_value_reg, codegen->builder_user_data_reg,
          codegen->fail_label, 8);
      break;
    case TURBO_RUNTIME_BINARY_VALUE_STRING16:
      turbo_runtime_binary_mir_append_variable_scalar_value(
          codegen->ctx, codegen->func_item, codegen->string_value_proto_item,
          codegen->string_value_import_item, codegen->field_value_reg,
          codegen->reader_reg, codegen->api, codegen->builder_user_data_reg);
      break;
    case TURBO_RUNTIME_BINARY_VALUE_BYTES16:
      turbo_runtime_binary_mir_append_variable_scalar_value(
          codegen->ctx, codegen->func_item, codegen->bytes_value_proto_item,
          codegen->bytes_value_import_item, codegen->field_value_reg,
          codegen->reader_reg, codegen->api, codegen->builder_user_data_reg);
      break;
  }
}

static turbo_runtime_binary_mir_status_t
turbo_runtime_binary_mir_declare_parse_bridge(MIR_context_t ctx,
                                              MIR_item_t *out_import_item,
                                              MIR_item_t *out_proto_item) {
  MIR_type_t result_type;
  MIR_var_t arguments[5];
  MIR_item_t import_item;
  MIR_item_t proto_item;
  size_t i;
  MIR_type_t size_type;

  if (!ctx) {
    return TURBO_RUNTIME_BINARY_MIR_INVALID_ARGUMENT;
  }

  result_type = MIR_T_P;
  size_type = turbo_runtime_binary_mir_size_type();
  for (i = 0; i < 5; ++i) {
    arguments[i].type = (i == 3) ? size_type : MIR_T_P;
    arguments[i].name = "";
    arguments[i].size = 0;
  }

  import_item = MIR_new_import(ctx, "turbo_runtime_binary_mir_parse_bridge");
  proto_item = MIR_new_proto_arr(ctx, "p_turbo_runtime_binary_mir_parse_bridge",
                                 1, &result_type, 5, arguments);
  if (!import_item || !proto_item) {
    return TURBO_RUNTIME_BINARY_MIR_ERROR;
  }

  if (out_import_item) {
    *out_import_item = import_item;
  }
  if (out_proto_item) {
    *out_proto_item = proto_item;
  }
  return TURBO_RUNTIME_BINARY_MIR_OK;
}

static turbo_runtime_binary_mir_status_t
turbo_runtime_binary_mir_declare_helper_proto(
    MIR_context_t ctx, const char *import_name, const char *proto_name,
    MIR_type_t result_type, size_t argument_count, MIR_type_t *argument_types,
    MIR_item_t *out_import_item, MIR_item_t *out_proto_item) {
  MIR_var_t arguments[6];
  MIR_item_t import_item;
  MIR_item_t proto_item;
  size_t i;

  if (!ctx || !import_name || !proto_name) {
    return TURBO_RUNTIME_BINARY_MIR_INVALID_ARGUMENT;
  }

  for (i = 0; i < argument_count; ++i) {
    arguments[i].type = argument_types[i];
    arguments[i].name = "";
    arguments[i].size = 0;
  }

  import_item = MIR_new_import(ctx, import_name);
  proto_item = MIR_new_proto_arr(ctx, proto_name,
                                 result_type == MIR_T_UNDEF ? 0 : 1,
                                 result_type == MIR_T_UNDEF ? NULL : &result_type,
                                 argument_count,
                                 argument_count == 0 ? NULL : arguments);
  if (!import_item || !proto_item) {
    return TURBO_RUNTIME_BINARY_MIR_ERROR;
  }
  if (out_import_item) {
    *out_import_item = import_item;
  }
  if (out_proto_item) {
    *out_proto_item = proto_item;
  }
  return TURBO_RUNTIME_BINARY_MIR_OK;
}

static turbo_runtime_binary_mir_status_t
turbo_runtime_binary_mir_load_api_symbol(
    MIR_context_t ctx, const turbo_runtime_data_bind_value_api_t *api,
    turbo_runtime_binary_mir_symbol_id_t id, int required, char *error_buffer,
    size_t error_buffer_size) {
  const turbo_runtime_binary_mir_symbol_meta_t *meta;
  void *address;

  meta = turbo_runtime_binary_mir_symbol_meta(id);
  if (!ctx || !api || !meta) {
    return TURBO_RUNTIME_BINARY_MIR_INVALID_ARGUMENT;
  }

  address = turbo_runtime_binary_mir_api_symbol_address(api, id);
  if (!address) {
    if (!required) {
      return TURBO_RUNTIME_BINARY_MIR_OK;
    }
    turbo_runtime_binary_mir_write_error(error_buffer, error_buffer_size,
                                         "missing api callback for mir symbol");
    return TURBO_RUNTIME_BINARY_MIR_UNSUPPORTED;
  }

  MIR_load_external(ctx, meta->name, address);
  return TURBO_RUNTIME_BINARY_MIR_OK;
}

static void turbo_runtime_binary_mir_load_fast_path_helpers(MIR_context_t ctx) {
  MIR_load_external(ctx, "turbo_runtime_binary_mir_reader_create_bridge",
                    (void *)turbo_runtime_binary_mir_reader_create_bridge);
  MIR_load_external(ctx, "turbo_runtime_binary_mir_reader_destroy_bridge",
                    (void *)turbo_runtime_binary_mir_reader_destroy_bridge);
  MIR_load_external(ctx, "turbo_runtime_binary_mir_reader_remaining_bridge",
                    (void *)turbo_runtime_binary_mir_reader_remaining_bridge);
  MIR_load_external(ctx, "turbo_runtime_binary_mir_reader_read_bool_value_bridge",
                    (void *)turbo_runtime_binary_mir_reader_read_bool_value_bridge);
  MIR_load_external(ctx, "turbo_runtime_binary_mir_reader_read_i32_value_bridge",
                    (void *)turbo_runtime_binary_mir_reader_read_i32_value_bridge);
  MIR_load_external(ctx, "turbo_runtime_binary_mir_reader_read_i64_value_bridge",
                    (void *)turbo_runtime_binary_mir_reader_read_i64_value_bridge);
  MIR_load_external(ctx, "turbo_runtime_binary_mir_reader_read_f32_value_bridge",
                    (void *)turbo_runtime_binary_mir_reader_read_f32_value_bridge);
  MIR_load_external(ctx, "turbo_runtime_binary_mir_reader_read_f64_value_bridge",
                    (void *)turbo_runtime_binary_mir_reader_read_f64_value_bridge);
  MIR_load_external(ctx, "turbo_runtime_binary_mir_parse_string_value_bridge",
                    (void *)turbo_runtime_binary_mir_parse_string_value_bridge);
  MIR_load_external(ctx, "turbo_runtime_binary_mir_parse_bytes_value_bridge",
                    (void *)turbo_runtime_binary_mir_parse_bytes_value_bridge);
  MIR_load_external(ctx, "turbo_runtime_binary_mir_parse_repeated_value_bridge",
                    (void *)turbo_runtime_binary_mir_parse_repeated_value_bridge);
  MIR_load_external(ctx, "turbo_runtime_binary_mir_parse_map_value_bridge",
                    (void *)turbo_runtime_binary_mir_parse_map_value_bridge);
}

static turbo_runtime_binary_mir_status_t
turbo_runtime_binary_mir_declare_fast_path_items(
    MIR_context_t ctx, turbo_runtime_binary_mir_fast_path_items_t *items) {
  MIR_type_t reader_create_arguments[2] = {MIR_T_P,
                                           turbo_runtime_binary_mir_size_type()};
  MIR_type_t reader_destroy_arguments[1] = {MIR_T_P};
  MIR_type_t reader_remaining_arguments[1] = {MIR_T_P};
  MIR_type_t read_bool_arguments[1] = {MIR_T_P};
  MIR_type_t read_i64_arguments[1] = {MIR_T_P};
  MIR_type_t read_double_arguments[1] = {MIR_T_P};
  MIR_type_t variable_value_arguments[3] = {MIR_T_P, MIR_T_P, MIR_T_P};
  MIR_type_t repeated_value_arguments[4] = {MIR_T_I64, MIR_T_P, MIR_T_P, MIR_T_P};

  if (!ctx || !items) {
    return TURBO_RUNTIME_BINARY_MIR_INVALID_ARGUMENT;
  }

  if (turbo_runtime_binary_mir_declare_symbol(
          ctx, TURBO_RUNTIME_BINARY_MIR_SYMBOL_CREATE_BOOL,
          &items->create_bool.import_item,
          &items->create_bool.proto_item) != TURBO_RUNTIME_BINARY_MIR_OK ||
      turbo_runtime_binary_mir_declare_symbol(
          ctx, TURBO_RUNTIME_BINARY_MIR_SYMBOL_CREATE_INT64,
          &items->create_int64.import_item,
          &items->create_int64.proto_item) != TURBO_RUNTIME_BINARY_MIR_OK ||
      turbo_runtime_binary_mir_declare_symbol(
          ctx, TURBO_RUNTIME_BINARY_MIR_SYMBOL_CREATE_DOUBLE,
          &items->create_double.import_item,
          &items->create_double.proto_item) != TURBO_RUNTIME_BINARY_MIR_OK ||
      turbo_runtime_binary_mir_declare_symbol(
          ctx, TURBO_RUNTIME_BINARY_MIR_SYMBOL_CREATE_OBJECT,
          &items->create_object.import_item,
          &items->create_object.proto_item) != TURBO_RUNTIME_BINARY_MIR_OK ||
      turbo_runtime_binary_mir_declare_symbol(
          ctx, TURBO_RUNTIME_BINARY_MIR_SYMBOL_OBJECT_SET,
          &items->object_set.import_item,
          &items->object_set.proto_item) != TURBO_RUNTIME_BINARY_MIR_OK ||
      turbo_runtime_binary_mir_declare_symbol(
          ctx, TURBO_RUNTIME_BINARY_MIR_SYMBOL_DESTROY_VALUE,
          &items->destroy_value.import_item,
          &items->destroy_value.proto_item) != TURBO_RUNTIME_BINARY_MIR_OK ||
      turbo_runtime_binary_mir_declare_helper_proto(
          ctx, "turbo_runtime_binary_mir_reader_create_bridge",
          "p_turbo_runtime_binary_mir_reader_create_bridge", MIR_T_P, 2,
          reader_create_arguments, &items->reader_create.import_item,
          &items->reader_create.proto_item) != TURBO_RUNTIME_BINARY_MIR_OK ||
      turbo_runtime_binary_mir_declare_helper_proto(
          ctx, "turbo_runtime_binary_mir_reader_destroy_bridge",
          "p_turbo_runtime_binary_mir_reader_destroy_bridge", MIR_T_UNDEF, 1,
          reader_destroy_arguments, &items->reader_destroy.import_item,
          &items->reader_destroy.proto_item) != TURBO_RUNTIME_BINARY_MIR_OK ||
      turbo_runtime_binary_mir_declare_helper_proto(
          ctx, "turbo_runtime_binary_mir_reader_remaining_bridge",
          "p_turbo_runtime_binary_mir_reader_remaining_bridge",
          turbo_runtime_binary_mir_size_type(), 1, reader_remaining_arguments,
          &items->reader_remaining.import_item,
          &items->reader_remaining.proto_item) != TURBO_RUNTIME_BINARY_MIR_OK ||
      turbo_runtime_binary_mir_declare_helper_proto(
          ctx, "turbo_runtime_binary_mir_reader_read_bool_value_bridge",
          "p_turbo_runtime_binary_mir_reader_read_bool_value_bridge", MIR_T_I64, 1,
          read_bool_arguments, &items->read_bool.import_item,
          &items->read_bool.proto_item) != TURBO_RUNTIME_BINARY_MIR_OK ||
      turbo_runtime_binary_mir_declare_helper_proto(
          ctx, "turbo_runtime_binary_mir_reader_read_i32_value_bridge",
          "p_turbo_runtime_binary_mir_reader_read_i32_value_bridge", MIR_T_I64, 1,
          read_i64_arguments, &items->read_i32.import_item,
          &items->read_i32.proto_item) != TURBO_RUNTIME_BINARY_MIR_OK ||
      turbo_runtime_binary_mir_declare_helper_proto(
          ctx, "turbo_runtime_binary_mir_reader_read_i64_value_bridge",
          "p_turbo_runtime_binary_mir_reader_read_i64_value_bridge", MIR_T_I64, 1,
          read_i64_arguments, &items->read_i64.import_item,
          &items->read_i64.proto_item) != TURBO_RUNTIME_BINARY_MIR_OK ||
      turbo_runtime_binary_mir_declare_helper_proto(
          ctx, "turbo_runtime_binary_mir_reader_read_f32_value_bridge",
          "p_turbo_runtime_binary_mir_reader_read_f32_value_bridge", MIR_T_D, 1,
          read_double_arguments, &items->read_f32.import_item,
          &items->read_f32.proto_item) != TURBO_RUNTIME_BINARY_MIR_OK ||
      turbo_runtime_binary_mir_declare_helper_proto(
          ctx, "turbo_runtime_binary_mir_reader_read_f64_value_bridge",
          "p_turbo_runtime_binary_mir_reader_read_f64_value_bridge", MIR_T_D, 1,
          read_double_arguments, &items->read_f64.import_item,
          &items->read_f64.proto_item) != TURBO_RUNTIME_BINARY_MIR_OK ||
      turbo_runtime_binary_mir_declare_helper_proto(
          ctx, "turbo_runtime_binary_mir_parse_string_value_bridge",
          "p_turbo_runtime_binary_mir_parse_string_value_bridge", MIR_T_P, 3,
          variable_value_arguments, &items->string_value.import_item,
          &items->string_value.proto_item) != TURBO_RUNTIME_BINARY_MIR_OK ||
      turbo_runtime_binary_mir_declare_helper_proto(
          ctx, "turbo_runtime_binary_mir_parse_bytes_value_bridge",
          "p_turbo_runtime_binary_mir_parse_bytes_value_bridge", MIR_T_P, 3,
          variable_value_arguments, &items->bytes_value.import_item,
          &items->bytes_value.proto_item) != TURBO_RUNTIME_BINARY_MIR_OK ||
      turbo_runtime_binary_mir_declare_helper_proto(
          ctx, "turbo_runtime_binary_mir_parse_repeated_value_bridge",
          "p_turbo_runtime_binary_mir_parse_repeated_value_bridge", MIR_T_P, 4,
          repeated_value_arguments, &items->repeated_value.import_item,
          &items->repeated_value.proto_item) != TURBO_RUNTIME_BINARY_MIR_OK ||
      turbo_runtime_binary_mir_declare_helper_proto(
          ctx, "turbo_runtime_binary_mir_parse_map_value_bridge",
          "p_turbo_runtime_binary_mir_parse_map_value_bridge", MIR_T_P, 4,
          repeated_value_arguments, &items->map_value.import_item,
          &items->map_value.proto_item) != TURBO_RUNTIME_BINARY_MIR_OK) {
    return TURBO_RUNTIME_BINARY_MIR_ERROR;
  }

  return TURBO_RUNTIME_BINARY_MIR_OK;
}

turbo_runtime_binary_mir_plan_t *
turbo_runtime_binary_mir_plan_create(
    const turbo_runtime_binary_schema_t *schema) {
  turbo_runtime_binary_abi_requirements_t requirements;
  turbo_runtime_binary_mir_plan_t *plan;

  if (!schema) {
    return NULL;
  }

  plan = (turbo_runtime_binary_mir_plan_t *)calloc(1, sizeof(*plan));
  if (!plan) {
    return NULL;
  }

  memset(&requirements, 0, sizeof(requirements));
  if (turbo_runtime_binary_schema_collect_abi_requirements(schema,
                                                           &requirements) !=
      TURBO_RUNTIME_BINARY_SCHEMA_OK) {
    turbo_runtime_binary_mir_plan_destroy(plan);
    return NULL;
  }

  if (turbo_runtime_binary_mir_plan_add_if_needed(
          plan, requirements.create_bool,
          TURBO_RUNTIME_BINARY_MIR_SYMBOL_CREATE_BOOL) !=
      TURBO_RUNTIME_BINARY_MIR_OK ||
      turbo_runtime_binary_mir_plan_add_if_needed(
          plan, requirements.create_int64,
          TURBO_RUNTIME_BINARY_MIR_SYMBOL_CREATE_INT64) !=
          TURBO_RUNTIME_BINARY_MIR_OK ||
      turbo_runtime_binary_mir_plan_add_if_needed(
          plan, requirements.create_double,
          TURBO_RUNTIME_BINARY_MIR_SYMBOL_CREATE_DOUBLE) !=
          TURBO_RUNTIME_BINARY_MIR_OK ||
      turbo_runtime_binary_mir_plan_add_if_needed(
          plan, requirements.create_string,
          TURBO_RUNTIME_BINARY_MIR_SYMBOL_CREATE_STRING) !=
          TURBO_RUNTIME_BINARY_MIR_OK ||
      turbo_runtime_binary_mir_plan_add_if_needed(
          plan, requirements.create_bytes,
          TURBO_RUNTIME_BINARY_MIR_SYMBOL_CREATE_BYTES) !=
          TURBO_RUNTIME_BINARY_MIR_OK ||
      turbo_runtime_binary_mir_plan_add_if_needed(
          plan, requirements.create_object,
          TURBO_RUNTIME_BINARY_MIR_SYMBOL_CREATE_OBJECT) !=
          TURBO_RUNTIME_BINARY_MIR_OK ||
      turbo_runtime_binary_mir_plan_add_if_needed(
          plan, requirements.create_array,
          TURBO_RUNTIME_BINARY_MIR_SYMBOL_CREATE_ARRAY) !=
          TURBO_RUNTIME_BINARY_MIR_OK ||
      turbo_runtime_binary_mir_plan_add_if_needed(
          plan, requirements.object_set,
          TURBO_RUNTIME_BINARY_MIR_SYMBOL_OBJECT_SET) !=
          TURBO_RUNTIME_BINARY_MIR_OK ||
      turbo_runtime_binary_mir_plan_add_if_needed(
          plan, requirements.array_append,
          TURBO_RUNTIME_BINARY_MIR_SYMBOL_ARRAY_APPEND) !=
          TURBO_RUNTIME_BINARY_MIR_OK ||
      turbo_runtime_binary_mir_plan_add_if_needed(
          plan, requirements.destroy_value,
          TURBO_RUNTIME_BINARY_MIR_SYMBOL_DESTROY_VALUE) !=
          TURBO_RUNTIME_BINARY_MIR_OK) {
    turbo_runtime_binary_mir_plan_destroy(plan);
    return NULL;
  }

  return plan;
}

void turbo_runtime_binary_mir_plan_destroy(turbo_runtime_binary_mir_plan_t *plan) {
  if (!plan) {
    return;
  }
  free(plan->symbols);
  free(plan);
}

size_t turbo_runtime_binary_mir_plan_symbol_count(
    const turbo_runtime_binary_mir_plan_t *plan) {
  return plan ? plan->symbol_count : 0;
}

turbo_runtime_binary_mir_status_t
turbo_runtime_binary_mir_plan_get_symbol(
    const turbo_runtime_binary_mir_plan_t *plan, size_t index,
    turbo_runtime_binary_mir_symbol_descriptor_t *out_symbol) {
  if (!plan || !out_symbol || index >= plan->symbol_count) {
    return TURBO_RUNTIME_BINARY_MIR_INVALID_ARGUMENT;
  }

  *out_symbol = plan->symbols[index];
  return TURBO_RUNTIME_BINARY_MIR_OK;
}

turbo_runtime_binary_mir_compiler_t *
turbo_runtime_binary_mir_compiler_create(
    const turbo_runtime_binary_schema_t *schema) {
  turbo_runtime_binary_mir_compiler_t *compiler;
  const char *schema_name;

  if (!schema) {
    return NULL;
  }

  compiler = (turbo_runtime_binary_mir_compiler_t *)calloc(1, sizeof(*compiler));
  if (!compiler) {
    return NULL;
  }

  compiler->schema = turbo_runtime_binary_schema_clone(schema);
  if (!compiler->schema) {
    turbo_runtime_binary_mir_compiler_destroy(compiler);
    return NULL;
  }

  compiler->plan = turbo_runtime_binary_mir_plan_create(schema);
  if (!compiler->plan) {
    turbo_runtime_binary_mir_compiler_destroy(compiler);
    return NULL;
  }

  schema_name = turbo_runtime_binary_schema_name(schema);
  compiler->entry_name = (char *)tstr_dup("parse_");
  if (!compiler->entry_name) {
    turbo_runtime_binary_mir_compiler_destroy(compiler);
    return NULL;
  }
  compiler->entry_name = (char *)tstr_cat(compiler->entry_name, schema_name);
  if (!compiler->entry_name) {
    turbo_runtime_binary_mir_compiler_destroy(compiler);
    return NULL;
  }
  return compiler;
}

void turbo_runtime_binary_mir_compiler_destroy(
    turbo_runtime_binary_mir_compiler_t *compiler) {
  if (!compiler) {
    return;
  }

  if (compiler->ctx) {
    if (compiler->gen_initialized) {
      MIR_gen_finish(compiler->ctx);
    }
    MIR_finish(compiler->ctx);
  }
  turbo_runtime_binary_schema_destroy(compiler->schema);
  turbo_runtime_binary_mir_plan_destroy(compiler->plan);
  tstr_free(compiler->entry_name);
  free(compiler);
}

const char *turbo_runtime_binary_mir_compiler_entry_name(
    const turbo_runtime_binary_mir_compiler_t *compiler) {
  return compiler ? compiler->entry_name : NULL;
}

const turbo_runtime_binary_mir_plan_t *
turbo_runtime_binary_mir_compiler_plan(
    const turbo_runtime_binary_mir_compiler_t *compiler) {
  return compiler ? compiler->plan : NULL;
}

turbo_runtime_binary_mir_status_t
turbo_runtime_binary_mir_compiler_build(
    turbo_runtime_binary_mir_compiler_t *compiler,
    const turbo_runtime_data_bind_value_api_t *api, char *error_buffer,
    size_t error_buffer_size) {
  MIR_type_t result_type;
  MIR_var_t arguments[3];
  int scalar_only;
  int repeated_scalar_only;
  int map_scalar_only;
  turbo_runtime_binary_mir_fast_path_kind_t fast_path_kind;
  MIR_item_t bridge_import_item = NULL;
  MIR_item_t bridge_proto_item = NULL;
  turbo_runtime_binary_mir_fast_path_items_t items = {0};
  MIR_label_t fail_label;
  MIR_label_t fail_return_label;
  MIR_reg_t result_reg;
  MIR_reg_t reader_reg;
  MIR_reg_t object_reg;
  MIR_reg_t status_reg;
  MIR_reg_t bool_reg;
  MIR_reg_t int64_reg;
  MIR_reg_t double_reg;
  MIR_reg_t field_value_reg;
  MIR_reg_t data_reg;
  MIR_reg_t size_reg;
  MIR_reg_t builder_user_data_reg;
  turbo_runtime_binary_mir_field_codegen_ctx_t field_codegen;
  size_t i;

  if (error_buffer && error_buffer_size != 0) {
    error_buffer[0] = '\0';
  }

  if (!compiler || !api) {
    turbo_runtime_binary_mir_write_error(error_buffer, error_buffer_size,
                                         "compiler and api are required");
    return TURBO_RUNTIME_BINARY_MIR_INVALID_ARGUMENT;
  }

  if (compiler->parser) {
    turbo_runtime_binary_mir_write_error(error_buffer, error_buffer_size,
                                         "compiler already built");
    return TURBO_RUNTIME_BINARY_MIR_ERROR;
  }
  if (compiler->ctx) {
    turbo_runtime_binary_mir_write_error(error_buffer, error_buffer_size,
                                         "compiler already initialized");
    return TURBO_RUNTIME_BINARY_MIR_ERROR;
  }

  if (turbo_runtime_binary_schema_validate_value_api(
          compiler->schema, api, error_buffer, error_buffer_size) !=
      TURBO_RUNTIME_BINARY_SCHEMA_OK) {
    return TURBO_RUNTIME_BINARY_MIR_UNSUPPORTED;
  }
  scalar_only = turbo_runtime_binary_mir_schema_is_all_scalar(compiler->schema);
  repeated_scalar_only =
      turbo_runtime_binary_mir_schema_is_all_repeated_scalar(compiler->schema);
  map_scalar_only =
      turbo_runtime_binary_mir_schema_is_all_string_key_map_scalar(compiler->schema);
  fast_path_kind = scalar_only ? TURBO_RUNTIME_BINARY_MIR_FAST_PATH_SCALAR
                               : (repeated_scalar_only
                                      ? TURBO_RUNTIME_BINARY_MIR_FAST_PATH_REPEATED
                                      : TURBO_RUNTIME_BINARY_MIR_FAST_PATH_MAP);

  compiler->ctx = MIR_init();
  if (!compiler->ctx) {
    return TURBO_RUNTIME_BINARY_MIR_OUT_OF_MEMORY;
  }

  compiler->module = MIR_new_module(compiler->ctx, "turbo_runtime_binary");
  if (!compiler->module) {
    turbo_runtime_binary_mir_write_error(error_buffer, error_buffer_size,
                                         "failed to create mir module");
    return TURBO_RUNTIME_BINARY_MIR_ERROR;
  }

  if (!scalar_only && !repeated_scalar_only && !map_scalar_only) {
    if (turbo_runtime_binary_mir_declare_externals(compiler->ctx, compiler->plan) !=
        TURBO_RUNTIME_BINARY_MIR_OK) {
      turbo_runtime_binary_mir_write_error(error_buffer, error_buffer_size,
                                           "failed to declare mir externals");
      return TURBO_RUNTIME_BINARY_MIR_ERROR;
    }
    if (turbo_runtime_binary_mir_declare_parse_bridge(
            compiler->ctx, &bridge_import_item, &bridge_proto_item) !=
        TURBO_RUNTIME_BINARY_MIR_OK) {
      turbo_runtime_binary_mir_write_error(error_buffer, error_buffer_size,
                                           "failed to declare parse bridge");
      return TURBO_RUNTIME_BINARY_MIR_ERROR;
    }
  } else {
    if (turbo_runtime_binary_mir_declare_fast_path_items(compiler->ctx, &items) !=
        TURBO_RUNTIME_BINARY_MIR_OK) {
      turbo_runtime_binary_mir_write_error(error_buffer, error_buffer_size,
                                           "failed to declare scalar mir helpers");
      return TURBO_RUNTIME_BINARY_MIR_ERROR;
    }
  }

  result_type = MIR_T_P;
  arguments[0].type = MIR_T_P;
  arguments[0].name = "data";
  arguments[0].size = 0;
  arguments[1].type = turbo_runtime_binary_mir_size_type();
  arguments[1].name = "size";
  arguments[1].size = 0;
  arguments[2].type = MIR_T_P;
  arguments[2].name = "builder_user_data";
  arguments[2].size = 0;
  compiler->func_item = MIR_new_func_arr(compiler->ctx, compiler->entry_name, 1,
                                         &result_type, 3, arguments);
  if (!compiler->func_item) {
    turbo_runtime_binary_mir_write_error(error_buffer, error_buffer_size,
                                         "failed to create mir function");
    return TURBO_RUNTIME_BINARY_MIR_ERROR;
  }

  result_reg =
      MIR_new_func_reg(compiler->ctx, compiler->func_item->u.func, MIR_T_I64, "$ret");
  reader_reg =
      MIR_new_func_reg(compiler->ctx, compiler->func_item->u.func, MIR_T_I64, "$reader");
  object_reg =
      MIR_new_func_reg(compiler->ctx, compiler->func_item->u.func, MIR_T_I64, "$object");
  status_reg =
      MIR_new_func_reg(compiler->ctx, compiler->func_item->u.func, MIR_T_I64, "$status");
  bool_reg =
      MIR_new_func_reg(compiler->ctx, compiler->func_item->u.func, MIR_T_I64, "$bool");
  int64_reg =
      MIR_new_func_reg(compiler->ctx, compiler->func_item->u.func, MIR_T_I64, "$i64");
  double_reg =
      MIR_new_func_reg(compiler->ctx, compiler->func_item->u.func, MIR_T_D, "$double");
  field_value_reg =
      MIR_new_func_reg(compiler->ctx, compiler->func_item->u.func, MIR_T_I64, "$field");
  data_reg = MIR_reg(compiler->ctx, "data", compiler->func_item->u.func);
  size_reg = MIR_reg(compiler->ctx, "size", compiler->func_item->u.func);
  builder_user_data_reg =
      MIR_reg(compiler->ctx, "builder_user_data", compiler->func_item->u.func);
  fail_label = MIR_new_label(compiler->ctx);
  fail_return_label = MIR_new_label(compiler->ctx);
  memset(&field_codegen, 0, sizeof(field_codegen));
  field_codegen.ctx = compiler->ctx;
  field_codegen.func_item = compiler->func_item;
  field_codegen.reader_remaining_proto_item = items.reader_remaining.proto_item;
  field_codegen.reader_remaining_import_item = items.reader_remaining.import_item;
  field_codegen.read_bool_proto_item = items.read_bool.proto_item;
  field_codegen.read_bool_import_item = items.read_bool.import_item;
  field_codegen.read_i32_proto_item = items.read_i32.proto_item;
  field_codegen.read_i32_import_item = items.read_i32.import_item;
  field_codegen.read_i64_proto_item = items.read_i64.proto_item;
  field_codegen.read_i64_import_item = items.read_i64.import_item;
  field_codegen.read_f32_proto_item = items.read_f32.proto_item;
  field_codegen.read_f32_import_item = items.read_f32.import_item;
  field_codegen.read_f64_proto_item = items.read_f64.proto_item;
  field_codegen.read_f64_import_item = items.read_f64.import_item;
  field_codegen.create_bool_proto_item = items.create_bool.proto_item;
  field_codegen.create_bool_import_item = items.create_bool.import_item;
  field_codegen.create_int64_proto_item = items.create_int64.proto_item;
  field_codegen.create_int64_import_item = items.create_int64.import_item;
  field_codegen.create_double_proto_item = items.create_double.proto_item;
  field_codegen.create_double_import_item = items.create_double.import_item;
  field_codegen.string_value_proto_item = items.string_value.proto_item;
  field_codegen.string_value_import_item = items.string_value.import_item;
  field_codegen.bytes_value_proto_item = items.bytes_value.proto_item;
  field_codegen.bytes_value_import_item = items.bytes_value.import_item;
  field_codegen.repeated_value_proto_item = items.repeated_value.proto_item;
  field_codegen.repeated_value_import_item = items.repeated_value.import_item;
  field_codegen.map_value_proto_item = items.map_value.proto_item;
  field_codegen.map_value_import_item = items.map_value.import_item;
  field_codegen.status_reg = status_reg;
  field_codegen.reader_reg = reader_reg;
  field_codegen.bool_reg = bool_reg;
  field_codegen.int64_reg = int64_reg;
  field_codegen.double_reg = double_reg;
  field_codegen.field_value_reg = field_value_reg;
  field_codegen.builder_user_data_reg = builder_user_data_reg;
  field_codegen.fail_label = fail_label;
  field_codegen.api = api;
  if (!scalar_only && !repeated_scalar_only && !map_scalar_only) {
    MIR_append_insn(compiler->ctx, compiler->func_item,
                    MIR_new_call_insn(
                        compiler->ctx, 8, MIR_new_ref_op(compiler->ctx, bridge_proto_item),
                        MIR_new_ref_op(compiler->ctx, bridge_import_item),
                        MIR_new_reg_op(compiler->ctx, result_reg),
                        MIR_new_uint_op(compiler->ctx,
                                        (uint64_t)(uintptr_t)compiler->schema),
                        MIR_new_uint_op(compiler->ctx, (uint64_t)(uintptr_t)api),
                        MIR_new_reg_op(compiler->ctx, data_reg),
                        MIR_new_reg_op(compiler->ctx, size_reg),
                        MIR_new_reg_op(compiler->ctx, builder_user_data_reg)));
    MIR_append_insn(compiler->ctx, compiler->func_item,
                    MIR_new_ret_insn(compiler->ctx, 1,
                                     MIR_new_reg_op(compiler->ctx, result_reg)));
  } else {
    turbo_runtime_binary_mir_append_fast_path_prologue(
        compiler->ctx, compiler->func_item, items.reader_create.proto_item,
        items.reader_create.import_item, items.create_object.proto_item,
        items.create_object.import_item, reader_reg, data_reg, size_reg, object_reg,
        builder_user_data_reg, fail_return_label, fail_label);

    for (i = 0; i < turbo_runtime_binary_schema_field_count(compiler->schema); ++i) {
      turbo_runtime_binary_field_descriptor_t field = {0};
      MIR_label_t next_label = MIR_new_label(compiler->ctx);

      if (turbo_runtime_binary_schema_get_field(compiler->schema, i, &field) !=
          TURBO_RUNTIME_BINARY_SCHEMA_OK) {
        turbo_runtime_binary_mir_write_error(error_buffer, error_buffer_size,
                                             "failed to inspect scalar field");
        return TURBO_RUNTIME_BINARY_MIR_ERROR;
      }

      turbo_runtime_binary_mir_append_field_value(
          &field_codegen, fast_path_kind, field.value_type);
      MIR_append_insn(
          compiler->ctx, compiler->func_item,
          MIR_new_insn(compiler->ctx, MIR_BF,
                       MIR_new_label_op(compiler->ctx, fail_label),
                       MIR_new_reg_op(compiler->ctx, field_value_reg)));
      turbo_runtime_binary_mir_append_object_set_or_cleanup(
          compiler->ctx, compiler->func_item, items.object_set.proto_item,
          items.object_set.import_item, items.destroy_value.proto_item,
          items.destroy_value.import_item, status_reg, builder_user_data_reg, object_reg,
          field.name, field_value_reg, next_label, fail_label);
      MIR_append_insn(compiler->ctx, compiler->func_item, next_label);
    }

    turbo_runtime_binary_mir_append_fast_path_epilogue(
        compiler->ctx, compiler->func_item, items.reader_remaining.proto_item,
        items.reader_remaining.import_item, items.reader_destroy.proto_item,
        items.reader_destroy.import_item, items.destroy_value.proto_item,
        items.destroy_value.import_item, status_reg, reader_reg, builder_user_data_reg,
        object_reg, fail_label, fail_return_label);
  }
  MIR_finish_func(compiler->ctx);
  MIR_finish_module(compiler->ctx);
  MIR_load_module(compiler->ctx, compiler->module);

  if (!scalar_only && !repeated_scalar_only && !map_scalar_only) {
    for (i = 0; i < compiler->plan->symbol_count; ++i) {
      void *address = turbo_runtime_binary_mir_api_symbol_address(
          api, compiler->plan->symbols[i].id);
      if (!address) {
        turbo_runtime_binary_mir_write_error(error_buffer, error_buffer_size,
                                             "missing api callback for mir symbol");
        return TURBO_RUNTIME_BINARY_MIR_UNSUPPORTED;
      }
      MIR_load_external(compiler->ctx, compiler->plan->symbols[i].name, address);
    }
    MIR_load_external(compiler->ctx, "turbo_runtime_binary_mir_parse_bridge",
                      (void *)turbo_runtime_binary_mir_parse_bridge);
  } else {
    if (turbo_runtime_binary_mir_load_api_symbol(
            compiler->ctx, api, TURBO_RUNTIME_BINARY_MIR_SYMBOL_CREATE_BOOL, 0,
            error_buffer, error_buffer_size) != TURBO_RUNTIME_BINARY_MIR_OK ||
        turbo_runtime_binary_mir_load_api_symbol(
            compiler->ctx, api, TURBO_RUNTIME_BINARY_MIR_SYMBOL_CREATE_INT64, 0,
            error_buffer, error_buffer_size) != TURBO_RUNTIME_BINARY_MIR_OK ||
        turbo_runtime_binary_mir_load_api_symbol(
            compiler->ctx, api, TURBO_RUNTIME_BINARY_MIR_SYMBOL_CREATE_DOUBLE, 0,
            error_buffer, error_buffer_size) != TURBO_RUNTIME_BINARY_MIR_OK ||
        turbo_runtime_binary_mir_load_api_symbol(
            compiler->ctx, api, TURBO_RUNTIME_BINARY_MIR_SYMBOL_CREATE_OBJECT, 1,
            error_buffer, error_buffer_size) != TURBO_RUNTIME_BINARY_MIR_OK ||
        turbo_runtime_binary_mir_load_api_symbol(
            compiler->ctx, api, TURBO_RUNTIME_BINARY_MIR_SYMBOL_OBJECT_SET, 1,
            error_buffer, error_buffer_size) != TURBO_RUNTIME_BINARY_MIR_OK ||
        turbo_runtime_binary_mir_load_api_symbol(
            compiler->ctx, api, TURBO_RUNTIME_BINARY_MIR_SYMBOL_DESTROY_VALUE, 1,
            error_buffer, error_buffer_size) != TURBO_RUNTIME_BINARY_MIR_OK) {
      return TURBO_RUNTIME_BINARY_MIR_UNSUPPORTED;
    }
    turbo_runtime_binary_mir_load_fast_path_helpers(compiler->ctx);
  }

  MIR_gen_init(compiler->ctx);
  compiler->gen_initialized = 1;
  MIR_link(compiler->ctx, MIR_set_gen_interface, NULL);
  compiler->parser = (turbo_runtime_binary_mir_parser_fn)MIR_gen(
      compiler->ctx, compiler->func_item);
  if (!compiler->parser) {
    turbo_runtime_binary_mir_write_error(error_buffer, error_buffer_size,
                                         "failed to jit mir parser");
    return TURBO_RUNTIME_BINARY_MIR_ERROR;
  }

  return TURBO_RUNTIME_BINARY_MIR_OK;
}

turbo_runtime_binary_mir_status_t
turbo_runtime_binary_mir_compiler_build_stub(
    turbo_runtime_binary_mir_compiler_t *compiler,
    const turbo_runtime_data_bind_value_api_t *api, char *error_buffer,
    size_t error_buffer_size) {
  return turbo_runtime_binary_mir_compiler_build(
      compiler, api, error_buffer, error_buffer_size);
}

turbo_runtime_binary_mir_parser_fn
turbo_runtime_binary_mir_compiler_parser(
    const turbo_runtime_binary_mir_compiler_t *compiler) {
  return compiler ? compiler->parser : NULL;
}
