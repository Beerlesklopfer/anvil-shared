/*
 * anvil_iox_wrapper.h - Anvil thin wrapper around IceOryx2 C binding
 *
 * Part of the Anvil IPC transport layer for ForgeIEC / OpenPLC v3.
 *
 * This header defines a minimal, stable C API that the auto-generated
 * iceoryx2_bridge.cpp uses. The actual IceOryx2 C FFI details are
 * encapsulated in anvil_iox_wrapper.cpp, isolating the generated code
 * from IceOryx2 API changes.
 *
 * Copyright (C) 2025-2026 Blacksmith (blacksmith@forgeiec.io)
 * Licensed under GPL v3 (same as OpenPLC v3)
 */

#ifndef ANVIL_IOX_WRAPPER_H
#define ANVIL_IOX_WRAPPER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque handle types */
typedef struct anvil_node_s*       anvil_node_t;
typedef struct anvil_publisher_s*  anvil_pub_t;
typedef struct anvil_subscriber_s* anvil_sub_t;

/*
 * Create an IceOryx2 node with the given name.
 * Returns NULL on failure.
 */
anvil_node_t anvil_node_create(const char *name);

/*
 * Destroy the IceOryx2 node and release all resources.
 */
void anvil_node_destroy(anvil_node_t node);

/*
 * Create a publisher on the given service name with the specified
 * maximum payload size. Returns NULL on failure.
 */
anvil_pub_t anvil_pub_create(anvil_node_t node, const char *service_name,
                         size_t payload_size);

/*
 * Destroy a publisher.
 */
void anvil_pub_destroy(anvil_pub_t pub);

/*
 * Publish data by copying `size` bytes from `data` to a loaned sample
 * and sending it. Returns 0 on success, -1 on failure.
 */
int anvil_publish(anvil_pub_t pub, const void *data, size_t size);

/*
 * Create a subscriber on the given service name with the specified
 * maximum payload size. Returns NULL on failure.
 */
anvil_sub_t anvil_sub_create(anvil_node_t node, const char *service_name,
                         size_t payload_size);

/*
 * Destroy a subscriber.
 */
void anvil_sub_destroy(anvil_sub_t sub);

/*
 * Receive the latest sample and copy up to `size` bytes into `buf`.
 * Returns 0 if a sample was received, -1 if no data available.
 */
int anvil_receive(anvil_sub_t sub, void *buf, size_t size);

/*
 * Enable/disable diagnostic logging.
 * level 0 = silent (default), 1 = errors only, 2 = verbose (stats).
 * Can also be set via ANVIL_DEBUG environment variable.
 */
void anvil_set_debug(int level);

#ifdef __cplusplus
}
#endif

#endif /* ANVIL_IOX_WRAPPER_H */
