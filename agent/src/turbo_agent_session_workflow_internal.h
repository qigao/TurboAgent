#ifndef TURBO_AGENT_SESSION_WORKFLOW_INTERNAL_H
#define TURBO_AGENT_SESSION_WORKFLOW_INTERNAL_H

#include "turbo_agent_knowledge_store.h"
#include "turbo_agent_session.h"
#include "turbo_retriever.h"

/* Borrow persistent Session storage, not a temporary copy: graph nodes retain
 * these addresses. Session and the borrowed backend must outlive graph use.
 * Factories require quiescence; neither accessor transfers ownership. */
const turbo_agent_knowledge_context_config_t *
turbo_agent_session_knowledge_context_internal(const turbo_agent_session_t *session);

const turbo_retriever_context_config_t *
turbo_agent_session_retriever_context_internal(const turbo_agent_session_t *session);

#endif
