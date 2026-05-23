/*
 * anvil_iox_wrapper.cpp - Anvil thin wrapper around IceOryx2 C binding
 *
 * Part of the Anvil IPC transport layer for ForgeIEC / OpenPLC v3.
 *
 * When _anvil_src is defined, this file uses the real IPC FFI.
 * Otherwise it compiles as a no-op stub so the build succeeds without
 * IceOryx2 installed.
 *
 * Copyright (C) 2025-2026 Blacksmith (blacksmith@forgeiec.io)
 * Licensed under GPL v3 (same as OpenPLC v3)
 */

#include "anvil_iox_wrapper.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <mutex>
#include <string>

/* Debug level: 0=silent, 1=errors, 2=verbose (stats) */
static int anvil_debug = 0;

void anvil_set_debug(int level) {
    anvil_debug = level;
    if (level > 0)
        printf("Anvil: debug level set to %d\n", level);
}

/* ─────────────────────────────────────────────────────────────────── */
/*  Build-info baked at compile time + ABI-mismatch diagnostics        */
/* ─────────────────────────────────────────────────────────────────── */

#ifndef ANVIL_WRAPPER_GIT_SHA
#define ANVIL_WRAPPER_GIT_SHA "unknown"
#endif

/* The constant is composed of __DATE__ + __TIME__ + " git:<sha>" — every
 * caller's binary picks up the values at THAT binary's compile, so two
 * processes that share an Anvil topic can compare strings to spot a
 * source-tree mismatch instantly.
 */
static const char *kAnvilWrapperBuildInfo =
    __DATE__ " " __TIME__ " git:" ANVIL_WRAPPER_GIT_SHA;

const char *anvil_wrapper_build_info(void) {
    return kAnvilWrapperBuildInfo;
}

/* Rate-limit the verbose ABI-mismatch diagnostic so a tight retry loop
 * in the calling code cannot spam syslog. Keyed by topic name, suppress
 * for 60 s after the last log.
 */
namespace {

std::mutex g_mismatch_log_mtx;
std::map<std::string, time_t> g_mismatch_last_log;

void log_abi_mismatch(const char *kind /* "pub" or "sub" */,
                       const char *service_name) {
    const time_t now = time(NULL);
    {
        std::lock_guard<std::mutex> lk(g_mismatch_log_mtx);
        const time_t last = g_mismatch_last_log[service_name];
        if (now - last < 60) {
            /* Already logged for this topic in the last minute — silent. */
            g_mismatch_last_log[service_name] = now;
            return;
        }
        g_mismatch_last_log[service_name] = now;
    }
    fprintf(stderr,
        "Anvil: ABI mismatch on '%s' (%s side) — this process built from "
        "anvil_wrapper at [%s].\n"
        "       Another process is using a DIFFERENT struct layout for the "
        "same topic.\n"
        "       Common causes:\n"
        "         1. anvild + tongs-* deployed older than the running "
        "forgeiec-plc (or vice versa).\n"
        "         2. shared/anvil_wrapper edited but only some binaries "
        "rebuilt.\n"
        "       Fix (production / Debian package install):\n"
        "         sudo apt update && sudo apt install --only-upgrade \\\n"
        "             forgeiec-studio anvild tongs-modbustcp\n"
        "         # then re-deploy the project from the editor.\n"
        "       Fix (local development build):\n"
        "         cd <project>/build && make anvild tongs-modbustcp -j$(nproc)\n"
        "         sudo ./deploy.sh anvild       # restarts anvild + tongs\n"
        "         # forgeiec-plc auto-rebuilds on next project deploy/restart\n"
        "       Compare anvil_wrapper_build_info() between processes to "
        "confirm.\n"
        "       (Suppressing further '%s' mismatch logs for 60 s.)\n",
        service_name, kind, kAnvilWrapperBuildInfo, service_name);
}

}  // namespace

int anvil_get_mismatch_topics(char *buf, size_t buflen) {
    std::lock_guard<std::mutex> lk(g_mismatch_log_mtx);
    if (buf && buflen > 0) {
        std::string out;
        out.reserve(g_mismatch_last_log.size() * 24);
        bool first = true;
        for (const auto &kv : g_mismatch_last_log) {
            if (!first) out += ',';
            first = false;
            out += kv.first;
        }
        const size_t n = std::min(out.size(), buflen - 1);
        std::memcpy(buf, out.data(), n);
        buf[n] = '\0';
    }
    return (int)g_mismatch_last_log.size();
}

#ifdef _anvil_src

/* ================================================================== */
/*  Real implementation using IceOryx2 C binding                       */
/* ================================================================== */

#include "iox2/iceoryx2.h"

/* --- Internal structures holding IceOryx2 handles --- */

struct anvil_node_s {
    iox2_node_h handle;
};

struct anvil_publisher_s {
    iox2_publisher_h   handle;
    size_t             payload_size;
};

struct anvil_subscriber_s {
    iox2_subscriber_h  handle;
    size_t             payload_size;
};

/* --- Dead-node cleanup (proactive + reactive) --- */

/*
 * Callback for iox2_node_list(): clean up dead nodes.
 * When a PLC program is recompiled with different HMI variables, the struct
 * size changes and stale services from the old process block open_or_create
 * with O_INCOMPATIBLE_TYPES (err=3). Cleaning dead nodes removes these.
 */
static int anvil_dead_nodes_cleaned = 0;

static enum iox2_callback_progression_e
anvil_cleanup_dead_node_cb(enum iox2_node_state_e state,
                           iox2_unique_node_id_ptr node_id,
                           const char * /*executable*/,
                           iox2_node_name_ptr /*node_name*/,
                           iox2_config_ptr config,
                           iox2_callback_context /*ctx*/)
{
    if (state == iox2_node_state_e_DEAD && node_id && config) {
        iox2_unique_node_id_h id_handle = NULL;
        iox2_config_h cfg_handle = NULL;

        /* Clone node ID and config into owning handles */
        iox2_unique_node_id_clone_from_ptr(NULL, node_id, &id_handle);
        iox2_config_from_ptr(config, NULL, &cfg_handle);

        if (id_handle && cfg_handle) {
            iox2_config_h_ref cfg_ref = &cfg_handle;
            /* iceoryx2 v0.9.0: NodeId -> UniqueNodeId rename (PR #1534);
             * iox2_dead_node_remove_stale_resources wurde umbenannt zu
             * _try_ Variante, success-out-Parameter ist weggefallen
             * (Return-Code IOX2_OK signalisiert Erfolg). */
            int ret = iox2_dead_node_try_remove_stale_resources(
                iox2_service_type_e_IPC,
                &id_handle, cfg_ref);
            if (ret == IOX2_OK) {
                anvil_dead_nodes_cleaned++;
                printf("Anvil: Cleaned stale resources from dead node\n");
            }
        }
        if (id_handle)  iox2_unique_node_id_drop(id_handle);
        if (cfg_handle) iox2_config_drop(cfg_handle);
    }
    return iox2_callback_progression_e_CONTINUE;
}

/*
 * Scan for dead iceoryx2 nodes and remove their stale services/shared memory.
 * This is critical after PLC recompilation when struct sizes change.
 */
static void anvil_cleanup_dead_nodes(void) {
    anvil_dead_nodes_cleaned = 0;
    iox2_config_ptr cfg = iox2_config_global_config();
    int ret = iox2_node_list(iox2_service_type_e_IPC, cfg,
                             anvil_cleanup_dead_node_cb, NULL);
    if (ret != IOX2_OK && anvil_debug >= 1)
        printf("Anvil: iox2_node_list failed (err=%d)\n", ret);
    if (anvil_dead_nodes_cleaned > 0)
        printf("Anvil: Cleaned %d dead node(s)\n", anvil_dead_nodes_cleaned);
}

/* --- Node --- */

anvil_node_t anvil_node_create(const char *name) {
    anvil_node_t n = (anvil_node_t)calloc(1, sizeof(struct anvil_node_s));
    if (!n) return NULL;

    /* Proactive: clean up stale services from previous PLC instances.
     * This prevents O_INCOMPATIBLE_TYPES when struct sizes changed. */
    anvil_cleanup_dead_nodes();

    iox2_node_builder_h builder = iox2_node_builder_new(NULL);
    if (!builder) {
        free(n);
        return NULL;
    }

    /* Set node name (optional, for debugging/discovery) */
    iox2_node_name_h node_name = NULL;
    if (name && name[0]) {
        iox2_node_name_new(NULL, name, strlen(name), &node_name);
        if (node_name)
            iox2_node_builder_set_name(&builder, iox2_cast_node_name_ptr(node_name));
    }

    int ret = iox2_node_builder_create(builder, NULL,
                                       iox2_service_type_e_IPC, &n->handle);
    if (node_name)
        iox2_node_name_drop(node_name);

    if (ret != IOX2_OK) {
        if (anvil_debug >= 1)
            printf("Anvil: iox2_node_builder_create failed (err=%d)\n", ret);
        free(n);
        return NULL;
    }

    return n;
}

void anvil_node_destroy(anvil_node_t node) {
    if (!node) return;
    if (node->handle)
        iox2_node_drop(node->handle);
    free(node);

    /* After dropping the node, clean up its now-stale services immediately.
     * Without this, the shared memory persists until the NEXT PLC start,
     * and a recompilation with different struct sizes would fail. */
    anvil_cleanup_dead_nodes();
}

/* --- Publisher --- */

/*
 * Internal: attempt to open_or_create a pub-sub service and create a publisher.
 * Returns IOX2_OK on success and fills p->handle, or an error code on failure.
 */
static int anvil_pub_try_create(anvil_node_t node, anvil_pub_t p,
                                const char *service_name, size_t payload_size) {
    /* Create service name */
    iox2_service_name_h svc_name = NULL;
    if (iox2_service_name_new(NULL, service_name, strlen(service_name),
                              &svc_name) != IOX2_OK) {
        if (anvil_debug >= 1)
            printf("Anvil: Failed to create service name '%s'\n", service_name);
        return -1;
    }

    /* Build publish-subscribe service */
    iox2_service_builder_h svc_builder =
        iox2_node_service_builder(&node->handle, NULL,
                                  iox2_cast_service_name_ptr(svc_name));
    iox2_service_name_drop(svc_name);

    if (!svc_builder) {
        if (anvil_debug >= 1)
            printf("Anvil: Failed to create service builder for '%s'\n", service_name);
        return -1;
    }

    iox2_service_builder_pub_sub_h ps_builder =
        iox2_service_builder_pub_sub(svc_builder);

    /* Set payload type details so IceOryx2 allocates enough shared memory.
     * Without this, the default payload size is 1 byte and send_copy fails
     * with LOAN_ERROR_EXCEEDS_MAX_LOAN_SIZE for larger payloads. */
    iox2_service_builder_pub_sub_set_payload_type_details(
        &ps_builder,
        iox2_type_variant_e_FIXED_SIZE,
        "Anvil", 5,
        payload_size, 1);

    if (anvil_debug >= 2)
        printf("Anvil: pub service '%s' payload_size=%zu\n", service_name, payload_size);

    iox2_port_factory_pub_sub_h pubsub = NULL;
    int ret = iox2_service_builder_pub_sub_open_or_create(
        ps_builder, NULL, &pubsub);
    if (ret != IOX2_OK || !pubsub)
        return ret;

    /* Create publisher */
    iox2_port_factory_publisher_builder_h pub_builder =
        iox2_port_factory_pub_sub_publisher_builder(&pubsub, NULL);

    ret = iox2_port_factory_publisher_builder_create(
        pub_builder, NULL, &p->handle);
    if (ret != IOX2_OK || !p->handle) {
        if (anvil_debug >= 1)
            printf("Anvil: Failed to create publisher for '%s' (err=%d)\n",
                   service_name, ret);
        return ret;
    }

    return IOX2_OK;
}

anvil_pub_t anvil_pub_create(anvil_node_t node, const char *service_name,
                         size_t payload_size) {
    if (!node || !node->handle || !service_name) return NULL;

    anvil_pub_t p = (anvil_pub_t)calloc(1, sizeof(struct anvil_publisher_s));
    if (!p) return NULL;
    p->payload_size = payload_size;

    int ret = anvil_pub_try_create(node, p, service_name, payload_size);
    if (ret == IOX2_OK)
        return p;

    /* O_INCOMPATIBLE_TYPES (3): stale service with different payload size.
     * Clean dead nodes and retry once. The diagnostic message is rate-
     * limited (once per topic per 60 s) and includes anvil_wrapper build
     * info so users can spot version skew at a glance. */
    const int O_INCOMPATIBLE_TYPES = (int)iox2_pub_sub_open_or_create_error_e_O_INCOMPATIBLE_TYPES;
    if (ret == O_INCOMPATIBLE_TYPES) {
        log_abi_mismatch("pub", service_name);
        anvil_cleanup_dead_nodes();

        p->handle = NULL;
        ret = anvil_pub_try_create(node, p, service_name, payload_size);
        if (ret == IOX2_OK)
            return p;
    }

    if (anvil_debug >= 1)
        printf("Anvil: Failed to open/create pub-sub service '%s' (err=%d)\n",
               service_name, ret);
    free(p);
    return NULL;
}

void anvil_pub_destroy(anvil_pub_t pub) {
    if (!pub) return;
    if (pub->handle)
        iox2_publisher_drop(pub->handle);
    free(pub);
}

static unsigned long anvil_pub_count = 0;
static unsigned long anvil_pub_ok = 0;
static unsigned long anvil_pub_fail = 0;
static unsigned long anvil_sub_count = 0;
static unsigned long anvil_sub_ok = 0;

int anvil_publish(anvil_pub_t pub, const void *data, size_t size) {
    if (!pub || !pub->handle || !data || size == 0) return -1;

    /* Use send_copy for simplicity - copies data into shared memory */
    size_t recipients = 0;
    int ret = iox2_publisher_send_copy(
        &pub->handle, data, size, &recipients);

    anvil_pub_count++;
    if (ret != IOX2_OK) {
        anvil_pub_fail++;
        if (anvil_debug >= 1 && (anvil_pub_fail <= 5 || anvil_pub_fail % 1000 == 0))
            printf("Anvil: publish failed (ret=%d, count=%lu)\n", ret, anvil_pub_fail);
        return -1;
    }
    anvil_pub_ok++;
    if (anvil_debug >= 2 && (anvil_pub_ok == 1 || anvil_pub_ok % 10000 == 0))
        printf("Anvil: publish ok (recipients=%zu, total=%lu)\n", recipients, anvil_pub_ok);
    return 0;
}

/* --- Subscriber --- */

/*
 * Internal: attempt to open_or_create a pub-sub service and create a subscriber.
 * Returns IOX2_OK on success and fills s->handle, or an error code on failure.
 */
static int anvil_sub_try_create(anvil_node_t node, anvil_sub_t s,
                                const char *service_name, size_t payload_size) {
    /* Create service name */
    iox2_service_name_h svc_name = NULL;
    if (iox2_service_name_new(NULL, service_name, strlen(service_name),
                              &svc_name) != IOX2_OK) {
        if (anvil_debug >= 1)
            printf("Anvil: Failed to create service name '%s'\n", service_name);
        return -1;
    }

    /* Build publish-subscribe service */
    iox2_service_builder_h svc_builder =
        iox2_node_service_builder(&node->handle, NULL,
                                  iox2_cast_service_name_ptr(svc_name));
    iox2_service_name_drop(svc_name);

    if (!svc_builder) {
        if (anvil_debug >= 1)
            printf("Anvil: Failed to create service builder for '%s'\n", service_name);
        return -1;
    }

    iox2_service_builder_pub_sub_h ps_builder =
        iox2_service_builder_pub_sub(svc_builder);

    /* Set payload type details to match the publisher's payload size */
    iox2_service_builder_pub_sub_set_payload_type_details(
        &ps_builder,
        iox2_type_variant_e_FIXED_SIZE,
        "Anvil", 5,
        payload_size, 1);

    if (anvil_debug >= 2)
        printf("Anvil: sub service '%s' payload_size=%zu\n", service_name, payload_size);

    iox2_port_factory_pub_sub_h pubsub = NULL;
    int ret = iox2_service_builder_pub_sub_open_or_create(
        ps_builder, NULL, &pubsub);
    if (ret != IOX2_OK || !pubsub)
        return ret;

    /* Create subscriber */
    iox2_port_factory_subscriber_builder_h sub_builder =
        iox2_port_factory_pub_sub_subscriber_builder(&pubsub, NULL);

    ret = iox2_port_factory_subscriber_builder_create(
        sub_builder, NULL, &s->handle);
    if (ret != IOX2_OK || !s->handle) {
        if (anvil_debug >= 1)
            printf("Anvil: Failed to create subscriber for '%s' (err=%d)\n",
                   service_name, ret);
        return ret;
    }

    return IOX2_OK;
}

anvil_sub_t anvil_sub_create(anvil_node_t node, const char *service_name,
                         size_t payload_size) {
    if (!node || !node->handle || !service_name) return NULL;

    anvil_sub_t s = (anvil_sub_t)calloc(1, sizeof(struct anvil_subscriber_s));
    if (!s) return NULL;
    s->payload_size = payload_size;

    int ret = anvil_sub_try_create(node, s, service_name, payload_size);
    if (ret == IOX2_OK)
        return s;

    /* O_INCOMPATIBLE_TYPES (3): stale service with different payload size.
     * Clean dead nodes and retry once. Diagnostic-on-mismatch goes through
     * log_abi_mismatch (rate-limited + carries build info). */
    const int O_INCOMPATIBLE_TYPES = (int)iox2_pub_sub_open_or_create_error_e_O_INCOMPATIBLE_TYPES;
    if (ret == O_INCOMPATIBLE_TYPES) {
        log_abi_mismatch("sub", service_name);
        anvil_cleanup_dead_nodes();

        s->handle = NULL;
        ret = anvil_sub_try_create(node, s, service_name, payload_size);
        if (ret == IOX2_OK)
            return s;
    }

    if (anvil_debug >= 1)
        printf("Anvil: Failed to open/create pub-sub service '%s' (err=%d)\n",
               service_name, ret);
    free(s);
    return NULL;
}

void anvil_sub_destroy(anvil_sub_t sub) {
    if (!sub) return;
    if (sub->handle)
        iox2_subscriber_drop(sub->handle);
    free(sub);
}

int anvil_receive(anvil_sub_t sub, void *buf, size_t size) {
    if (!sub || !sub->handle || !buf || size == 0) return -1;

    /* Receive the latest sample */
    iox2_sample_h sample = NULL;
    int ret = iox2_subscriber_receive(&sub->handle, NULL, &sample);

    anvil_sub_count++;
    if (ret != IOX2_OK) {
        if (anvil_debug >= 2 && (anvil_sub_count <= 5 || anvil_sub_count % 100000 == 0))
            printf("Anvil: receive ret=%d (not IOX2_OK=%d, calls=%lu)\n",
                   ret, IOX2_OK, anvil_sub_count);
        return -1;
    }
    if (!sample) {
        /* Queue empty - normal condition */
        return -1;
    }

    anvil_sub_ok++;
    if (anvil_debug >= 2 && (anvil_sub_ok == 1 || anvil_sub_ok % 10000 == 0))
        printf("Anvil: receive ok (total=%lu)\n", anvil_sub_ok);

    /* Copy payload into caller's buffer */
    const void *payload = NULL;
    size_t num_elements = 0;
    iox2_sample_payload(&sample, &payload, &num_elements);

    if (payload) {
        /* num_elements is the number of typed elements (always 1 for FIXED_SIZE),
         * NOT the byte count. Use the known payload_size from the subscriber. */
        size_t copy_len = (sub->payload_size < size) ? sub->payload_size : size;
        memcpy(buf, payload, copy_len);
    }

    iox2_sample_drop(sample);
    return 0;
}

#else /* !_anvil_src */

/* ================================================================== */
/*  Stub implementation (IceOryx2 not available)                       */
/* ================================================================== */

anvil_node_t anvil_node_create(const char *) { return NULL; }
void       anvil_node_destroy(anvil_node_t)  { }

anvil_pub_t  anvil_pub_create(anvil_node_t, const char *, size_t) { return NULL; }
void       anvil_pub_destroy(anvil_pub_t)    { }
int        anvil_publish(anvil_pub_t, const void *, size_t) { return -1; }

anvil_sub_t  anvil_sub_create(anvil_node_t, const char *, size_t) { return NULL; }
void       anvil_sub_destroy(anvil_sub_t)    { }
int        anvil_receive(anvil_sub_t, void *, size_t) { return -1; }

/* anvil_set_debug is defined above the #ifdef, so no stub needed */

#endif /* _anvil_src */
