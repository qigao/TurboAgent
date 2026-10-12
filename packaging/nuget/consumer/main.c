#include "turbo_runtime_store.h"

/* Uses exported RuntimeCore symbols from an installed SDK, never source files. */
int main(void) {
  turbo_agent_runtime_store_t store = turbo_agent_runtime_store_memory_create();
  if (!store.put || !store.get || !store.list) {
    if (store.user_data_free) store.user_data_free(store.user_data);
    return 1;
  }
  turbo_agent_runtime_t *runtime = turbo_agent_runtime_create(&store);
  if (!runtime) {
    if (store.user_data_free) store.user_data_free(store.user_data);
    return 2;
  }
  turbo_agent_runtime_destroy(runtime);
  return 0;
}
