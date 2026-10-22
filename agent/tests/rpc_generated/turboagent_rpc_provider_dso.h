#ifndef TURBOAGENT_RPC_PROVIDER_DSO_H
#define TURBOAGENT_RPC_PROVIDER_DSO_H

#include <salts/plugin_decl.h>
#include <data_bind_native_binding.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

/* One shared probe ABI across the host test and the actual loaded DSO. */
typedef struct turboagent_rpc_provider_dso_probe {
  atomic_int enabled;
  atomic_int entered;
  atomic_int release;
  /* Count actual provider entries, including unblocked requests. */
  atomic_int invoked;
} turboagent_rpc_provider_dso_probe;

typedef struct turboagent_rpc_provider_dso_offer {
  DataBindNativeInvokeFn invoke;
} turboagent_rpc_provider_dso_offer;

#define TURBOAGENT_RPC_PROVIDER_DSO_EXPORT "chttp.rpc.dso.invoke"
#define TURBOAGENT_RPC_PROVIDER_DSO_CONTRACT "test.turboagent.rpc.dso.invoke.v1"
enum { TURBOAGENT_RPC_PROVIDER_DSO_VERSION = 1 };

static const cmeta_type_identity TURBOAGENT_RPC_DSO_OFFER_ID =
    CMETA_TYPE_ID_ATOM_INIT("test.turboagent.rpc.dso.offer.v1");
static const cmeta_type_identity TURBOAGENT_RPC_DSO_OFFER_CONST =
    CMETA_TYPE_ID_CONST_INIT(&TURBOAGENT_RPC_DSO_OFFER_ID);
static const cmeta_type_identity TURBOAGENT_RPC_DSO_OFFER_POINTER_ID =
    CMETA_TYPE_ID_POINTER_INIT(&TURBOAGENT_RPC_DSO_OFFER_CONST);
static const cmeta_type_desc TURBOAGENT_RPC_DSO_OFFER_TYPE = {
    "const turboagent_rpc_provider_dso_offer",
    sizeof(turboagent_rpc_provider_dso_offer),
    CMETA_ALIGNOF(turboagent_rpc_provider_dso_offer),
    CMETA_T_OBJECT, NULL, NULL, &TURBOAGENT_RPC_DSO_OFFER_CONST};
static const cmeta_type_desc TURBOAGENT_RPC_DSO_OFFER_POINTER_TYPE = {
    "const turboagent_rpc_provider_dso_offer *",
    sizeof(const turboagent_rpc_provider_dso_offer *),
    CMETA_ALIGNOF(const turboagent_rpc_provider_dso_offer *),
    CMETA_T_POINTER, &TURBOAGENT_RPC_DSO_OFFER_TYPE, NULL,
    &TURBOAGENT_RPC_DSO_OFFER_POINTER_ID};

/* A reflected function export is the only way for the test to obtain the
 * provider's callback address. The registry's borrow is kept until mount. */
Function0InvokeDeclAsAbiResult(
    value, const turboagent_rpc_provider_dso_offer *,
    &TURBOAGENT_RPC_DSO_OFFER_POINTER_TYPE, CMETA_ABI_OBJECT_POINTER,
    CMETA_RESULT_BORROWED, turboagent_rpc_provider_dso_get_offer);

#endif
