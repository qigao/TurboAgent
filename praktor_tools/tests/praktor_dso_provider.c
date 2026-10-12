#include "praktor_dso_fixture.h"
#include <salts/plugin_decl.h>

#ifndef PRAKTOR_DSO_EPOCH
#error "PRAKTOR_DSO_EPOCH must name the independently built Provider DSO"
#endif
#ifndef PRAKTOR_DSO_PLUGIN_ID
#error "PRAKTOR_DSO_PLUGIN_ID must be a unique Plugin identity"
#endif

cmeta_component(PraktorDsoFixture,
    cmeta_provides(praktor_dso_gate));

static int fixture_object = PRAKTOR_DSO_EPOCH;

/* This function and the vtable that points to it physically live in the
 * dynamically loaded Provider. An in-flight HostTool must pin this image. */
static int fixture_run(void *self, praktor_dso_control *control) {
  const uint64_t deadline =
      control && control->now_ms
        ? control->now_ms(control->host) + UINT64_C(10000) : 0;
  (void)self;
  if (!control) return -1000;
  atomic_store_explicit(&control->entered, 1, memory_order_release);
  if (control->block_until_cancel) {
    if (!control->cancelled || !control->now_ms || !control->yield)
      return -1001;
    while (control->now_ms(control->host) < deadline) {
      if (control->cancelled(control->host)) {
        atomic_store_explicit(&control->exited, 1, memory_order_release);
        return -PRAKTOR_DSO_EPOCH;
      }
      control->yield(control->host);
    }
    atomic_store_explicit(&control->exited, 1, memory_order_release);
    return -1002;
  }
  atomic_store_explicit(&control->exited, 1, memory_order_release);
  return PRAKTOR_DSO_EPOCH;
}

CMETA_IMPLEMENTS(
    praktor_dso_gate, praktor_dso_gate_impl, 0u,
    .run = fixture_run);

static cmeta_status fixture_project(
    void *context, const cmeta_object_ref *object,
    const cmeta_interface_desc *expected,
    cmeta_interface_projection *out) {
  (void)context;
  if (!object || !out) return CMETA_INVALID_ARGUMENT;
  if (!cmeta_interface_desc_equal(expected, praktor_dso_gate_interface()))
    return CMETA_TRAIT_MISSING;
  out->size = sizeof(*out);
  out->self = object->object;
  out->interface = praktor_dso_gate_interface();
  out->dispatch = &praktor_dso_gate_impl_vtable;
  return CMETA_OK;
}

static const cmeta_object_interface_provider fixture_interfaces = {
  sizeof(cmeta_object_interface_provider),
  NULL,
  fixture_project
};

static cmeta_status SALTS_COMPONENT_CALL fixture_create(
    void *provider_context,
    const cmeta_data_desc *config_data, const void *config_value,
    const salts_component_dependency *dependencies, size_t dependency_count,
    cmeta_object_ref *out_instance) {
  (void)dependencies;
  if (!provider_context || !out_instance || config_data || config_value ||
      dependency_count != 0u)
    return CMETA_INVALID_ARGUMENT;
  return cmeta_object_borrow(
      out_instance, provider_context, &cmeta_data_int, NULL);
}

static const salts_component_provider_binding fixture_binding = {
  sizeof(salts_component_provider_binding),
  SALTS_COMPONENT_PROVIDER_BINDING_ABI_VERSION,
  cmeta_component_meta(PraktorDsoFixture),
  &fixture_object,
  &fixture_interfaces,
  fixture_create,
  NULL,
  NULL
};

static const salts_component_provider_binding *fixture_get_binding(void *self) {
  return (const salts_component_provider_binding *)self;
}

CMETA_IMPLEMENTS(
    salts_component_provider, fixture_component_provider_impl, 0u,
    .get_binding = fixture_get_binding);

static salts_component_provider fixture_provider = {
  (void *)&fixture_binding,
  &fixture_component_provider_impl_vtable
};

#define PRAKTOR_DSO_EXPORTS(X) \
  X(interface, (salts_component_provider, &fixture_provider), \
    PRAKTOR_DSO_PROVIDER_EXPORT_ID, SALTS_COMPONENT_PROVIDER_CONTRACT_ID, \
    SALTS_COMPONENT_PROVIDER_CONTRACT_VERSION, 0)

CMETA_PLUGIN_DECLARE(
    praktor_dso_provider_fixture,
    PRAKTOR_DSO_PLUGIN_ID,
    (1,0,0),
    PRAKTOR_DSO_EXPORTS,
    CMETA_PLUGIN_PASSIVE());
