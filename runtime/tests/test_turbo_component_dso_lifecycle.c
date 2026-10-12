/* Native Provider DSO lifecycle test. This target deliberately does not
 * depend on Praktor: Windows and macOS can qualify OS module unload while the
 * optional PraktorTools module is absent or untested on those platforms.
 * Salts Plugin Registry remains the sole DSO loader and ref-count authority.
 */
#include "tinytest.h"
#include "praktor_dso_fixture.h"

#include <salts/component_plugin.h>
#include <salts/clock.h>
#include <salts/thread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef PRAKTOR_DSO_N_PATH
#error "Provider N shared module must be provided by CMake"
#endif
#ifndef PRAKTOR_DSO_N1_PATH
#error "Provider N+1 shared module must be provided by CMake"
#endif

typedef struct provider_generation_fixture {
  salts_component_plugin_generation generation;
  salts_component_deployment deployments[1];
  salts_component_instance instances[1];
  salts_component_dependency dependencies[1];
  size_t order[1];
  salts_component_plugin_module modules[1];
} provider_generation_fixture;

typedef struct provider_dso_host {
  atomic_int cancel;
} provider_dso_host;

typedef struct provider_dso_call {
  praktor_dso_gate gate; /* borrowed; owning Scope outlives this thread */
  praktor_dso_control control;
  int result;
} provider_dso_call;

typedef struct provider_dso_racer {
  salts_component_plugin_runtime *runtime;
  salts_component_plugin_generation *old_generation;
  cmeta_plugin_registry *registry;
  cmeta_plugin_ref old_ref;
  atomic_int ready;
  atomic_int go;
  salts_component_plugin_status drain_result;
  cmeta_plugin_status unload_result;
} provider_dso_racer;

static salts_component_plugin_status provider_dso_build(
    provider_generation_fixture *fixture, uint64_t id,
    cmeta_plugin_registry *registry, cmeta_plugin_ref ref) {
  const salts_component_plugin_generation_storage storage = {
    fixture->deployments, 1u, fixture->instances, 1u,
    fixture->dependencies, 1u, fixture->order, 1u,
    fixture->modules, 1u
  };
  const salts_component_plugin_source source = {
    ref, PRAKTOR_DSO_PROVIDER_EXPORT_ID, NULL, NULL
  };
  return salts_component_plugin_generation_build(
      &fixture->generation, id, registry, &storage,
      NULL, 0u, &source, 1u, NULL, 0u);
}

static int provider_dso_bind(
    salts_component_plugin_scope *scope, praktor_dso_gate *out) {
  salts_component_service service = {0};
  return salts_component_plugin_scope_find_service(
             scope, praktor_dso_gate_interface(), &service) ==
             SALTS_COMPONENT_PLUGIN_OK &&
         praktor_dso_gate_borrow_from_object(
             service.object, service.interfaces, out) == CMETA_OK;
}

static int provider_dso_cancelled(void *host) {
  provider_dso_host *owner = (provider_dso_host *)host;
  return atomic_load_explicit(&owner->cancel, memory_order_acquire);
}

static uint64_t provider_dso_now_ms(void *host) {
  (void)host;
  return cmeta_monotonic_ms();
}

static void provider_dso_yield(void *host) {
  (void)host;
  cmeta_thread_yield();
}

static void provider_dso_invoke(void *arg) {
  provider_dso_call *call = (provider_dso_call *)arg;
  call->result = praktor_dso_gate_run(&call->gate, &call->control);
}

static void provider_dso_concurrent_unload(void *arg) {
  provider_dso_racer *race = (provider_dso_racer *)arg;
  atomic_store_explicit(&race->ready, 1, memory_order_release);
  while (!atomic_load_explicit(&race->go, memory_order_acquire))
    cmeta_thread_yield();
  /* Independently run against the real registry while the main host
   * simultaneously closes N+1. N must still have an in-flight callback
   * and a live Salts scope. These calls cannot unload N. */
  race->drain_result = salts_component_plugin_generation_drain(
      race->runtime, race->old_generation);
  race->unload_result = cmeta_plugin_registry_unload(
      race->registry, race->old_ref);
}


/* Race only the public, synchronized ComponentPlugin APIs. The scope is
 * released after the native callback thread joins: unloading code that is
 * still on a stack is never an admissible outcome. */
typedef struct provider_dso_terminal_race {
  salts_component_plugin_runtime *runtime;
  salts_component_plugin_generation *generation;
  salts_component_plugin_scope *scope;
  cmeta_plugin_registry *registry;
  cmeta_plugin_ref ref;
  atomic_int initial_busy_ready;
  atomic_int release_gate;
  atomic_int drain_first_observed;
  atomic_int release_completed;
  /* 0 = competing, 1 = forced drain-first, 2 = forced release-first. */
  int schedule;
  salts_component_plugin_status initial_drain;
  cmeta_plugin_status initial_unload;
  salts_component_plugin_status scope_release;
  salts_component_plugin_status final_drain;
  cmeta_plugin_status poll_result;
  cmeta_plugin_status final_unload;
  int busy_polls;
  atomic_int timed_out;
  bool quiescent;
} provider_dso_terminal_race;

static void provider_dso_terminal_drainer(void *arg) {
  provider_dso_terminal_race *race = (provider_dso_terminal_race *)arg;
  uint64_t stop;
  race->initial_drain = salts_component_plugin_generation_drain(
      race->runtime, race->generation);
  race->initial_unload = cmeta_plugin_registry_unload(
      race->registry, race->ref);
  atomic_store_explicit(&race->initial_busy_ready, 1, memory_order_release);
  stop = cmeta_monotonic_ms() + UINT64_C(10000);
  while (!atomic_load_explicit(&race->release_gate, memory_order_acquire) &&
         cmeta_monotonic_ms() < stop)
    cmeta_thread_yield();
  if (!atomic_load_explicit(&race->release_gate, memory_order_acquire)) {
    atomic_store_explicit(&race->timed_out, 1, memory_order_release);
    return;
  }
  /* The release-first schedule waits for a completed final release before
   * the first drain attempt. The drain-first schedule forces one BUSY
   * observation before allowing release; otherwise the calls truly race. */
  if (race->schedule == 2) {
    while (!atomic_load_explicit(&race->release_completed,
                                 memory_order_acquire) &&
           cmeta_monotonic_ms() < stop)
      cmeta_thread_yield();
    if (!atomic_load_explicit(&race->release_completed,
                              memory_order_acquire)) {
      atomic_store_explicit(&race->timed_out, 1, memory_order_release);
      return;
    }
  }
  race->final_drain = SALTS_COMPONENT_PLUGIN_BUSY;
  if (race->schedule == 1) {
    race->final_drain = salts_component_plugin_generation_drain(
        race->runtime, race->generation);
    if (race->final_drain == SALTS_COMPONENT_PLUGIN_BUSY)
      ++race->busy_polls;
    atomic_store_explicit(&race->drain_first_observed, 1,
                          memory_order_release);
    if (race->final_drain != SALTS_COMPONENT_PLUGIN_BUSY) return;
  }
  while (cmeta_monotonic_ms() < stop) {
    race->final_drain = salts_component_plugin_generation_drain(
        race->runtime, race->generation);
    if (race->final_drain != SALTS_COMPONENT_PLUGIN_BUSY) break;
    ++race->busy_polls;
    cmeta_thread_yield();
  }
  if (race->final_drain == SALTS_COMPONENT_PLUGIN_BUSY) {
    atomic_store_explicit(&race->timed_out, 1, memory_order_release);
    return;
  }
  if (race->final_drain != SALTS_COMPONENT_PLUGIN_OK) return;
  race->poll_result = cmeta_plugin_registry_poll_quiescent(
      race->registry, race->ref, &race->quiescent);
  if (race->poll_result == CMETA_PLUGIN_OK && race->quiescent)
    race->final_unload = cmeta_plugin_registry_unload(
        race->registry, race->ref);
}

static void provider_dso_terminal_releaser(void *arg) {
  provider_dso_terminal_race *race = (provider_dso_terminal_race *)arg;
  const uint64_t stop = cmeta_monotonic_ms() + UINT64_C(10000);
  while (!atomic_load_explicit(&race->release_gate, memory_order_acquire) &&
         cmeta_monotonic_ms() < stop)
    cmeta_thread_yield();
  if (!atomic_load_explicit(&race->release_gate, memory_order_acquire)) {
    atomic_store_explicit(&race->timed_out, 1, memory_order_release);
    return;
  }
  if (race->schedule == 1) {
    while (!atomic_load_explicit(&race->drain_first_observed,
                                 memory_order_acquire) &&
           cmeta_monotonic_ms() < stop)
      cmeta_thread_yield();
    if (!atomic_load_explicit(&race->drain_first_observed,
                              memory_order_acquire)) {
      atomic_store_explicit(&race->timed_out, 1, memory_order_release);
      return;
    }
  }
  race->scope_release = salts_component_plugin_scope_release(race->scope);
  atomic_store_explicit(&race->release_completed, 1, memory_order_release);
}


typedef struct provider_dso_load_gate {
  cmeta_plugin_registry *registry;
  atomic_int ready, go;
} provider_dso_load_gate;
typedef struct provider_dso_load_thread {
  provider_dso_load_gate *gate;
  cmeta_plugin_status result;
  cmeta_plugin_ref ref;
} provider_dso_load_thread;
static void provider_dso_parallel_load(void *arg) {
  provider_dso_load_thread *worker = (provider_dso_load_thread *)arg;
  worker->ref = (cmeta_plugin_ref){1u, 37u};
  atomic_fetch_add_explicit(&worker->gate->ready, 1, memory_order_acq_rel);
  while (!atomic_load_explicit(&worker->gate->go, memory_order_acquire))
    cmeta_thread_yield();
  worker->result = cmeta_plugin_registry_load(
      worker->gate->registry, PRAKTOR_DSO_N_PATH, &worker->ref);
}

static void provider_dso_control_init(
    praktor_dso_control *control, provider_dso_host *host,
    int block_until_cancel) {
  memset(control, 0, sizeof(*control));
  control->host = host;
  control->cancelled = provider_dso_cancelled;
  control->now_ms = provider_dso_now_ms;
  control->yield = provider_dso_yield;
  control->block_until_cancel = block_until_cancel;
  atomic_init(&control->entered, 0);
  atomic_init(&control->exited, 0);
}

spec("cross-platform real Provider DSO close, drain, unload") {

  it("admits exactly one of two simultaneous loads of the same real DSO") {
    const cmeta_plugin_registry_config config = {2};
    cmeta_plugin_registry registry = {0};
    provider_dso_load_gate gate = {0};
    provider_dso_load_thread workers[2] = {0};
    cmeta_thread_t threads[2] = {0};
    cmeta_plugin_ref winner = {0};
    provider_generation_fixture generation = {0};
    salts_component_plugin_runtime runtime = SALTS_COMPONENT_PLUGIN_RUNTIME_INIT;
    salts_component_plugin_scope scope = SALTS_COMPONENT_PLUGIN_SCOPE_INIT;
    salts_component_plugin_generation *retired = NULL;
    provider_dso_host host = {0};
    provider_dso_call call = {0};
    int ready, success = 0, duplicate = 0;
    bool quiet = false;

    atomic_init(&host.cancel, 0);
    provider_dso_control_init(&call.control, &host, 0);
    atomic_init(&gate.ready, 0);
    atomic_init(&gate.go, 0);
    check_equal(cmeta_plugin_registry_init(&registry, &config), CMETA_PLUGIN_OK);
    gate.registry = &registry;
    for (size_t i = 0; i < 2; ++i) {
      workers[i].gate = &gate;
      check_equal(cmeta_thread_create(
          &threads[i], provider_dso_parallel_load, &workers[i]), SALTS_OK);
    }
    {
      const uint64_t deadline = cmeta_monotonic_ms() + UINT64_C(10000);
      while (atomic_load_explicit(&gate.ready, memory_order_acquire) < 2 &&
             cmeta_monotonic_ms() < deadline)
        cmeta_thread_yield();
    }
    ready = atomic_load_explicit(&gate.ready, memory_order_acquire);
    atomic_store_explicit(&gate.go, 1, memory_order_release);
    for (size_t i = 0; i < 2; ++i) {
      check_equal(cmeta_thread_join(&threads[i]), SALTS_OK);
      if (workers[i].result == CMETA_PLUGIN_OK) {
        ++success;
        winner = workers[i].ref;
        check_true(cmeta_plugin_ref_valid(workers[i].ref));
      } else if (workers[i].result == CMETA_PLUGIN_DUPLICATE_PLUGIN_ID) {
        ++duplicate;
        check_false(cmeta_plugin_ref_valid(workers[i].ref));
      }
    }
    check_equal(ready, 2);
    check_equal(success, 1);
    check_equal(duplicate, 1);
    check_equal(cmeta_plugin_registry_count(&registry), (size_t)1u);

    /* Failed competing dlclose must not invalidate the published callback. */
    check_equal(cmeta_plugin_registry_start(&registry, winner), CMETA_PLUGIN_OK);
    check_equal(provider_dso_build(&generation, UINT64_C(41),
                                    &registry, winner), SALTS_COMPONENT_PLUGIN_OK);
    check_equal(salts_component_plugin_runtime_init(&runtime), SALTS_COMPONENT_PLUGIN_OK);
    check_equal(salts_component_plugin_runtime_publish(
        &runtime, &generation.generation, &retired), SALTS_COMPONENT_PLUGIN_OK);
    check_null(retired);
    check_equal(salts_component_plugin_scope_acquire(
        &runtime, &scope), SALTS_COMPONENT_PLUGIN_OK);
    check_true(provider_dso_bind(&scope, &call.gate));
    check_equal(praktor_dso_gate_run(&call.gate, &call.control), 41);
    check_equal(salts_component_plugin_runtime_close(
        &runtime, &retired), SALTS_COMPONENT_PLUGIN_OK);
    check_true(retired == &generation.generation);
    check_equal(salts_component_plugin_generation_drain(
        &runtime, retired), SALTS_COMPONENT_PLUGIN_BUSY);
    check_equal(salts_component_plugin_scope_release(
        &scope), SALTS_COMPONENT_PLUGIN_OK);
    check_equal(salts_component_plugin_generation_drain(
        &runtime, retired), SALTS_COMPONENT_PLUGIN_OK);
    check_equal(salts_component_plugin_runtime_destroy(&runtime),
                SALTS_COMPONENT_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_request_stop(
        &registry, winner), CMETA_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_poll_quiescent(
        &registry, winner, &quiet), CMETA_PLUGIN_OK);
    check_true(quiet);
    check_equal(cmeta_plugin_registry_unload(
        &registry, winner), CMETA_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_count(&registry), (size_t)0u);
    check_equal(cmeta_plugin_registry_destroy(&registry), CMETA_PLUGIN_OK);
  }


  it("rejects missing DSOs and duplicate identities without disturbing live N") {
    const cmeta_plugin_registry_config config = {2};
    cmeta_plugin_registry registry = {0};
    cmeta_plugin_ref old_ref = {0}, rejected_ref = {0};
    cmeta_plugin_lifecycle_info lifecycle;
    provider_generation_fixture generation = {0};
    salts_component_plugin_runtime runtime = SALTS_COMPONENT_PLUGIN_RUNTIME_INIT;
    salts_component_plugin_scope scope = SALTS_COMPONENT_PLUGIN_SCOPE_INIT;
    salts_component_plugin_generation *retired = NULL;
    provider_dso_host host = {0};
    provider_dso_call call = {0};
    bool quiet = false;

    atomic_init(&host.cancel, 0);
    provider_dso_control_init(&call.control, &host, 0);
    check_equal(cmeta_plugin_registry_init(&registry, &config), CMETA_PLUGIN_OK);

    /* UTF-8 syntax is valid, but the DSO does not exist. This enters the
     * platform loader and must restore reservation and output identity. */
    rejected_ref = (cmeta_plugin_ref){1u, 11u};
    check_equal(cmeta_plugin_registry_load(
        &registry, "__turboagent_missing_provider_fixture_98__", &rejected_ref),
        CMETA_PLUGIN_LOAD_FAILED);
    check_false(cmeta_plugin_ref_valid(rejected_ref));
    check_equal(cmeta_plugin_registry_count(&registry), (size_t)0u);

    check_equal(cmeta_plugin_registry_load(
        &registry, PRAKTOR_DSO_N_PATH, &old_ref), CMETA_PLUGIN_OK);
    check_true(cmeta_plugin_ref_valid(old_ref));
    check_equal(cmeta_plugin_registry_start(&registry, old_ref), CMETA_PLUGIN_OK);
    check_equal(provider_dso_build(&generation, UINT64_C(41),
                                    &registry, old_ref), SALTS_COMPONENT_PLUGIN_OK);
    check_equal(salts_component_plugin_runtime_init(&runtime),
                SALTS_COMPONENT_PLUGIN_OK);
    check_equal(salts_component_plugin_runtime_publish(
        &runtime, &generation.generation, &retired), SALTS_COMPONENT_PLUGIN_OK);
    check_null(retired);
    check_equal(salts_component_plugin_scope_acquire(
        &runtime, &scope), SALTS_COMPONENT_PLUGIN_OK);
    check_true(provider_dso_bind(&scope, &call.gate));
    check_equal(praktor_dso_gate_run(&call.gate, &call.control), 41);

    /* Registry capacity still permits a second load, but the existing
     * identity must win. dlclose of the rejected duplicate must not
     * invalidate an already loaded and scope-pinned Provider. */
    rejected_ref = (cmeta_plugin_ref){1u, 33u};
    check_equal(cmeta_plugin_registry_load(
        &registry, PRAKTOR_DSO_N_PATH, &rejected_ref),
        CMETA_PLUGIN_DUPLICATE_PLUGIN_ID);
    check_false(cmeta_plugin_ref_valid(rejected_ref));
    check_equal(cmeta_plugin_registry_count(&registry), (size_t)1u);
    check_equal(cmeta_plugin_registry_get_lifecycle(
        &registry, old_ref, &lifecycle), CMETA_PLUGIN_OK);
    check_true(lifecycle.active_leases > 0u);
    check_equal(praktor_dso_gate_run(&call.gate, &call.control), 41);

    check_equal(salts_component_plugin_runtime_close(
        &runtime, &retired), SALTS_COMPONENT_PLUGIN_OK);
    check_true(retired == &generation.generation);
    check_equal(salts_component_plugin_generation_drain(
        &runtime, &generation.generation), SALTS_COMPONENT_PLUGIN_BUSY);
    check_equal(salts_component_plugin_scope_release(&scope),
                SALTS_COMPONENT_PLUGIN_OK);
    check_equal(salts_component_plugin_generation_drain(
        &runtime, &generation.generation), SALTS_COMPONENT_PLUGIN_OK);
    check_equal(salts_component_plugin_runtime_destroy(&runtime),
                SALTS_COMPONENT_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_request_stop(
        &registry, old_ref), CMETA_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_poll_quiescent(
        &registry, old_ref, &quiet), CMETA_PLUGIN_OK);
    check_true(quiet);
    check_equal(cmeta_plugin_registry_unload(&registry, old_ref), CMETA_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_count(&registry), (size_t)0u);
    check_equal(cmeta_plugin_registry_destroy(&registry), CMETA_PLUGIN_OK);
  }

  it("releases failed candidate leases and permits explicit corrected generation build") {
    const cmeta_plugin_registry_config config = {1};
    cmeta_plugin_registry registry = {0};
    cmeta_plugin_ref ref = {0};
    cmeta_plugin_lifecycle_info lifecycle;
    provider_generation_fixture generation = {0};
    const salts_component_plugin_generation_storage storage = {
      generation.deployments, 1u, generation.instances, 1u,
      generation.dependencies, 1u, generation.order, 1u,
      generation.modules, 1u
    };
    salts_component_plugin_source wrong = {
      {0}, "missing-provider-export-98", NULL, NULL
    };
    const salts_component_plugin_failure *failure;
    salts_component_plugin_runtime runtime = SALTS_COMPONENT_PLUGIN_RUNTIME_INIT;
    salts_component_plugin_generation *retired = NULL;
    salts_component_plugin_scope scope = SALTS_COMPONENT_PLUGIN_SCOPE_INIT;
    provider_dso_call call = {0};
    provider_dso_host host = {0};
    bool quiet = false;

    atomic_init(&host.cancel, 0);
    provider_dso_control_init(&call.control, &host, 0);
    check_equal(cmeta_plugin_registry_init(&registry, &config), CMETA_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_load(
        &registry, PRAKTOR_DSO_N_PATH, &ref), CMETA_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_start(&registry, ref), CMETA_PLUGIN_OK);

    wrong.plugin = ref;
    /* Candidate acquires one real Plugin lease before discovering that the
     * selected provider export is absent. It must release that lease and
     * never publish or retain an abandoned generation. */
    check_equal(salts_component_plugin_generation_build(
        &generation.generation, UINT64_C(41), &registry, &storage,
        NULL, 0u, &wrong, 1u, NULL, 0u),
        SALTS_COMPONENT_PLUGIN_PLUGIN_ERROR);
    check_equal(generation.generation.state,
                SALTS_COMPONENT_PLUGIN_GENERATION_FAILED);
    failure = salts_component_plugin_generation_failure(&generation.generation);
    check_not_null(failure);
    check_equal(failure->source_index, (size_t)0u);
    check_equal(failure->plugin_status, CMETA_PLUGIN_UNKNOWN_EXPORT);
    check_equal(cmeta_plugin_registry_get_lifecycle(
        &registry, ref, &lifecycle), CMETA_PLUGIN_OK);
    check_equal(lifecycle.active_leases, (size_t)0u);
    check_equal(salts_component_plugin_runtime_init(&runtime),
                SALTS_COMPONENT_PLUGIN_OK);

    /* An explicit trusted host correction may rebuild the same unowned
     * candidate. There is no fallback, implicit retry or a second loader. */
    check_equal(provider_dso_build(
        &generation, UINT64_C(42), &registry, ref), SALTS_COMPONENT_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_get_lifecycle(
        &registry, ref, &lifecycle), CMETA_PLUGIN_OK);
    check_equal(lifecycle.active_leases, (size_t)1u);
    check_equal(salts_component_plugin_runtime_publish(
        &runtime, &generation.generation, &retired), SALTS_COMPONENT_PLUGIN_OK);
    check_null(retired);
    check_equal(salts_component_plugin_scope_acquire(
        &runtime, &scope), SALTS_COMPONENT_PLUGIN_OK);
    check_equal(salts_component_plugin_scope_generation_id(&scope),
                UINT64_C(42));
    check_true(provider_dso_bind(&scope, &call.gate));
    check_equal(praktor_dso_gate_run(&call.gate, &call.control), 41);
    check_equal(salts_component_plugin_runtime_close(
        &runtime, &retired), SALTS_COMPONENT_PLUGIN_OK);
    check_true(retired == &generation.generation);
    check_equal(salts_component_plugin_generation_drain(
        &runtime, &generation.generation), SALTS_COMPONENT_PLUGIN_BUSY);
    check_equal(salts_component_plugin_scope_release(&scope),
                SALTS_COMPONENT_PLUGIN_OK);
    check_equal(salts_component_plugin_generation_drain(
        &runtime, &generation.generation), SALTS_COMPONENT_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_get_lifecycle(
        &registry, ref, &lifecycle), CMETA_PLUGIN_OK);
    check_equal(lifecycle.active_leases, (size_t)0u);
    check_equal(salts_component_plugin_runtime_destroy(&runtime),
                SALTS_COMPONENT_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_request_stop(
        &registry, ref), CMETA_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_poll_quiescent(
        &registry, ref, &quiet), CMETA_PLUGIN_OK);
    check_true(quiet);
    check_equal(cmeta_plugin_registry_unload(&registry, ref), CMETA_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_destroy(&registry), CMETA_PLUGIN_OK);
  }

  it("rejects malformed UTF-8 module paths before native loader admission") {
    const cmeta_plugin_registry_config config = {2};
    cmeta_plugin_registry registry = {0};
    cmeta_plugin_ref ref = {0};
    cmeta_plugin_lifecycle_info info;
    bool quiescent = false;
    const char truncated_utf8[] = {(char)0xc3, '(', 0};
    const char overlong_utf8[] = {(char)0xc0, (char)0xaf, 0};
    const char lone_continuation[] = {(char)0x80, 0};
    const char *const rejected[] = {
      NULL, "", truncated_utf8, overlong_utf8, lone_continuation
    };

    check_equal(cmeta_plugin_registry_init(&registry, &config),
                CMETA_PLUGIN_OK);
    for (size_t i = 0; i < sizeof(rejected) / sizeof(rejected[0]); ++i) {
      /* Every rejected request resets an output ref and leaves capacity
       * available for a legitimate release-pinned Provider DSO. */
      ref = (cmeta_plugin_ref){1u, 17u};
      check_equal(cmeta_plugin_registry_load(
          &registry, rejected[i], &ref), CMETA_PLUGIN_INVALID_ARGUMENT);
      check_false(cmeta_plugin_ref_valid(ref));
      check_equal(cmeta_plugin_registry_count(&registry), (size_t)0u);
    }

    check_equal(cmeta_plugin_registry_load(
        &registry, PRAKTOR_DSO_N_PATH, &ref), CMETA_PLUGIN_OK);
    check_true(cmeta_plugin_ref_valid(ref));
    check_equal(cmeta_plugin_registry_count(&registry), (size_t)1u);
    check_equal(cmeta_plugin_registry_start(&registry, ref), CMETA_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_request_stop(
        &registry, ref), CMETA_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_poll_quiescent(
        &registry, ref, &quiescent), CMETA_PLUGIN_OK);
    check_true(quiescent);
    check_equal(cmeta_plugin_registry_get_lifecycle(
        &registry, ref, &info), CMETA_PLUGIN_OK);
    check_equal(info.active_leases, (size_t)0u);
    check_equal(cmeta_plugin_registry_unload(&registry, ref), CMETA_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_count(&registry), (size_t)0u);
    check_equal(cmeta_plugin_registry_destroy(&registry), CMETA_PLUGIN_OK);
  }

  it("pins callback inside N while N+1 executes and unload races with close") {
    const cmeta_plugin_registry_config plugin_config = {2};
    cmeta_plugin_registry registry = {0};
    cmeta_plugin_ref old_ref = {0}, new_ref = {0};
    cmeta_plugin_lifecycle_info info;
    provider_generation_fixture n = {0}, n1 = {0};
    salts_component_plugin_runtime runtime = SALTS_COMPONENT_PLUGIN_RUNTIME_INIT;
    salts_component_plugin_generation *retired = NULL;
    salts_component_plugin_scope old_scope = SALTS_COMPONENT_PLUGIN_SCOPE_INIT;
    salts_component_plugin_scope new_scope = SALTS_COMPONENT_PLUGIN_SCOPE_INIT;
    salts_component_plugin_scope stale_scope = SALTS_COMPONENT_PLUGIN_SCOPE_INIT;
    provider_dso_host host = {0};
    provider_dso_call old_call = {0}, new_call = {0};
    provider_dso_racer racer = {0};
    cmeta_thread_t callback_thread = {0}, race_thread = {0};
    bool quiet = false;
    int entered, ready;

    atomic_init(&host.cancel, 0);
    provider_dso_control_init(&old_call.control, &host, 1);
    provider_dso_control_init(&new_call.control, &host, 0);
    check_equal(cmeta_plugin_registry_init(&registry, &plugin_config),
                CMETA_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_load(
        &registry, PRAKTOR_DSO_N_PATH, &old_ref), CMETA_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_load(
        &registry, PRAKTOR_DSO_N1_PATH, &new_ref), CMETA_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_start(&registry, old_ref), CMETA_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_start(&registry, new_ref), CMETA_PLUGIN_OK);
    check_equal(provider_dso_build(&n, UINT64_C(41), &registry, old_ref),
                SALTS_COMPONENT_PLUGIN_OK);
    check_equal(provider_dso_build(&n1, UINT64_C(42), &registry, new_ref),
                SALTS_COMPONENT_PLUGIN_OK);
    check_equal(salts_component_plugin_runtime_init(&runtime),
                SALTS_COMPONENT_PLUGIN_OK);
    check_equal(salts_component_plugin_runtime_publish(
        &runtime, &n.generation, &retired), SALTS_COMPONENT_PLUGIN_OK);
    check_null(retired);

    check_equal(salts_component_plugin_scope_acquire(
        &runtime, &old_scope), SALTS_COMPONENT_PLUGIN_OK);
    check_equal(salts_component_plugin_scope_generation_id(&old_scope),
                UINT64_C(41));
    check_true(provider_dso_bind(&old_scope, &old_call.gate));
    check_equal(cmeta_thread_create(
        &callback_thread, provider_dso_invoke, &old_call), SALTS_OK);
    {
      const uint64_t stop = cmeta_monotonic_ms() + UINT64_C(10000);
      while (!atomic_load_explicit(&old_call.control.entered,
                                   memory_order_acquire) &&
             cmeta_monotonic_ms() < stop)
        cmeta_thread_yield();
    }
    entered = atomic_load_explicit(
        &old_call.control.entered, memory_order_acquire);
    check_true(entered);

    check_equal(salts_component_plugin_runtime_publish(
        &runtime, &n1.generation, &retired), SALTS_COMPONENT_PLUGIN_OK);
    check_true(retired == &n.generation);
    check_equal(salts_component_plugin_generation_drain(
        &runtime, &n.generation), SALTS_COMPONENT_PLUGIN_BUSY);
    check_equal(cmeta_plugin_registry_request_stop(
        &registry, old_ref), CMETA_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_unload(
        &registry, old_ref), CMETA_PLUGIN_BUSY);
    check_equal(cmeta_plugin_registry_get_lifecycle(
        &registry, old_ref, &info), CMETA_PLUGIN_OK);
    check_true(info.active_leases > 0u);

    /* Admission moves forward atomically. A stale expected-41 projection
     * must be rejected rather than silently executing its N callback in N+1. */
    check_equal(salts_component_plugin_scope_acquire(
        &runtime, &stale_scope), SALTS_COMPONENT_PLUGIN_OK);
    check_equal(salts_component_plugin_scope_generation_id(&stale_scope),
                UINT64_C(42));
    check_true(salts_component_plugin_scope_generation_id(
        &stale_scope) != UINT64_C(41));
    check_equal(salts_component_plugin_scope_release(
        &stale_scope), SALTS_COMPONENT_PLUGIN_OK);
    check_equal(salts_component_plugin_scope_acquire(
        &runtime, &new_scope), SALTS_COMPONENT_PLUGIN_OK);
    check_equal(salts_component_plugin_scope_generation_id(&new_scope),
                UINT64_C(42));
    check_true(provider_dso_bind(&new_scope, &new_call.gate));
    new_call.result = praktor_dso_gate_run(
        &new_call.gate, &new_call.control);
    check_equal(new_call.result, 42);
    check_true(atomic_load_explicit(
        &new_call.control.exited, memory_order_acquire));

    racer.runtime = &runtime;
    racer.old_generation = &n.generation;
    racer.registry = &registry;
    racer.old_ref = old_ref;
    atomic_init(&racer.ready, 0);
    atomic_init(&racer.go, 0);
    check_equal(cmeta_thread_create(
        &race_thread, provider_dso_concurrent_unload, &racer), SALTS_OK);
    {
      const uint64_t stop = cmeta_monotonic_ms() + UINT64_C(10000);
      while (!atomic_load_explicit(&racer.ready, memory_order_acquire) &&
             cmeta_monotonic_ms() < stop)
        cmeta_thread_yield();
    }
    ready = atomic_load_explicit(&racer.ready, memory_order_acquire);
    check_true(ready);
    atomic_store_explicit(&racer.go, 1, memory_order_release);
    check_equal(salts_component_plugin_runtime_close(
        &runtime, &retired), SALTS_COMPONENT_PLUGIN_OK);
    check_true(retired == &n1.generation);
    check_equal(cmeta_thread_join(&race_thread), SALTS_OK);
    check_equal(racer.drain_result, SALTS_COMPONENT_PLUGIN_BUSY);
    check_equal(racer.unload_result, CMETA_PLUGIN_BUSY);
    check_equal(salts_component_plugin_generation_drain(
        &runtime, &n1.generation), SALTS_COMPONENT_PLUGIN_BUSY);
    check_equal(cmeta_plugin_registry_unload(
        &registry, new_ref), CMETA_PLUGIN_BUSY);

    atomic_store_explicit(&host.cancel, 1, memory_order_release);
    check_equal(cmeta_thread_join(&callback_thread), SALTS_OK);
    check_equal(old_call.result, -41);
    check_true(atomic_load_explicit(
        &old_call.control.exited, memory_order_acquire));
    /* A completed callback does not remove the caller-held scope. */
    check_equal(salts_component_plugin_generation_drain(
        &runtime, &n.generation), SALTS_COMPONENT_PLUGIN_BUSY);
    check_equal(salts_component_plugin_scope_release(
        &old_scope), SALTS_COMPONENT_PLUGIN_OK);
    check_equal(salts_component_plugin_generation_drain(
        &runtime, &n.generation), SALTS_COMPONENT_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_poll_quiescent(
        &registry, old_ref, &quiet), CMETA_PLUGIN_OK);
    check_true(quiet);
    /* Platform Plugin backend reaches actual dlclose / FreeLibrary. */
    check_equal(cmeta_plugin_registry_unload(&registry, old_ref),
                CMETA_PLUGIN_OK);

    check_equal(salts_component_plugin_scope_release(
        &new_scope), SALTS_COMPONENT_PLUGIN_OK);
    check_equal(salts_component_plugin_generation_drain(
        &runtime, &n1.generation), SALTS_COMPONENT_PLUGIN_OK);
    check_equal(salts_component_plugin_runtime_destroy(&runtime),
                SALTS_COMPONENT_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_request_stop(
        &registry, new_ref), CMETA_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_poll_quiescent(
        &registry, new_ref, &quiet), CMETA_PLUGIN_OK);
    check_true(quiet);
    check_equal(cmeta_plugin_registry_unload(
        &registry, new_ref), CMETA_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_destroy(&registry), CMETA_PLUGIN_OK);
  }
  it("races final scope release against drain and real module unload after callback terminal") {
    const cmeta_plugin_registry_config plugin_config = {1};
    cmeta_plugin_registry registry = {0};
    cmeta_plugin_ref ref = {0};
    provider_generation_fixture n = {0};
    salts_component_plugin_runtime runtime = SALTS_COMPONENT_PLUGIN_RUNTIME_INIT;
    salts_component_plugin_generation *retired = NULL;
    salts_component_plugin_scope scope = SALTS_COMPONENT_PLUGIN_SCOPE_INIT;
    provider_dso_host host = {0};
    provider_dso_call call = {0};
    provider_dso_terminal_race race = {0};
    cmeta_thread_t callback_thread = {0};
    cmeta_thread_t drainer = {0}, releaser = {0};
    int entered, busy_ready;
    const uint64_t stop = cmeta_monotonic_ms() + UINT64_C(10000);

    atomic_init(&host.cancel, 0);
    provider_dso_control_init(&call.control, &host, 1);
    check_equal(cmeta_plugin_registry_init(&registry, &plugin_config),
                CMETA_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_load(
        &registry, PRAKTOR_DSO_N_PATH, &ref), CMETA_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_start(&registry, ref), CMETA_PLUGIN_OK);
    check_equal(provider_dso_build(&n, UINT64_C(41), &registry, ref),
                SALTS_COMPONENT_PLUGIN_OK);
    check_equal(salts_component_plugin_runtime_init(&runtime),
                SALTS_COMPONENT_PLUGIN_OK);
    check_equal(salts_component_plugin_runtime_publish(
        &runtime, &n.generation, &retired), SALTS_COMPONENT_PLUGIN_OK);
    check_null(retired);
    check_equal(salts_component_plugin_scope_acquire(
        &runtime, &scope), SALTS_COMPONENT_PLUGIN_OK);
    check_true(provider_dso_bind(&scope, &call.gate));
    check_equal(cmeta_thread_create(
        &callback_thread, provider_dso_invoke, &call), SALTS_OK);
    while (!atomic_load_explicit(&call.control.entered, memory_order_acquire) &&
           cmeta_monotonic_ms() < stop)
      cmeta_thread_yield();
    entered = atomic_load_explicit(
        &call.control.entered, memory_order_acquire);
    check_true(entered);

    check_equal(salts_component_plugin_runtime_close(
        &runtime, &retired), SALTS_COMPONENT_PLUGIN_OK);
    check_true(retired == &n.generation);
    check_equal(cmeta_plugin_registry_request_stop(
        &registry, ref), CMETA_PLUGIN_OK);

    race.runtime = &runtime;
    race.generation = &n.generation;
    race.scope = &scope;
    race.registry = &registry;
    race.ref = ref;
    atomic_init(&race.initial_busy_ready, 0);
    atomic_init(&race.release_gate, 0);
    atomic_init(&race.drain_first_observed, 0);
    atomic_init(&race.release_completed, 0);
    atomic_init(&race.timed_out, 0);
    {
      const char *order = getenv("TURBO_DSO_RACE_ORDER");
      race.schedule = !order || !order[0] ? 0 :
          strcmp(order, "drain-first") == 0 ? 1 :
          strcmp(order, "release-first") == 0 ? 2 : -1;
      check_true(race.schedule >= 0);
    }
    check_equal(cmeta_thread_create(
        &drainer, provider_dso_terminal_drainer, &race), SALTS_OK);
    while (!atomic_load_explicit(&race.initial_busy_ready,
                                 memory_order_acquire) &&
           cmeta_monotonic_ms() < stop)
      cmeta_thread_yield();
    busy_ready = atomic_load_explicit(
        &race.initial_busy_ready, memory_order_acquire);
    check_true(busy_ready);
    check_equal(race.initial_drain, SALTS_COMPONENT_PLUGIN_BUSY);
    check_equal(race.initial_unload, CMETA_PLUGIN_BUSY);

    /* The completion itself does NOT retire an application-owned scope. */
    atomic_store_explicit(&host.cancel, 1, memory_order_release);
    check_equal(cmeta_thread_join(&callback_thread), SALTS_OK);
    check_equal(call.result, -41);
    check_true(atomic_load_explicit(
        &call.control.exited, memory_order_acquire));
    check_equal(salts_component_plugin_generation_drain(
        &runtime, &n.generation), SALTS_COMPONENT_PLUGIN_BUSY);

    check_equal(cmeta_thread_create(
        &releaser, provider_dso_terminal_releaser, &race), SALTS_OK);
    atomic_store_explicit(&race.release_gate, 1, memory_order_release);
    check_equal(cmeta_thread_join(&releaser), SALTS_OK);
    check_equal(cmeta_thread_join(&drainer), SALTS_OK);
    check_equal(atomic_load_explicit(&race.timed_out, memory_order_acquire), 0);
    check_equal(race.scope_release, SALTS_COMPONENT_PLUGIN_OK);
    check_true(atomic_load_explicit(&race.release_completed,
                                     memory_order_acquire));
    if (race.schedule == 1) {
      check_true(atomic_load_explicit(&race.drain_first_observed,
                                      memory_order_acquire));
      check_true(race.busy_polls > 0);
    }
    if (race.schedule == 2) check_equal(race.busy_polls, 0);
    check_equal(race.final_drain, SALTS_COMPONENT_PLUGIN_OK);
    check_equal(race.poll_result, CMETA_PLUGIN_OK);
    check_true(race.quiescent);
    check_equal(race.final_unload, CMETA_PLUGIN_OK);
    check_equal(salts_component_plugin_runtime_destroy(
        &runtime), SALTS_COMPONENT_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_destroy(&registry), CMETA_PLUGIN_OK);
  }

}
