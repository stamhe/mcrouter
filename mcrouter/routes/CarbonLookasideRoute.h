/*
 *  Copyright (c) 2018-present, Facebook, Inc.
 *
 *  This source code is licensed under the MIT license found in the LICENSE
 *  file in the root directory of this source tree.
 *
 */
#pragma once

#include <string>
#include <utility>

#include <folly/Conv.h>
#include <folly/Range.h>
#include <folly/io/IOBuf.h>

#include "mcrouter/CarbonRouterFactory.h"
#include "mcrouter/CarbonRouterInstance.h"
#include "mcrouter/McrouterFiberContext.h"
#include "mcrouter/config.h"
#include "mcrouter/lib/Operation.h"
#include "mcrouter/lib/RouteHandleTraverser.h"
#include "mcrouter/lib/fbi/cpp/globals.h"
#include "mcrouter/lib/routes/NullRoute.h"
#include "mcrouter/routes/McRouteHandleBuilder.h"

namespace facebook {
namespace memcache {
namespace mcrouter {

struct LeaseSettings {
  bool enableLeases{false}; // Enable or disable leases.
  int32_t initialWaitMs{2}; // Initial wait time of a retry on hot miss.
  int32_t maxWaitMs{500}; // Maximum wait time of a retry on hot miss.
  int32_t numRetries{10}; // Number of retries on a hot miss.
};

std::shared_ptr<CarbonRouterInstance<MemcacheRouterInfo>>
createCarbonLookasideRouter(
    const std::string& persistenceId,
    folly::StringPiece flavorUri,
    std::unordered_map<std::string, std::string> optionOverrides =
        std::unordered_map<std::string, std::string>());

LeaseSettings parseLeaseSettings(const folly::dynamic& json);

/**
 * CarbonLookasideRoute is a route handle that can store replies in memcache
 * with a user defined key. The user controls which replies should be cached.
 * Replies found in memcache will be returned directly without having to
 * traverse further into the routing tree.
 *
 * This behavior is controlled through a user defined class with the following
 * prototype:
 *
 * class CarbonLookasideHelper {
 *    CarbonLookasideHelper(const folly::dynamic* config);
 *
 *    std::string name();
 *
 *    template <typename Request>
 *    bool cacheCandidate(const Request& req);
 *
 *   template <typename Request>
 *   std::string buildKey(const Request& req);
 * };
 *
 * @tparam RouterInfo            The Router
 * @tparam CarbonLookasideHelper User defined class with helper functions
 */
template <class RouterInfo, class CarbonLookasideHelper>
class CarbonLookasideRoute {
 private:
  using RouteHandleIf = typename RouterInfo::RouteHandleIf;
  using RouteHandlePtr = typename RouterInfo::RouteHandlePtr;

 public:
  std::string routeName() const {
    return folly::sformat(
        "lookaside-cache|name={}|ttl={}s|leases={}",
        carbonLookasideHelper_.name(),
        ttl_,
        leaseSettings_.enableLeases ? "true" : "false");
  }

  /**
   * Constructs CarbonLookasideRoute.
   *
   * @param child         The child route handle
   * @param router        A shared_ptr to the CarbonRouterInstance used to
   *                      communicate with memcache. Note: this share_ptr is
   *                      here just to keep the router alive.
   * @param client        The CarbonRouterClient used to communicate with
   *                      memcache.
   * @param prefix        Prefix prepend to memcache keys generated by the
   *                      helper.
   * @param keySplitSize  Tells how many different keys we want to have for
   *                      the same request. Useful for dealing with hot keys.
   * @param ttl           TTL of items stored in memcache by this route handle,
   *                      in seconds.
   * @param helper        The helper used to build keys and see if we should
   *                      cache a given request. This helper is use-case
   *                      specific.
   * @param leaseSettings The lease settings for memcache leases.
   */
  CarbonLookasideRoute(
      RouteHandlePtr child,
      std::shared_ptr<CarbonRouterInstance<MemcacheRouterInfo>> router,
      CarbonRouterClient<MemcacheRouterInfo>::Pointer client,
      std::string prefix,
      size_t keySplitSize,
      int32_t ttl,
      CarbonLookasideHelper helper,
      LeaseSettings leaseSettings)
      : child_(std::move(child)),
        router_(std::move(router)),
        client_(std::move(client)),
        keyPrefix_(std::move(prefix)),
        keySuffix_(buildKeySuffix(keySplitSize)),
        ttl_(ttl),
        carbonLookasideHelper_(std::move(helper)),
        leaseSettings_(std::move(leaseSettings)) {
    assert(router_);
    assert(client_);
  }

  template <class Request>
  void traverse(
      const Request& req,
      const RouteHandleTraverser<RouteHandleIf>& t) const {
    t(*child_, req);
  }

  template <class Request>
  ReplyT<Request> route(const Request& req) {
    int64_t leaseToken = 0;
    std::string key;
    bool cacheCandidate = carbonLookasideHelper_.cacheCandidate(req);
    if (cacheCandidate) {
      key = buildKey(req);
      if (auto optReply = carbonLookasideGet<Request>(key, leaseToken)) {
        return optReply.value();
      }
    }

    auto reply = child_->route(req);

    if (cacheCandidate) {
      carbonLookasideSet(key, reply, leaseToken);
    }
    return reply;
  }

 private:
  const RouteHandlePtr child_;
  const std::shared_ptr<CarbonRouterInstance<MemcacheRouterInfo>> router_;
  const CarbonRouterClient<MemcacheRouterInfo>::Pointer client_;
  const std::string keyPrefix_;
  const std::string keySuffix_;
  const int32_t ttl_;
  CarbonLookasideHelper carbonLookasideHelper_;
  const LeaseSettings leaseSettings_;

  template <typename Request>
  folly::Optional<ReplyT<Request>> carbonLookasideGet(
      folly::StringPiece key,
      int64_t& leaseToken) {
    if (leaseSettings_.enableLeases) {
      return carbonLookasideLeaseGet<Request>(key, leaseToken);
    }
    return carbonLookasideGet<Request>(key);
  }

  // Build a request to CarbonLookaside to query for key. Successful replies
  // are deserialized.
  template <typename Request>
  folly::Optional<ReplyT<Request>> carbonLookasideGet(folly::StringPiece key) {
    McGetRequest cacheRequest(key);
    folly::Optional<ReplyT<Request>> ret;
    folly::fibers::Baton baton;
    client_->send(
        cacheRequest,
        [&baton, &ret](const McGetRequest&, McGetReply&& cacheReply) {
          if (isHitResult(cacheReply.result()) &&
              cacheReply.value().hasValue()) {
            folly::io::Cursor cur(cacheReply.value().get_pointer());
            carbon::CarbonProtocolReader reader(cur);
            ReplyT<Request> reply;
            reply.deserialize(reader);
            ret.assign(std::move(reply));
          }
          baton.post();
        });
    baton.wait();
    return ret;
  }

  // Build a request using leases to CarbonLookaside to query for key.
  // Successful replies are deserialized.
  template <typename Request>
  folly::Optional<ReplyT<Request>> carbonLookasideLeaseGet(
      folly::StringPiece key,
      int64_t& leaseToken) {
    leaseToken = 0;
    McLeaseGetRequest cacheRequest(key);
    folly::Optional<ReplyT<Request>> ret;
    auto nextInterval = leaseSettings_.initialWaitMs;
    for (int32_t attempt = 0; attempt <= leaseSettings_.numRetries; ++attempt) {
      folly::fibers::Baton sleepBaton;
      if (attempt != 0) {
        sleepBaton.try_wait_for(std::chrono::milliseconds(nextInterval));
        nextInterval = std::min(nextInterval * 2, leaseSettings_.maxWaitMs);
      }
      folly::fibers::Baton baton;
      bool retry = false;
      client_->send(
          cacheRequest,
          [&baton, &ret, &retry, &leaseToken](
              const McLeaseGetRequest&, McLeaseGetReply&& cacheReply) {
            retry = false;
            if (isHitResult(cacheReply.result()) &&
                cacheReply.value().hasValue()) {
              folly::io::Cursor cur(cacheReply.value().get_pointer());
              carbon::CarbonProtocolReader reader(cur);
              ReplyT<Request> reply;
              reply.deserialize(reader);
              ret.assign(std::move(reply));
            } else if (isMissResult(cacheReply.result())) {
              // Hot miss will retry using an expoential backoff.
              // A miss will return with the lease token set.
              constexpr size_t kLeaseHotMissToken = 1;
              if (cacheReply.leaseToken() == kLeaseHotMissToken) {
                retry = true;
              } else {
                leaseToken = cacheReply.leaseToken();
              }
            }
            baton.post();
          });
      baton.wait();
      if (!retry) {
        return ret;
      }
    }
    return ret;
  }

  template <typename Reply>
  folly::IOBuf serializeOffFiber(const Reply& reply) const {
    return folly::fibers::runInMainContext([&reply]() {
      carbon::CarbonQueueAppenderStorage storage;
      carbon::CarbonProtocolWriter writer(storage);
      reply.serialize(writer);
      folly::IOBuf body(folly::IOBuf::CREATE, storage.computeBodySize());
      const auto iovs = storage.getIovecs();
      for (size_t i = 0; i < iovs.second; ++i) {
        const struct iovec* iov = iovs.first + i;
        std::memcpy(body.writableTail(), iov->iov_base, iov->iov_len);
        body.append(iov->iov_len);
      }
      return body;
    });
  }

  template <typename Reply>
  void carbonLookasideSet(
      folly::StringPiece key,
      const Reply& reply,
      int64_t leaseToken) {
    if (leaseSettings_.enableLeases && leaseToken) {
      return carbonLookasideLeaseSet(key, reply, leaseToken);
    }
    return carbonLookasideSet(key, reply);
  }

  // Build a request to memcache to store the serialized reply with the
  // provided key.
  template <typename Reply>
  void carbonLookasideSet(folly::StringPiece key, const Reply& reply) {
    McSetRequest req(key);
    req.exptime() = ttl_;
    req.value() = serializeOffFiber(reply);
    folly::fibers::addTask([this, req = std::move(req)]() {
      folly::fibers::Baton baton;
      client_->send(
          req, [&baton](const McSetRequest&, McSetReply&&) { baton.post(); });
      baton.wait();
    });
  }

  // Build a request using leases to memcache to store the serialized reply
  // with the provided key.
  template <typename Reply>
  void carbonLookasideLeaseSet(
      folly::StringPiece key,
      const Reply& reply,
      const int64_t leaseToken) {
    McLeaseSetRequest req(key);
    req.exptime() = ttl_;
    req.leaseToken() = leaseToken;
    req.value() = serializeOffFiber(reply);
    folly::fibers::addTask([this, req = std::move(req)]() {
      folly::fibers::Baton baton;
      client_->send(req, [&baton](const McLeaseSetRequest&, McLeaseSetReply&&) {
        baton.post();
      });
      baton.wait();
    });
  }

  template <typename Request>
  std::string buildKey(const Request& req) {
    return folly::to<std::string>(
        keyPrefix_, carbonLookasideHelper_.buildKey(req), keySuffix_);
  }

  static std::string buildKeySuffix(size_t keySplitSize) {
    if (keySplitSize <= 1) {
      return "";
    }
    return folly::to<std::string>(":ks", globals::hostid() % keySplitSize);
  }
};

/**
 * Creates a carbon lookaside route-handle.
 *
 * Sample json format:
 * {
 *   "child": "PoolRoute|A",
 *   "ttl": 10, // 10 seconds
 *   "key_split_size": 3, // we will have 3 different keys for the same request
 *   "prefix": "reg",
 *   "flavor": "web",
 *   "helper_config": {
 *     // configs specific to the helper class.
 *   }
 * }
 */
template <class RouterInfo, class CarbonLookasideHelper>
typename RouterInfo::RouteHandlePtr createCarbonLookasideRoute(
    RouteHandleFactory<typename RouterInfo::RouteHandleIf>& factory,
    const folly::dynamic& json) {
  checkLogic(json.isObject(), "CarbonLookasideRoute is not an object");

  auto jChild = json.get_ptr("child");
  checkLogic(
      jChild != nullptr, "CarbonLookasideRoute: 'child' property is missing");

  auto child = factory.create(*jChild);
  checkLogic(
      child != nullptr,
      "CarbonLookasideRoute: cannot create route handle from 'child'");

  auto jTtl = json.get_ptr("ttl");
  checkLogic(
      jTtl != nullptr, "CarbonLookasideRoute: 'ttl' property is missing");
  checkLogic(jTtl->isInt(), "CarbonLookasideRoute: 'ttl' is not an integer");
  int32_t ttl = jTtl->getInt();

  std::string prefix = ""; // Defaults to no prefix.
  if (auto jPrefix = json.get_ptr("prefix")) {
    checkLogic(
        jPrefix->isString(), "CarbonLookasideRoute: 'prefix' is not a string");
    prefix = jPrefix->getString();
  }

  std::string flavor = "web"; // Defaults to web flavor.
  if (auto jFlavor = json.get_ptr("flavor")) {
    checkLogic(
        jFlavor->isString(), "CarbonLookasideRoute: 'flavor' is not a string");
    flavor = jFlavor->getString();
  }

  size_t keySplitSize = 1;
  if (auto jKeySplitSize = json.get_ptr("key_split_size")) {
    checkLogic(
        jKeySplitSize->isInt() && jKeySplitSize->getInt() > 0,
        "CarbonLookasideRoute: 'key_split_size' must be a positive integer");
    keySplitSize = jKeySplitSize->getInt();
  }

  LeaseSettings leaseSettings = parseLeaseSettings(json);

  auto helperConfig = json.get_ptr("helper_config");
  if (helperConfig) {
    checkLogic(
        helperConfig->isObject(),
        "CarbonLookasideRoute: 'helper_config' is not an object");
  }
  CarbonLookasideHelper helper(helperConfig);

  // Creates a McRouter client to communicate with memcache using the
  // specified flavor information. The route handle owns the router resource
  // via a shared_ptr. The router will survive reconfigurations given that
  // at least one route handle will maintain a reference to it at any one time.
  // It will be cleaned up automatically whenever the last route handle using it
  // is removed.
  auto persistenceId = folly::to<std::string>("CarbonLookasideClient:", flavor);
  auto router = createCarbonLookasideRouter(persistenceId, flavor);
  if (!router) {
    LOG(ERROR) << "Failed to create router from flavor '" << flavor
               << "' for CarbonLookasideRouter.";
    return std::move(child);
  }

  CarbonRouterClient<MemcacheRouterInfo>::Pointer client{nullptr};
  try {
    client = router->createClient(0 /* max_outstanding_requests */);
  } catch (const std::runtime_error& e) {
    LOG(ERROR)
        << "Failed to create client for CarbonLookasideRouter. Exception: "
        << e.what();
    return std::move(child);
  }
  return makeRouteHandleWithInfo<
      RouterInfo,
      CarbonLookasideRoute,
      CarbonLookasideHelper>(
      std::move(child),
      std::move(router),
      std::move(client),
      std::move(prefix),
      keySplitSize,
      ttl,
      std::move(helper),
      std::move(leaseSettings));
}

} // namespace mcrouter
} // namespace memcache
} // namespace facebook