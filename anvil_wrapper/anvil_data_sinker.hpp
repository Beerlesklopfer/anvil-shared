// Copyright 2025-2026 Blacksmith (blacksmith@forgeiec.io)
// Part of the Anvil IPC transport layer for ForgeIEC / OpenPLC v3.
// Licensed under the GNU General Public License v3.

#ifndef __ANVIL_DATA_SINKER_H_
#define __ANVIL_DATA_SINKER_H_

#ifdef __cplusplus

#include <atomic>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include "iox2/node.hpp"
#include "i_anvil_data_sink.h"

namespace anvil_wrapper {

template <typename Ht>
class AnvilDataSinker {
public:
  struct Settings {
    uint32_t cycletime_ms{0};              // required > 0
    uint32_t max_publishers{1};
    uint32_t max_subscribers{3};
    uint32_t subscriber_buffer_size{20};
    bool enable_safe_overflow{true};
  };

  // Every setting is provided via ctor: service_name, sink, and Settings.
  AnvilDataSinker(std::string service_name,
                i_anvil_data_sink<Ht>* sink,
                const Settings& settings)
      : service_name_(std::move(service_name))
      , sink_(sink)
      , settings_(settings)
      , stop_(false)
      , node_(create_node_or_die(service_name_))
      , service_(create_service_or_die(node_, service_name_, settings_))
      , subscriber_(create_subscriber_or_die(service_, service_name_))
      , subscriber_thread_(&AnvilDataSinker::run, this) {

    if (!sink_) {
      std::cerr << "AnvilDataSinker error: sink callback is null for service '"
                << service_name_ << "'\n";
    }

    if (settings_.cycletime_ms == 0) {
      std::cerr << "AnvilDataSinker error: cycletime_ms must be > 0 for service '"
                << service_name_ << "'\n";
    }
  }

  ~AnvilDataSinker() {
    stop_.store(true);
    if (subscriber_thread_.joinable()) {
      subscriber_thread_.join();
    }
  }

  AnvilDataSinker(const AnvilDataSinker&) = delete;
  AnvilDataSinker& operator=(const AnvilDataSinker&) = delete;

private:
  static iox2::Node<iox2::ServiceType::Ipc>
  create_node_or_die(const std::string& service_name) {
    auto node =
        iox2::NodeBuilder()
            .signal_handling_mode(iox2::SignalHandlingMode::Disabled)
            .create<iox2::ServiceType::Ipc>();

    if (node.has_error()) {
      std::cerr << "Error AnvilDataSinker: failed to create node ("
                << static_cast<uint32_t>(node.error())
                << ") on service '" << service_name << "'\n";
    }
    return std::move(node.expect("successful node creation"));
  }

  static iox2::PortFactoryPublishSubscribe<iox2::ServiceType::Ipc,
                                          iox::Slice<uint8_t>,
                                          Ht>
  create_service_or_die(iox2::Node<iox2::ServiceType::Ipc>& node,
                        const std::string& service_name,
                        const Settings& s) {
    auto service =
        node.service_builder(iox2::ServiceName::create(service_name.c_str())
                                 .expect("valid service name"))
            .publish_subscribe<iox::Slice<uint8_t>>()
            .template user_header<Ht>()
            .max_publishers(s.max_publishers)
            .max_subscribers(s.max_subscribers)
            .subscriber_max_buffer_size(s.subscriber_buffer_size)
            .enable_safe_overflow(s.enable_safe_overflow)
            .open_or_create();

    if (service.has_error()) {
      std::cerr << "Error AnvilDataSinker: failed to create/open service ("
                << static_cast<uint32_t>(service.error())
                << ") on '" << service_name << "'\n";
    }
    return std::move(service.expect("successful service creation"));
  }

  static iox2::Subscriber<iox2::ServiceType::Ipc, iox::Slice<uint8_t>, Ht>
  create_subscriber_or_die(
      iox2::PortFactoryPublishSubscribe<iox2::ServiceType::Ipc,
                                        iox::Slice<uint8_t>,
                                        Ht>& service,
      const std::string& service_name) {
    auto subscriber = service.subscriber_builder().create();
    if (subscriber.has_error()) {
      std::cerr << "Error AnvilDataSinker: failed to create subscriber ("
                << static_cast<uint32_t>(subscriber.error())
                << ") on '" << service_name << "'\n";
    }
    return std::move(subscriber.expect("successful service subscriber"));
  }

  void run() {
    while (!stop_.load()) {
      // Wait up to cycletime for wakeup.
      const auto woke =
          node_.wait(iox::units::Duration::fromMilliseconds(settings_.cycletime_ms));

      if (!woke.has_value()) {
        // timeout -> nothing to do
        continue;
      }

      // If there are samples, drain them.
      auto hs = subscriber_.has_samples();
      if (!hs.has_value()) {
        continue;
      }

      auto sample_ret = subscriber_.receive();
      if (sample_ret.has_error()) {
        std::cerr << "AnvilDataSinker: receive() failed on '" << service_name_
                  << "'\n";
        continue;
      }

      auto sample = std::move(sample_ret.value());
      bool any = false;

      while (sample.has_value()) {
        any = true;

        auto payload = sample->payload();
        const void* data = payload.data();
        const uint32_t n_data = payload.number_of_bytes();
        auto header = sample->user_header();

        if (sink_) {
          sink_->OnDataReceived(&header, data, n_data);
        }

        auto next = subscriber_.receive();
        if (next.has_error()) {
          std::cerr << "AnvilDataSinker: receive() failed while draining on '"
                    << service_name_ << "'\n";
          break;
        }
        sample = std::move(next.value());
      }

      if (any && sink_) {
        sink_->OnDataReady();
      }
    }
  }

private:
  const std::string service_name_;
  i_anvil_data_sink<Ht>* const sink_;
  const Settings settings_;

  std::atomic<bool> stop_;

  iox2::Node<iox2::ServiceType::Ipc> node_;
  iox2::PortFactoryPublishSubscribe<iox2::ServiceType::Ipc,
                                    iox::Slice<uint8_t>,
                                    Ht>
      service_;
  iox2::Subscriber<iox2::ServiceType::Ipc, iox::Slice<uint8_t>, Ht> subscriber_;

  std::thread subscriber_thread_;
};

} // namespace anvil_wrapper

#endif // __cplusplus
#endif // __ANVIL_DATA_SINKER_H_
