#include <turbo_tool_registry.h>
#include <turbo_tool_runtime.h>

#include <type_traits>

static_assert(
    std::is_standard_layout<turbo_tool_native_projection_t>::value,
    "native projection must remain C-compatible");
static_assert(
    TURBO_TOOL_NATIVE_PROJECTION_ABI_VERSION == 1u,
    "native projection ABI version changed unexpectedly");

int main() {
  turbo_tool_native_projection_t projection{};
  projection.struct_size = sizeof(projection);
  projection.abi_version = TURBO_TOOL_NATIVE_PROJECTION_ABI_VERSION;

  return turbo_tool_registry_get_native_projection(
             nullptr, "missing", &projection) ==
         TURBO_TOOL_INVALID_ARGUMENT
             ? 0
             : 1;
}
