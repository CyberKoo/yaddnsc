//
// Created by Kotarou on 2022/4/5.
//

#ifndef YADDNSC_NETWORK_HTTPCLIENT_H
#define YADDNSC_NETWORK_HTTPCLIENT_H

#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "interface/http_client.h"

#include "address_family.h"
#include "uri.h"

namespace httplib
{
class Client;
}

// ---------------------------------------------------------------------------
// HttpClientOptions — all knobs exposed by TransientHttpClient / PersistentHttpClient.
//
// Every field is optional; unset fields fall back to a sensible default.
// ---------------------------------------------------------------------------
struct HttpClientOptions
{
  std::optional<AddressFamily> address_family{};
  std::optional<std::string> interface{};
  std::optional<std::string> ca_cert_path{};
  std::optional<bool> verify_server_cert{};
  std::optional<std::chrono::seconds> connection_timeout{};
  std::optional<std::chrono::seconds> read_timeout{};
  std::optional<std::chrono::seconds> write_timeout{};
  std::optional<bool> follow_location{};
  std::optional<bool> keep_alive{};
  std::optional<std::multimap<std::string, std::string>> default_headers{};
};

// ---------------------------------------------------------------------------
// TransientHttpClient — HttpClient that creates a new httplib::Client
// on every send() call.
//
// Construct with HttpClientOptions (all fields optional); use
// send() to issue arbitrary requests.
// --------------------------------------------------------------------------
class TransientHttpClient final : public HttpClient
{
public:
  explicit TransientHttpClient(HttpClientOptions opts = {});

  ~TransientHttpClient() override = default;

  [[nodiscard]] HttpResult exchange(std::string_view url, const HttpRequest& req) const override;

private:
  HttpClientOptions opts_;
};

// ---------------------------------------------------------------------------
// PersistentHttpClient — HttpClient that owns a single httplib::Client
// instance for the lifetime of the object.
//
// Construct with a Uri and HttpClientOptions; the underlying httplib::Client
// is built once in the constructor and reused across all send() calls.
// This is more efficient than TransientHttpClient when making multiple
// requests to the same host.
//
// Not thread-safe: a single PersistentHttpClient must not be used from
// multiple threads simultaneously.
// --------------------------------------------------------------------------
class PersistentHttpClient final : public HttpClient
{
public:
  explicit PersistentHttpClient(const Uri& uri, const HttpClientOptions& opts = {});

  ~PersistentHttpClient() override;

  [[nodiscard]] HttpResult exchange(std::string_view url, const HttpRequest& req) const override;

private:
  Uri uri_;
  std::unique_ptr<httplib::Client> client_;
};

#endif  // YADDNSC_NETWORK_HTTPCLIENT_H
