#include "turbo_agent_session_workflow_internal.h"

static turbo_graph_t *turbo_agent_session_create_graph_base(const turbo_agent_session_t *session,
                                                            const char *graph_name) {
  turbo_graph_t *graph;

  if (!session || !turbo_agent_session_agent(session) || !graph_name) {
    return NULL;
  }
  graph = turbo_graph_create(graph_name);
  return graph;
}

CXX_C_API turbo_graph_t *
turbo_agent_session_create_loop_graph(const turbo_agent_session_t *session) {
  turbo_graph_t *graph;

  graph = turbo_agent_session_create_graph_base(session, "session-loop");
  if (!graph) {
    return NULL;
  }
  if (turbo_agent_install_loop(graph, turbo_agent_session_agent(session), "model", "tools", "end",
                               1) != TURBO_GRAPH_EXEC_OK) {
    turbo_graph_destroy(graph);
    return NULL;
  }
  return graph;
}

CXX_C_API turbo_graph_t *
turbo_agent_session_create_review_graph(const turbo_agent_session_t *session) {
  turbo_graph_t *graph;

  graph = turbo_agent_session_create_graph_base(session, "session-review-loop");
  if (!graph) {
    return NULL;
  }
  if (turbo_agent_install_review_loop(graph, turbo_agent_session_agent(session),
                                      turbo_agent_session_agent(session), "planner", "plan_commit",
                                      "plan_step", "review", "executor", "tools", "plan_advance",
                                      "end", 1) != TURBO_GRAPH_EXEC_OK) {
    turbo_graph_destroy(graph);
    return NULL;
  }
  return graph;
}

CXX_C_API turbo_graph_t *
turbo_agent_session_create_engineering_graph(const turbo_agent_session_t *session) {
  turbo_graph_t *graph;

  graph = turbo_agent_session_create_graph_base(session, "session-engineering-loop");
  if (!graph) {
    return NULL;
  }
  if (turbo_agent_install_engineering_loop(
          graph, turbo_agent_session_agent(session), turbo_agent_session_agent(session), "planner",
          "plan_commit", "plan_step", "review", "executor", "tools", "detect_failure",
          "replan_route", "replan_prepare", "plan_advance", "end", 1) != TURBO_GRAPH_EXEC_OK) {
    turbo_graph_destroy(graph);
    return NULL;
  }
  return graph;
}

CXX_C_API turbo_graph_t *
turbo_agent_session_create_knowledge_engineering_graph(const turbo_agent_session_t *session) {
  const turbo_agent_knowledge_context_config_t *config =
      turbo_agent_session_knowledge_context_internal(session);
  turbo_graph_t *graph;

  graph = turbo_agent_session_create_graph_base(session, "session-knowledge-engineering-loop");
  if (!graph || !config || !config->store) {
    turbo_graph_destroy(graph);
    return NULL;
  }
  if (turbo_agent_install_knowledge_engineering_loop(
          graph, config, turbo_agent_session_agent(session), turbo_agent_session_agent(session),
          "knowledge", "planner", "plan_commit", "plan_step", "review", "executor", "tools",
          "detect_failure", "replan_route", "replan_prepare", "plan_advance", "end",
          1) != TURBO_GRAPH_EXEC_OK) {
    turbo_graph_destroy(graph);
    return NULL;
  }
  return graph;
}

CXX_C_API turbo_graph_t *
turbo_agent_session_create_retriever_engineering_graph(const turbo_agent_session_t *session) {
  const turbo_retriever_context_config_t *config =
      turbo_agent_session_retriever_context_internal(session);
  turbo_graph_t *graph;

  graph = turbo_agent_session_create_graph_base(session, "session-retriever-engineering-loop");
  if (!graph || !config || !config->retriever) {
    turbo_graph_destroy(graph);
    return NULL;
  }
  if (turbo_agent_install_retriever_engineering_loop(
          graph, config, turbo_agent_session_agent(session), turbo_agent_session_agent(session),
          "retriever", "planner", "plan_commit", "plan_step", "review", "executor", "tools",
          "detect_failure", "replan_route", "replan_prepare", "plan_advance", "end",
          1) != TURBO_GRAPH_EXEC_OK) {
    turbo_graph_destroy(graph);
    return NULL;
  }
  return graph;
}

CXX_C_API turbo_graph_t *
turbo_agent_session_create_preset_graph(const turbo_agent_session_t *session,
                                        turbo_agent_session_workflow_kind_t kind) {
  switch (kind) {
  case TURBO_AGENT_SESSION_WORKFLOW_LOOP:
    return turbo_agent_session_create_loop_graph(session);
  case TURBO_AGENT_SESSION_WORKFLOW_REVIEW:
    return turbo_agent_session_create_review_graph(session);
  case TURBO_AGENT_SESSION_WORKFLOW_ENGINEERING:
    return turbo_agent_session_create_engineering_graph(session);
  case TURBO_AGENT_SESSION_WORKFLOW_KNOWLEDGE_ENGINEERING:
    return turbo_agent_session_create_knowledge_engineering_graph(session);
  case TURBO_AGENT_SESSION_WORKFLOW_RETRIEVER_ENGINEERING:
    return turbo_agent_session_create_retriever_engineering_graph(session);
  default:
    return NULL;
  }
}
