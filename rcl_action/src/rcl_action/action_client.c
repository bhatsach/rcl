// Copyright 2018 Open Source Robotics Foundation, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifdef __cplusplus
extern "C"
{
#endif

#include "rcl_action/action_client.h"

#include "rcl_action/default_qos.h"
#include "rcl_action/names.h"
#include "rcl_action/types.h"
#include "rcl_action/wait.h"

#include "rcl/client.h"
#include "rcl/error_handling.h"
#include "rcl/subscription.h"
#include "rcl/types.h"
#include "rcl/wait.h"

#include "rcutils/logging_macros.h"
#include "rcutils/strdup.h"

#include "rmw/qos_profiles.h"
#include "rmw/types.h"


typedef struct rcl_action_client_impl_t
{
  rcl_client_t goal_client;
  rcl_client_t cancel_client;
  rcl_client_t result_client;
  rcl_subscription_t feedback_subscription;
  rcl_subscription_t status_subscription;
  rcl_action_client_options_t options;
  char * action_name;
} rcl_action_client_impl_t;

rcl_action_client_t
rcl_action_get_zero_initialized_client(void)
{
  static rcl_action_client_t null_action_client = {0};
  return null_action_client;
}

// \internal Initializes an action client specific service client.
#define CLIENT_INIT(Type) \
  char * Type ## _service_name = NULL; \
  ret = rcl_action_get_ ## Type ## _service_name(action_name, allocator, &Type ## _service_name); \
  if (RCL_RET_OK != ret) { \
    RCL_SET_ERROR_MSG("failed to get " #Type " service name"); \
    if (RCL_RET_BAD_ALLOC == ret) { \
      ret = RCL_RET_BAD_ALLOC; \
    } else { \
      ret = RCL_RET_ERROR; \
    } \
    goto fail; \
  } \
  rcl_client_options_t Type ## _service_client_options = { \
    .qos = options->Type ## _service_qos, .allocator = allocator \
  }; \
  action_client->impl->Type ## _client = rcl_get_zero_initialized_client(); \
  ret = rcl_client_init( \
    &action_client->impl->Type ## _client, \
    node, \
    type_support->Type ## _service_type_support, \
    Type ## _service_name, \
    &Type ## _service_client_options); \
  allocator.deallocate(Type ## _service_name, allocator.state); \
  if (RCL_RET_OK != ret) { \
    if (RCL_RET_BAD_ALLOC == ret) { \
      ret = RCL_RET_BAD_ALLOC; \
    } else if (RCL_RET_SERVICE_NAME_INVALID == ret) { \
      ret = RCL_RET_ACTION_NAME_INVALID; \
    } else { \
      ret = RCL_RET_ERROR; \
    } \
    goto fail; \
  }

// \internal Initializes an action client specific topic subscription.
#define SUBSCRIPTION_INIT(Type) \
  char * Type ## _topic_name = NULL; \
  ret = rcl_action_get_ ## Type ## _topic_name(action_name, allocator, &Type ## _topic_name); \
  if (RCL_RET_OK != ret) { \
    RCL_SET_ERROR_MSG("failed to get " #Type " topic name"); \
    if (RCL_RET_BAD_ALLOC == ret) { \
      ret = RCL_RET_BAD_ALLOC; \
    } else { \
      ret = RCL_RET_ERROR; \
    } \
    goto fail; \
  } \
  rcl_subscription_options_t Type ## _topic_subscription_options = { \
    .qos = options->Type ## _topic_qos, \
    .ignore_local_publications = false, \
    .allocator = allocator \
  }; \
  action_client->impl->Type ## _subscription = rcl_get_zero_initialized_subscription(); \
  ret = rcl_subscription_init( \
    &action_client->impl->Type ## _subscription, \
    node, \
    type_support->Type ## _message_type_support, \
    Type ## _topic_name, \
    &Type ## _topic_subscription_options); \
  allocator.deallocate(Type ## _topic_name, allocator.state); \
  if (RCL_RET_OK != ret) { \
    if (RCL_RET_BAD_ALLOC == ret) { \
      ret = RCL_RET_BAD_ALLOC; \
    } else if (RCL_RET_TOPIC_NAME_INVALID == ret) { \
      ret = RCL_RET_ACTION_NAME_INVALID; \
    } else { \
      ret = RCL_RET_ERROR; \
    } \
    goto fail; \
  }

rcl_ret_t
rcl_action_client_init(
  rcl_action_client_t * action_client,
  rcl_node_t * node,
  const rosidl_action_type_support_t * type_support,
  const char * action_name,
  const rcl_action_client_options_t * options)
{
  RCL_CHECK_ARGUMENT_FOR_NULL(action_client, RCL_RET_INVALID_ARGUMENT);
  if (!rcl_node_is_valid(node)) {
    return RCL_RET_NODE_INVALID;
  }
  RCL_CHECK_ARGUMENT_FOR_NULL(type_support, RCL_RET_INVALID_ARGUMENT);
  RCL_CHECK_ARGUMENT_FOR_NULL(action_name, RCL_RET_INVALID_ARGUMENT);
  RCL_CHECK_ARGUMENT_FOR_NULL(options, RCL_RET_INVALID_ARGUMENT);
  rcl_allocator_t allocator = options->allocator;
  RCL_CHECK_ALLOCATOR_WITH_MSG(&allocator, "invalid allocator", return RCL_RET_INVALID_ARGUMENT);

  rcl_ret_t ret = RCL_RET_OK;
  rcl_ret_t fini_ret = RCL_RET_OK;
  RCUTILS_LOG_DEBUG_NAMED(
    ROS_PACKAGE_NAME, "Initializing client for action name '%s'", action_name);
  if (NULL != action_client->impl) {
    RCL_SET_ERROR_MSG("action client already initialized, or memory was uninitialized");
    return RCL_RET_ALREADY_INIT;
  }
  // Allocate space for the implementation struct.
  action_client->impl = allocator.allocate(sizeof(rcl_action_client_impl_t), allocator.state);
  RCL_CHECK_FOR_NULL_WITH_MSG(
    action_client->impl, "allocating memory failed", return RCL_RET_BAD_ALLOC);

  // Copy action client name and options.
  action_client->impl->action_name = rcutils_strdup(action_name, allocator);
  if (NULL == action_client->impl->action_name) {
    ret = RCL_RET_BAD_ALLOC;
    goto fail;
  }
  action_client->impl->options = *options;

  // Initialize action service clients.
  CLIENT_INIT(goal);
  CLIENT_INIT(cancel);
  CLIENT_INIT(result);

  // Initialize action topic subscriptions.
  SUBSCRIPTION_INIT(feedback);
  SUBSCRIPTION_INIT(status);

  RCUTILS_LOG_DEBUG_NAMED(ROS_PACKAGE_NAME, "Action client initialized");
  return ret;
fail:
  fini_ret = rcl_action_client_fini(action_client, node);
  if (RCL_RET_OK != fini_ret) {
    RCL_SET_ERROR_MSG("failed to cleanup action client");
    ret = RCL_RET_ERROR;
  }
  return ret;
}

rcl_ret_t
rcl_action_client_fini(rcl_action_client_t * action_client, rcl_node_t * node)
{
  RCUTILS_LOG_DEBUG_NAMED(ROS_PACKAGE_NAME, "Finalizing action client");
  if (!rcl_action_client_is_valid(action_client)) {
    return RCL_RET_ACTION_CLIENT_INVALID;  // error already set
  }
  if (!rcl_node_is_valid(node)) {
    return RCL_RET_NODE_INVALID;  // error already set
  }
  rcl_ret_t ret = RCL_RET_OK;
  if (RCL_RET_OK != rcl_client_fini(&action_client->impl->goal_client, node)) {
    ret = RCL_RET_ERROR;
  }
  if (RCL_RET_OK != rcl_client_fini(&action_client->impl->cancel_client, node)) {
    ret = RCL_RET_ERROR;
  }
  if (RCL_RET_OK != rcl_client_fini(&action_client->impl->result_client, node)) {
    ret = RCL_RET_ERROR;
  }
  if (RCL_RET_OK != rcl_subscription_fini(&action_client->impl->feedback_subscription, node)) {
    ret = RCL_RET_ERROR;
  }
  if (RCL_RET_OK != rcl_subscription_fini(&action_client->impl->status_subscription, node)) {
    ret = RCL_RET_ERROR;
  }
  rcl_allocator_t * allocator = &action_client->impl->options.allocator;
  allocator->deallocate(action_client->impl->action_name, allocator->state);
  allocator->deallocate(action_client->impl, allocator->state);
  action_client->impl = NULL;
  RCUTILS_LOG_DEBUG_NAMED(ROS_PACKAGE_NAME, "Action client finalized");
  return ret;
}

rcl_action_client_options_t
rcl_action_client_get_default_options(void)
{
  static rcl_action_client_options_t default_options;
  default_options.goal_service_qos = rmw_qos_profile_services_default;
  default_options.cancel_service_qos = rmw_qos_profile_services_default;
  default_options.result_service_qos = rmw_qos_profile_services_default;
  default_options.feedback_topic_qos = rmw_qos_profile_default;
  default_options.status_topic_qos = rcl_action_qos_profile_status_default;
  default_options.allocator = rcl_get_default_allocator();
  return default_options;
}

// \internal Sends an action client specific service request.
#define SEND_SERVICE_REQUEST(Type) \
  RCUTILS_LOG_DEBUG_NAMED(ROS_PACKAGE_NAME, "Sending action " #Type " request"); \
  if (!rcl_action_client_is_valid(action_client)) { \
    return RCL_RET_ACTION_SERVER_INVALID;  /* error already set */ \
  } \
  RCL_CHECK_ARGUMENT_FOR_NULL(ros_ ## Type ## _request, RCL_RET_INVALID_ARGUMENT); \
  int64_t sequence_number;  /* ignored */ \
  rcl_ret_t ret = rcl_send_request( \
    &action_client->impl->Type ## _client, ros_ ## Type ## _request, &sequence_number); \
  if (RCL_RET_OK != ret) { \
    return RCL_RET_ERROR;  /* error already set */ \
  } \
  RCUTILS_LOG_DEBUG_NAMED(ROS_PACKAGE_NAME, "Action " #Type " request sent"); \
  return RCL_RET_OK;

// \internal Takes an action client specific service response.
#define TAKE_SERVICE_RESPONSE(Type) \
  RCUTILS_LOG_DEBUG_NAMED(ROS_PACKAGE_NAME, "Taking action " #Type " response"); \
  if (!rcl_action_client_is_valid(action_client)) { \
    return RCL_RET_ACTION_SERVER_INVALID;  /* error already set */ \
  } \
  RCL_CHECK_ARGUMENT_FOR_NULL(ros_ ## Type ## _response, RCL_RET_INVALID_ARGUMENT); \
  rmw_request_id_t request_header;  /* ignored */ \
  rcl_ret_t ret = rcl_take_response( \
    &action_client->impl->Type ## _client, &request_header, ros_ ## Type ## _response); \
  if (RCL_RET_OK != ret) { \
    if (RCL_RET_BAD_ALLOC == ret) { \
      return RCL_RET_BAD_ALLOC;  /* error already set */ \
    } \
    if (RCL_RET_CLIENT_TAKE_FAILED == ret) { \
      return RCL_RET_ACTION_CLIENT_TAKE_FAILED; \
    } \
    return RCL_RET_ERROR;  /* error already set */ \
  } \
  RCUTILS_LOG_DEBUG_NAMED(ROS_PACKAGE_NAME, "Action " #Type " response taken"); \
  return RCL_RET_OK;


rcl_ret_t
rcl_action_send_goal_request(
  const rcl_action_client_t * action_client,
  const void * ros_goal_request)
{
  SEND_SERVICE_REQUEST(goal)
}

rcl_ret_t
rcl_action_take_goal_response(
  const rcl_action_client_t * action_client,
  void * ros_goal_response)
{
  TAKE_SERVICE_RESPONSE(goal);
}

rcl_ret_t
rcl_action_send_result_request(
  const rcl_action_client_t * action_client,
  const void * ros_result_request)
{
  SEND_SERVICE_REQUEST(result);
}

rcl_ret_t
rcl_action_take_result_response(
  const rcl_action_client_t * action_client,
  void * ros_result_response)
{
  TAKE_SERVICE_RESPONSE(result);
}

rcl_ret_t
rcl_action_send_cancel_request(
  const rcl_action_client_t * action_client,
  const void * ros_cancel_request)
{
  SEND_SERVICE_REQUEST(cancel);
}

rcl_ret_t
rcl_action_take_cancel_response(
  const rcl_action_client_t * action_client,
  void * ros_cancel_response)
{
  TAKE_SERVICE_RESPONSE(cancel);
}

// \internal Takes an action client specific topic message.
#define TAKE_MESSAGE(Type) \
  RCUTILS_LOG_DEBUG_NAMED(ROS_PACKAGE_NAME, "Taking action " #Type); \
  if (!rcl_action_client_is_valid(action_client)) { \
    return RCL_RET_ACTION_CLIENT_INVALID;  /* error already set */ \
  } \
  RCL_CHECK_ARGUMENT_FOR_NULL(ros_ ## Type, RCL_RET_INVALID_ARGUMENT); \
  rmw_message_info_t message_info; /* ignored */ \
  rcl_ret_t ret = rcl_take( \
    &action_client->impl->Type ## _subscription, ros_ ## Type, &message_info); \
  if (RCL_RET_OK != ret) { \
    if (RCL_RET_SUBSCRIPTION_TAKE_FAILED == ret) { \
      return RCL_RET_ACTION_CLIENT_TAKE_FAILED; \
    } \
    if (RCL_RET_BAD_ALLOC == ret) { \
      return RCL_RET_BAD_ALLOC; \
    } \
    return RCL_RET_ERROR; \
  } \
  RCUTILS_LOG_DEBUG_NAMED(ROS_PACKAGE_NAME, "Action " #Type " taken"); \
  return RCL_RET_OK;

rcl_ret_t
rcl_action_take_feedback(
  const rcl_action_client_t * action_client,
  void * ros_feedback)
{
  TAKE_MESSAGE(feedback);
}

rcl_ret_t
rcl_action_take_status(
  const rcl_action_client_t * action_client,
  void * ros_status)
{
  TAKE_MESSAGE(status);
}

const char *
rcl_action_client_get_action_name(const rcl_action_client_t * action_client)
{
  if (!rcl_action_client_is_valid(action_client)) {
    return NULL;
  }
  return action_client->impl->action_name;
}

const rcl_action_client_options_t *
rcl_action_client_get_options(const rcl_action_client_t * action_client)
{
  if (!rcl_action_client_is_valid(action_client)) {
    return NULL;
  }
  return &action_client->impl->options;
}

bool
rcl_action_client_is_valid(const rcl_action_client_t * action_client)
{
  RCL_CHECK_FOR_NULL_WITH_MSG(
    action_client, "action client pointer is invalid", return false);
  RCL_CHECK_FOR_NULL_WITH_MSG(
    action_client->impl, "action client implementation is invalid", return false);
  if (!rcl_client_is_valid(&action_client->impl->goal_client)) {
    RCL_SET_ERROR_MSG("goal client is invalid");
    return false;
  }
  if (!rcl_client_is_valid(&action_client->impl->cancel_client)) {
    RCL_SET_ERROR_MSG("cancel client is invalid");
    return false;
  }
  if (!rcl_client_is_valid(&action_client->impl->result_client)) {
    RCL_SET_ERROR_MSG("result client is invalid");
    return false;
  }
  if (!rcl_subscription_is_valid(&action_client->impl->feedback_subscription)) {
    RCL_SET_ERROR_MSG("feedback subscription is invalid");
    return false;
  }
  if (!rcl_subscription_is_valid(&action_client->impl->status_subscription)) {
    RCL_SET_ERROR_MSG("status subscription is invalid");
    return false;
  }
  return true;
}

rcl_ret_t
rcl_action_wait_set_add_action_client(
  rcl_wait_set_t * wait_set,
  const rcl_action_client_t * action_client)
{
  rcl_ret_t ret;
  RCL_CHECK_ARGUMENT_FOR_NULL(wait_set, RCL_RET_WAIT_SET_INVALID);
  if (!rcl_action_client_is_valid(action_client)) {
    return RCL_RET_ACTION_CLIENT_INVALID;  // error already set
  }
  // Wait on action goal service response messages.
  ret = rcl_wait_set_add_client(wait_set, &action_client->impl->goal_client);
  if (RCL_RET_OK != ret) {
    return ret;
  }
  // Wait on action cancel service response messages.
  ret = rcl_wait_set_add_client(wait_set, &action_client->impl->cancel_client);
  if (RCL_RET_OK != ret) {
    return ret;
  }
  // Wait on action result service response messages.
  ret = rcl_wait_set_add_client(wait_set, &action_client->impl->result_client);
  if (RCL_RET_OK != ret) {
    return ret;
  }
  // Wait on action feedback messages.
  ret = rcl_wait_set_add_subscription(wait_set, &action_client->impl->feedback_subscription);
  if (RCL_RET_OK != ret) {
    return ret;
  }
  return RCL_RET_OK;
  // Wait on action status messages.
  ret = rcl_wait_set_add_subscription(wait_set, &action_client->impl->status_subscription);
  if (RCL_RET_OK != ret) {
    return ret;
  }
  return RCL_RET_OK;
}

rcl_ret_t
rcl_action_client_wait_set_get_num_entities(
  const rcl_action_client_t * action_client,
  size_t * num_subscriptions,
  size_t * num_guard_conditions,
  size_t * num_timers,
  size_t * num_clients,
  size_t * num_services)
{
  if (!rcl_action_client_is_valid(action_client)) {
    return RCL_RET_ACTION_CLIENT_INVALID;  // error already set
  }
  RCL_CHECK_ARGUMENT_FOR_NULL(num_subscriptions, RCL_RET_INVALID_ARGUMENT);
  RCL_CHECK_ARGUMENT_FOR_NULL(num_guard_conditions, RCL_RET_INVALID_ARGUMENT);
  RCL_CHECK_ARGUMENT_FOR_NULL(num_timers, RCL_RET_INVALID_ARGUMENT);
  RCL_CHECK_ARGUMENT_FOR_NULL(num_clients, RCL_RET_INVALID_ARGUMENT);
  RCL_CHECK_ARGUMENT_FOR_NULL(num_services, RCL_RET_INVALID_ARGUMENT);
  *num_subscriptions = 2;
  *num_guard_conditions = 0;
  *num_timers = 0;
  *num_clients = 3;
  *num_services = 0;
  return RCL_RET_OK;
}

rcl_ret_t
rcl_action_client_wait_set_get_entities_ready(
  const rcl_wait_set_t * wait_set,
  const rcl_action_client_t * action_client,
  bool * is_feedback_ready,
  bool * is_status_ready,
  bool * is_goal_response_ready,
  bool * is_cancel_response_ready,
  bool * is_result_response_ready)
{
  if (!rcl_action_client_is_valid(action_client)) {
    return RCL_RET_ACTION_CLIENT_INVALID;  // error already set
  }
  RCL_CHECK_ARGUMENT_FOR_NULL(is_feedback_ready, RCL_RET_INVALID_ARGUMENT);
  RCL_CHECK_ARGUMENT_FOR_NULL(is_status_ready, RCL_RET_INVALID_ARGUMENT);
  RCL_CHECK_ARGUMENT_FOR_NULL(is_goal_response_ready, RCL_RET_INVALID_ARGUMENT);
  RCL_CHECK_ARGUMENT_FOR_NULL(is_cancel_response_ready, RCL_RET_INVALID_ARGUMENT);
  RCL_CHECK_ARGUMENT_FOR_NULL(is_result_response_ready, RCL_RET_INVALID_ARGUMENT);
  if (2 != wait_set->size_of_subscriptions || 3 != wait_set->size_of_clients) {
    RCL_SET_ERROR_MSG("wait set not initialized or not used by the action client alone");
    return RCL_RET_WAIT_SET_INVALID;
  }
  *is_feedback_ready = !!wait_set->subscriptions[0];
  *is_status_ready = !!wait_set->subscriptions[1];
  *is_goal_response_ready = !!wait_set->clients[0];
  *is_cancel_response_ready = !!wait_set->clients[1];
  *is_result_response_ready = !!wait_set->clients[2];
  return RCL_RET_OK;
}


#ifdef __cplusplus
}
#endif