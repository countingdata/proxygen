/*
 *  Copyright (c) 2019-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#pragma once

#include <folly/io/IOBufQueue.h>
#include <folly/io/async/AsyncSocket.h>
#include <folly/io/async/DelayedDestructionBase.h>
#include <folly/io/async/EventBase.h>
#include <proxygen/lib/http/codec/HQControlCodec.h>
#include <proxygen/lib/http/codec/HQStreamCodec.h>
#include <proxygen/lib/http/codec/HQUnidirectionalCodec.h>
#include <proxygen/lib/http/codec/HQUtils.h>
#include <proxygen/lib/http/codec/HTTP1xCodec.h>
#include <proxygen/lib/http/codec/HTTPChecks.h>
#include <proxygen/lib/http/codec/HTTPCodec.h>
#include <proxygen/lib/http/codec/HTTPCodecFilter.h>
#include <proxygen/lib/http/codec/HTTPSettings.h>
#include <proxygen/lib/http/codec/QPACKDecoderCodec.h>
#include <proxygen/lib/http/codec/QPACKEncoderCodec.h>
#include <proxygen/lib/http/session/ByteEventTracker.h>
#include <proxygen/lib/http/session/HQStreamBase.h>
#include <proxygen/lib/http/session/HQStreamLookup.h>
#include <proxygen/lib/http/session/HQUnidirectionalCallbacks.h>
#include <proxygen/lib/http/session/HTTPSessionBase.h>
#include <proxygen/lib/http/session/HTTPSessionController.h>
#include <proxygen/lib/http/session/HTTPTransaction.h>
#include <proxygen/lib/http/session/ServerPushLifecycle.h>
#include <proxygen/lib/utils/ConditionalGate.h>
#include <quic/api/QuicSocket.h>
#include <quic/logging/QuicLogger.h>

namespace proxygen {

class HTTPSessionController;
class HQSession;
class VersionUtils;

std::ostream& operator<<(std::ostream& os, const HQSession& session);

enum class HQVersion : uint8_t {
  H1Q_FB_V1, // HTTP1.1 on each stream, no control stream
  H1Q_FB_V2, // HTTP1.1 on each stream, control stream for GOAWAY
  HQ,        // The real McCoy
};

extern const std::string kH3FBCurrentDraft;
extern const std::string kH3CurrentDraft;
extern const std::string kHQCurrentDraft;

// Default Priority Node
extern const proxygen::http2::PriorityUpdate hqDefaultPriority;

using HQVersionType = std::underlying_type<HQVersion>::type;

/**
 * Session-level protocol info.
 */
struct QuicProtocolInfo : public wangle::ProtocolInfo {
  virtual ~QuicProtocolInfo() override = default;

  folly::Optional<quic::ConnectionId> clientConnectionId;
  folly::Optional<quic::ConnectionId> serverConnectionId;
  folly::Optional<quic::TransportSettings> transportSettings;

  uint32_t ptoCount{0};
  uint32_t totalPTOCount{0};
  uint64_t totalTransportBytesSent{0};
  uint64_t totalTransportBytesRecvd{0};
};

/**
 *  Stream level protocol info. Contains all data from
 *  the sessinon info, plus stream-specific information.
 *  This structure is owned by each individual stream,
 *  and is updated when requested.
 *  If instance of HQ Transport Stream outlives the corresponding QUIC socket,
 *  has been destroyed, this structure will contain the last snapshot
 *  of the data received from the QUIC socket.
 *
 * Usage:
 *   TransportInfo tinfo;
 *   txn.getCurrentTransportInfo(&tinfo); // txn is the HTTP transaction object
 *   auto streamInfo = dynamic_cast<QuicStreamProtocolInfo>(tinfo.protocolInfo);
 *   if (streamInfo) {
 *      // stream level AND connection level info is available
 *   };
 *   auto connectionInfo = dynamic_cast<QuicProtocolInfo>(tinfo.protocolInfo);
 *   if (connectionInfo) {
 *     // ONLY connection level info is available. No stream level info.
 *   }
 *
 */
struct QuicStreamProtocolInfo : public QuicProtocolInfo {

  // Slicing assignment operator to initialize the per-stream protocol info
  // with the values of the per-session protocol info.
  QuicStreamProtocolInfo& operator=(const QuicProtocolInfo& other) {
    if (this != &other) {
      *(static_cast<QuicProtocolInfo*>(this)) = other;
    }
    return *this;
  }

  quic::QuicSocket::StreamTransportInfo streamTransportInfo;
  // NOTE: when the control stream latency stats will be reintroduced,
  // collect it here.
};

class HQSession
    : public quic::QuicSocket::ConnectionCallback
    , public quic::QuicSocket::ReadCallback
    , public quic::QuicSocket::WriteCallback
    , public quic::QuicSocket::DeliveryCallback
    , public HTTPSessionBase
    , public folly::EventBase::LoopCallback
    , public HQUnidirStreamDispatcher::Callback {

  // Forward declarations
 protected:
  class HQStreamTransport;
  class HQEgressPushStream;
  class HQIngressPushStream;
  class HQStreamTransportBase;

 private:
  class HQControlStream;
  class H1QFBV1VersionUtils;
  class H1QFBV2VersionUtils;
  class HQVersionUtils;

  static constexpr uint8_t kMaxCodecStackDepth = 3;

 public:
  void setServerPushLifecycleCallback(ServerPushLifecycleCallback* cb) {
    serverPushLifecycleCb_ = cb;
  }

  class ConnectCallback {
   public:
    virtual ~ConnectCallback() {
    }

    /**
     * This function is not terminal of the callback, downstream should expect
     * onReplaySafe to be invoked after connectSuccess.
     * onReplaySafe is invoked right after connectSuccess if zero rtt is not
     * attempted.
     * In zero rtt case, onReplaySafe might never be invoked if e.g. server
     * does not respond.
     */
    virtual void connectSuccess() {
      // Default empty implementation is provided in case downstream does not
      // attempt zero rtt data.
    }

    /**
     * Terminal callback.
     */
    virtual void onReplaySafe() = 0;

    /**
     * Terminal callback.
     */
    virtual void connectError(
        std::pair<quic::QuicErrorCode, std::string> code) = 0;
  };

  virtual ~HQSession();

  HTTPSessionBase::SessionType getType() const noexcept override {
    return HTTPSessionBase::SessionType::HQ;
  }

  void setSocket(std::shared_ptr<quic::QuicSocket> sock) noexcept {
    sock_ = sock;
    if (infoCallback_) {
      infoCallback_->onCreate(*this);
    }

    if (quicInfo_) {
      quicInfo_->transportSettings = sock_->getTransportSettings();
    }
  }

  void setForceUpstream1_1(bool force) {
    forceUpstream1_1_ = force;
  }

  void setSessionStats(HTTPSessionStats* stats) override;

  void onNewBidirectionalStream(quic::StreamId id) noexcept override;

  void onNewUnidirectionalStream(quic::StreamId id) noexcept override;

  void onStopSending(quic::StreamId id,
                     quic::ApplicationErrorCode error) noexcept override;

  void onConnectionEnd() noexcept override {
    VLOG(4) << __func__ << " sess=" << *this;
    // The transport will not call onConnectionEnd after we call close(),
    // so there is no need for us here to handle re-entrancy
    // checkForShutdown->close->onConnectionEnd.
    drainState_ = DrainState::DONE;
    closeWhenIdle();
  }

  void onConnectionError(
      std::pair<quic::QuicErrorCode, std::string> code) noexcept override;

  // returns false in case of failure
  bool onTransportReadyCommon() noexcept;

  void onReplaySafe() noexcept override;

  void onFlowControlUpdate(quic::StreamId id) noexcept override;

  // quic::QuicSocket::ReadCallback
  void readAvailable(quic::StreamId id) noexcept override;

  void readError(
      quic::StreamId id,
      std::pair<quic::QuicErrorCode, folly::Optional<folly::StringPiece>>
          error) noexcept override;

  // quic::QuicSocket::WriteCallback
  void onConnectionWriteReady(uint64_t maxToSend) noexcept override;

  void onConnectionWriteError(
      std::pair<quic::QuicErrorCode, folly::Optional<folly::StringPiece>>
          error) noexcept override;

  // Only for UpstreamSession
  HTTPTransaction* newTransaction(HTTPTransaction::Handler* handler) override;

  void startNow() override;

  void describe(std::ostream& os) const override {
    using quic::operator<<;
    os << "proto=" << alpn_;
    auto clientCid = (sock_ && sock_->getClientConnectionId())
                         ? *sock_->getClientConnectionId()
                         : quic::ConnectionId({0, 0, 0, 0});
    auto serverCid = (sock_ && sock_->getServerConnectionId())
                         ? *sock_->getServerConnectionId()
                         : quic::ConnectionId({0, 0, 0, 0});
    if (direction_ == TransportDirection::DOWNSTREAM) {
      os << ", client CID=" << clientCid << ", server CID=" << serverCid
         << ", downstream=" << getPeerAddress() << ", " << getLocalAddress()
         << "=local";
    } else {
      os << ", client CID=" << clientCid << ", server CID=" << serverCid
         << ", local=" << getLocalAddress() << ", " << getPeerAddress()
         << "=upstream";
    }
  }

  void onGoaway(uint64_t lastGoodStreamID,
                ErrorCode code,
                std::unique_ptr<folly::IOBuf> debugData = nullptr);

  void onSettings(const SettingsList& settings);

  folly::AsyncTransportWrapper* getTransport() override {
    return nullptr;
  }

  folly::EventBase* getEventBase() const override {
    if (sock_) {
      return sock_->getEventBase();
    }
    return nullptr;
  }

  const folly::AsyncTransportWrapper* getTransport() const override {
    return nullptr;
  }

  bool hasActiveTransactions() const override {
    return numberOfStreams() > 0;
  }

  uint32_t getNumOutgoingStreams() const override {
    // need transport API
    return static_cast<uint32_t>((direction_ == TransportDirection::DOWNSTREAM)
                                     ? numberOfEgressPushStreams()
                                     : numberOfEgressStreams());
  }

  uint32_t getNumIncomingStreams() const override {
    // need transport API
    return static_cast<uint32_t>((direction_ == TransportDirection::UPSTREAM)
                                     ? numberOfIngressPushStreams()
                                     : numberOfIngressStreams());
  }

  CodecProtocol getCodecProtocol() const override {
    if (!versionUtils_) {
      // return a default protocol before alpn is set
      return CodecProtocol::HTTP_1_1;
    }
    return versionUtils_->getCodecProtocol();
  }

  // for testing only
  HQUnidirStreamDispatcher* getDispatcher() {
    return &unidirectionalReadDispatcher_;
  }

  /**
   * Set flow control properties on an already started session.
   * QUIC requires both stream and connection flow control window sizes to be
   * specified in the initial transport handshake. Specifying
   * SETTINGS_INITIAL_WINDOW_SIZE in the SETTINGS frame is an error.
   *
   * @param initialReceiveWindow      (unused)
   * @param receiveStreamWindowSize   per-stream receive window for NEW streams;
   * @param receiveSessionWindowSize  per-session receive window;
   */
  void setFlowControl(size_t /* initialReceiveWindow */,
                      size_t receiveStreamWindowSize,
                      size_t receiveSessionWindowSize) override {
    if (sock_) {
      sock_->setConnectionFlowControlWindow(receiveSessionWindowSize);
    }
    receiveStreamWindowSize_ = (uint32_t)receiveStreamWindowSize;
  }

  /**
   * Set outgoing settings for this session
   */
  void setEgressSettings(const SettingsList& settings) override {
    for (const auto& setting : settings) {
      egressSettings_.setSetting(setting.id, setting.value);
    }
    const auto maxHeaderListSize =
        egressSettings_.getSetting(SettingsId::MAX_HEADER_LIST_SIZE);
    if (maxHeaderListSize) {
      versionUtilsReady_.then([this, size = maxHeaderListSize->value] {
        versionUtils_->setMaxUncompressed(size);
      });
    }
  }

  void setMaxConcurrentIncomingStreams(uint32_t /*num*/) override {
    // need transport API
  }

  /**
   * Send a settings frame
   */
  size_t sendSettings() override;

  /**
   * Causes a ping to be sent on the session. If the underlying protocol
   * doesn't support pings, this will return 0. Otherwise, it will return
   * the number of bytes written on the transport to send the ping.
   */
  size_t sendPing() override {
    sock_->sendPing(nullptr, std::chrono::milliseconds(0));
    return 0;
  }

  /**
   * Sends a priority message on this session.  If the underlying protocol
   * doesn't support priority, this is a no-op.  A new stream identifier will
   * be selected and returned.
   */
  HTTPCodec::StreamID sendPriority(http2::PriorityUpdate /*pri*/) override {
    return 0;
  }

  /**
   * As above, but updates an existing priority node.  Do not use for
   * real nodes, prefer HTTPTransaction::changePriority.
   */
  size_t sendPriority(HTTPCodec::StreamID /*id*/,
                      http2::PriorityUpdate /*pri*/) override {
    return 0;
  }

  /**
   * Get session-level transport info.
   * NOTE: The protocolInfo will be set to connection-level pointer.
   */
  bool getCurrentTransportInfo(wangle::TransportInfo* /*tinfo*/) override;

  /**
   *  Get session level AND stream level transport info.
   *  NOTE: the protocolInfo will be set to stream-level pointer.
   */
  bool getCurrentStreamTransportInfo(QuicStreamProtocolInfo* /*qspinfo*/,
                                     quic::StreamId /*streamId*/);

  bool connCloseByRemote() override {
    return false;
  }

  // From ManagedConnection
  void timeoutExpired() noexcept override;

  bool isBusy() const override {
    return numberOfStreams() > 0;
  }
  void notifyPendingShutdown() override;
  void closeWhenIdle() override;
  void dropConnection() override;
  void dumpConnectionState(uint8_t /*loglevel*/) override {
  }

  /* errorCode is passed to transport CLOSE_CONNNECTION frame
   *
   * proxygenError is delivered to open transactions
   */
  void dropConnectionWithError(
      std::pair<quic::QuicErrorCode, std::string> errorCode,
      ProxygenError proxygenError);

  bool getCurrentTransportInfoWithoutUpdate(
      wangle::TransportInfo* /*tinfo*/) const override;

  void setHeaderCodecStats(HeaderCodec::Stats* stats) override {
    versionUtilsReady_.then(
        [this, stats] { versionUtils_->setHeaderCodecStats(stats); });
  }

  void enableDoubleGoawayDrain() override {
  }

  // Upstream interface
  bool isReusable() const override {
    VLOG(4) << __func__ << " sess=" << *this;
    return !isClosing();
  }

  bool isClosing() const override {
    VLOG(4) << __func__ << " sess=" << *this;
    return (drainState_ != DrainState::NONE || dropping_);
  }

  void drain() override {
    notifyPendingShutdown();
  }

  folly::Optional<const HTTPMessage::HTTPPriority> getHTTPPriority(
      uint8_t /*level*/) override {
    return folly::none;
  }

  const quic::QuicSocket* getQuicSocket() const {
    return sock_.get();
  }

  // Override HTTPSessionBase address getter functions
  const folly::SocketAddress& getLocalAddress() const noexcept {
    return sock_ && sock_->good() ? sock_->getLocalAddress() : localAddr_;
  }

  const folly::SocketAddress& getPeerAddress() const noexcept {
    return sock_ && sock_->good() ? sock_->getPeerAddress() : peerAddr_;
  }

  void setPartiallyReliableCallbacks(quic::StreamId id);

  bool isPartialReliabilityEnabled() const noexcept {
    CHECK(versionUtils_) << ": versionUtils is not set";
    return versionUtils_->isPartialReliabilityEnabled();
  }

 protected:
  // Finds any transport-like stream that has not been detached
  // by quic stream id
  HQStreamTransportBase* findNonDetachedStream(quic::StreamId streamId);

  //  Find any transport-like stream by quic stream id
  HQStreamTransportBase* findStream(quic::StreamId streamId);

  // Find any transport-like stream suitable for ingress (request/push-ingress)
  HQStreamTransportBase* findIngressStream(quic::StreamId streamId,
                                           bool includeDetached = false);
  // Find any transport-like stream suitable for egress (request/push-egress)
  HQStreamTransportBase* findEgressStream(quic::StreamId streamId,
                                          bool includeDetached = false);

  // Find an ingress push stream
  HQIngressPushStream* findIngressPushStream(quic::StreamId);
  HQIngressPushStream* findIngressPushStreamByPushId(hq::PushId);

  // Find an egress push stream
  HQEgressPushStream* findEgressPushStream(quic::StreamId);
  HQEgressPushStream* findEgressPushStreamByPushId(hq::PushId);

  // Erase the stream. Returns true if the stream
  // has been erased
  bool eraseStream(quic::StreamId);
  bool eraseStreamByPushId(hq::PushId);

  // Find a control stream by type
  HQControlStream* findControlStream(hq::UnidirectionalStreamType streamType);

  // Find a control stream by stream id (either ingress or egress)
  HQControlStream* findControlStream(quic::StreamId streamId);

  // NOTE: for now we are using uint32_t as the limit for
  // the number of streams.
  uint32_t numberOfStreams() const;
  uint32_t numberOfIngressStreams() const;
  uint32_t numberOfEgressStreams() const;
  uint32_t numberOfIngressPushStreams() const;
  uint32_t numberOfEgressPushStreams() const;

  /*
   * for HQ we need a read callback for unidirectional streams to read the
   * stream type from the the wire to decide whether a stream is
   * a control stream, a header codec/decoder stream or a push stream
   *
   * This part is now implemented in HQUnidirStreamDispatcher
   */

  // Callback methods that are invoked by the dispatcher
  void assignPeekCallback(
      quic::StreamId /* id */,
      hq::UnidirectionalStreamType /* type */,
      size_t /* toConsume */,
      quic::QuicSocket::PeekCallback* const /* cb */) override;

  void onNewPushStream(quic::StreamId /* pushStreamId */,
                       hq::PushId /* pushId */,
                       size_t /* toConsume */) override;

  void assignReadCallback(
      quic::StreamId /* id */,
      hq::UnidirectionalStreamType /* type */,
      size_t /* toConsume */,
      quic::QuicSocket::ReadCallback* const /* cb */) override;

  void rejectStream(quic::StreamId /* id */) override;

  bool isPartialReliabilityEnabled(quic::StreamId /* id */) override;

  void onPartialDataAvailable(
      quic::StreamId /* id */,
      const HQUnidirStreamDispatcher::Callback::PeekData& /* data */) override;

  void processExpiredData(quic::StreamId /* id */,
                          uint64_t /* offset */) override;

  void processRejectedData(quic::StreamId /* id */,
                           uint64_t /* offset */) override;

  folly::Optional<hq::UnidirectionalStreamType> parseStreamPreface(
      uint64_t preface) override;

  void controlStreamReadAvailable(quic::StreamId /* id */) override;

  void controlStreamReadError(
      quic::StreamId /* id */,
      const HQUnidirStreamDispatcher::Callback::ReadError& /* err */) override;

  /**
   * Attempt to bind an ingress push stream object (which has the txn)
   * to a nascent stream (which has the transport/codec).
   * If successful, remove the nascent stream and
   * re-enable ingress.
   * returns true if binding was successful
   */
  bool tryBindIngressStreamToTxn(hq::PushId pushId,
                                 HQIngressPushStream* pushStream = nullptr);

  /**
   * HQSession is an HTTPSessionBase that uses QUIC as the underlying transport
   *
   * HQSession is an abstract base class and cannot be instantiated
   * directly. If you want to handle requests and send responses (act as a
   * server), construct a HQDownstreamSession. If you want to make
   * requests and handle responses (act as a client), construct a
   * HQUpstreamSession.
   */
  HQSession(const std::chrono::milliseconds transactionsTimeout,
            HTTPSessionController* controller,
            proxygen::TransportDirection direction,
            const wangle::TransportInfo& tinfo,
            InfoCallback* sessionInfoCb,
            folly::Function<void(HTTPCodecFilterChain& chain)>
            /* codecFilterCallbackFn */
            = nullptr)
      : HTTPSessionBase(folly::SocketAddress(),
                        folly::SocketAddress(),
                        controller,
                        tinfo,
                        sessionInfoCb,
                        std::make_unique<HTTP1xCodec>(direction),
                        WheelTimerInstance(),
                        hq::kSessionStreamId),
        direction_(direction),
        transactionsTimeout_(transactionsTimeout),
        started_(false),
        dropping_(false),
        inLoopCallback_(false),
        unidirectionalReadDispatcher_(*this) {
    codec_.add<HTTPChecks>();
    // dummy, ingress, egress
    codecStack_.reserve(kMaxCodecStackDepth);
    codecStack_.emplace_back(nullptr, nullptr, nullptr);

    attachToSessionController();
    nextEgressResults_.reserve(maxConcurrentIncomingStreams_);
    quicInfo_ = std::make_shared<QuicProtocolInfo>();
  }

  // EventBase::LoopCallback methods
  void runLoopCallback() noexcept override;

  /**
   * Called by transactionTimeout if the transaction has no handler.
   */
  virtual HTTPTransaction::Handler* getTransactionTimeoutHandler(
      HTTPTransaction* txn) = 0;

  /**
   * Called by onHeadersComplete(). This function allows downstream and
   * upstream to do any setup (like preparing a handler) when headers are
   * first received from the remote side on a given transaction.
   */
  virtual void setupOnHeadersComplete(HTTPTransaction* txn,
                                      HTTPMessage* msg) = 0;

  virtual void onConnectionErrorHandler(
      std::pair<quic::QuicErrorCode, std::string> error) noexcept = 0;

  void applySettings(const SettingsList& settings);

  virtual void connectSuccess() noexcept {
  }

  bool isPeerUniStream(quic::StreamId id) {
    return sock_->isUnidirectionalStream(id) &&
           ((direction_ == TransportDirection::DOWNSTREAM &&
             sock_->isClientStream(id)) ||
            (direction_ == TransportDirection::UPSTREAM &&
             sock_->isServerStream(id)));
  }

  bool isSelfUniStream(quic::StreamId id) {
    return sock_->isUnidirectionalStream(id) &&
           ((direction_ == TransportDirection::DOWNSTREAM &&
             sock_->isServerStream(id)) ||
            (direction_ == TransportDirection::UPSTREAM &&
             sock_->isClientStream(id)));
  }

  void abortStream(HTTPException::Direction dir,
                   quic::StreamId id,
                   HTTP3::ErrorCode err);

  proxygen::TransportDirection direction_;
  std::chrono::milliseconds transactionsTimeout_;
  TimePoint transportStart_;

  std::shared_ptr<quic::QuicSocket> sock_;

 private:
  std::unique_ptr<HTTPCodec> createStreamCodec(quic::StreamId streamId);
  HQStreamTransport* createStreamTransport(quic::StreamId streamId);
  bool createEgressControlStreams();
  HQControlStream* tryCreateIngressControlStream(quic::StreamId id,
                                                 uint64_t preface);

  bool createEgressControlStream(hq::UnidirectionalStreamType streamType);

  HQControlStream* createIngressControlStream(
      quic::StreamId id, hq::UnidirectionalStreamType streamType);

  HQIngressPushStream* FOLLY_NULLABLE
  createIngressPushStream(quic::StreamId parentStreamId, hq::PushId pushId);

  // gets the ALPN from the transport and returns whether the protocol is
  // supported. Drops the connection if not supported
  bool getAndCheckApplicationProtocol();
  void setVersionUtils();

  void onDeliveryAck(quic::StreamId id,
                     uint64_t offset,
                     std::chrono::microseconds rtt) override;

  void onCanceled(quic::StreamId id, uint64_t offset) override;

  // helper functions for reads
  void readRequestStream(quic::StreamId id) noexcept;
  void readControlStream(HQControlStream* controlStream);

  void processReadData();
  void resumeReads(quic::StreamId id);
  void pauseReads(quic::StreamId id);

  void pauseTransactions() override;

  void notifyEgressBodyBuffered(int64_t bytes);

  // Schedule the loop callback.
  // To keep this consistent with EventBase::runInLoop run in the next loop
  // by default
  void scheduleLoopCallback(bool thisIteration = false);

  // helper functions for writes
  void writeRequestStreams(uint64_t maxEgress) noexcept;
  void scheduleWrite();
  void handleWriteError(HQStreamTransportBase* hqStream,
                        quic::QuicErrorCode err);

  /**
   * Handles the write to the socket and errors for a request stream.
   * Returns the number of bytes written from data.
   */
  size_t handleWrite(HQStreamTransportBase* hqStream,
                     std::unique_ptr<folly::IOBuf> data,
                     size_t length,
                     bool sendEof);

  /**
   * Wraps calls to the socket writeChain and handles the case where the
   * transport gives data back to the caller. To be used for both request and
   * control streams. Returns the number of bytes written from data if success,
   * in case of error returns the transport error, and leaves error handling to
   * the caller
   */
  folly::Expected<size_t, quic::LocalErrorCode> writeBase(
      quic::StreamId id,
      folly::IOBufQueue* writeBuf,
      std::unique_ptr<folly::IOBuf> data,
      size_t tryToSend,
      bool sendEof,
      quic::QuicSocket::DeliveryCallback* deliveryCallback = nullptr);

  /**
   * Helper function to perform writes on a single request stream
   * The first argument defines whether the implementation should
   * call onWriteReady on the transaction to get data allocated
   * in the write buffer.
   * Returns the number of bytes written to the transport
   */
  uint64_t requestStreamWriteImpl(HQStreamTransport* hqStream,
                                  uint64_t maxEgress,
                                  double ratio);

  uint64_t writeControlStreams(uint64_t maxEgress);
  uint64_t controlStreamWriteImpl(HQControlStream* ctrlStream,
                                  uint64_t maxEgress);
  void handleSessionError(HQStreamBase* stream,
                          hq::StreamDirection streamDir,
                          quic::QuicErrorCode err,
                          ProxygenError proxygenError);

  void detachStreamTransport(HQStreamTransportBase* hqStream);

  void drainImpl();

  void checkForShutdown();
  void onGoawayAck();
  quic::StreamId getGoawayStreamId();

  void errorOnTransactionId(quic::StreamId id, HTTPException ex);

  /**
   * Shared implementation of "findXXXstream" methods
   */
  HQStreamTransportBase* findStreamImpl(quic::StreamId streamId,
                                        bool includeEgress = true,
                                        bool includeIngress = true,
                                        bool includeDetached = true);

  /**
   * Shared implementation of "numberOfXXX" methods
   */
  uint32_t countStreamsImpl(bool includeEgress = true,
                            bool includeIngress = true) const;

  /**
   * The following functions invoke a callback on all or on all non-detached
   * request streams. It does an extra lookup per stream but it is safe. Note
   * that if the callback *adds* streams, they will not get the callback.
   */
  template <typename... Args1, typename... Args2>
  void invokeOnAllStreams(std::function<void(HQStreamTransportBase*)> fn) {
    invokeOnStreamsImpl(
        fn,
        std::bind(&HQSession::findStream, this, std::placeholders::_1),
        std::bind(&HQSession::findIngressPushStreamByPushId,
                  this,
                  std::placeholders::_1));
  }

  template <typename... Args1, typename... Args2>
  void invokeOnEgressStreams(std::function<void(HQStreamTransportBase*)> fn,
                             bool includeDetached = false) {
    invokeOnStreamsImpl(fn,
                        std::bind(&HQSession::findEgressStream,
                                  this,
                                  std::placeholders::_1,
                                  includeDetached));
  }

  template <typename... Args1, typename... Args2>
  void invokeOnIngressStreams(std::function<void(HQStreamTransportBase*)> fn,
                              bool includeDetached = false) {
    invokeOnStreamsImpl(fn,
                        std::bind(&HQSession::findIngressStream,
                                  this,
                                  std::placeholders::_1,
                                  includeDetached),
                        std::bind(&HQSession::findIngressPushStreamByPushId,
                                  this,
                                  std::placeholders::_1));
  }

  template <typename... Args1, typename... Args2>
  void invokeOnNonDetachedStreams(
      std::function<void(HQStreamTransportBase*)> fn) {
    invokeOnStreamsImpl(fn,
                        std::bind(&HQSession::findNonDetachedStream,
                                  this,
                                  std::placeholders::_1));
  }

  // Apply the function on the streams found by the two locators.
  // Note that same stream can be returned by a find-by-stream-id
  // and find-by-push-id locators.
  // This is mitigated by collecting the streams in an unordered set
  // prior to application of the funtion
  // Note that the function is allowed to delete a stream by invoking
  // erase stream, but the locators are not allowed to do so.
  // Note that neither the locators nor the function are allowed
  // to call "invokeOnStreamsImpl"
  template <typename... Args1, typename... Args2>
  void invokeOnStreamsImpl(
      std::function<void(HQStreamTransportBase*)> fn,
      std::function<HQStreamTransportBase*(quic::StreamId)> findByStreamIdFn,
      std::function<HQStreamTransportBase*(hq::PushId)> findByPushIdFn =
          [](hq::PushId /* id */) { return nullptr; }) {
    DestructorGuard g(this);
    std::unordered_set<HQStreamTransportBase*> streams;
    streams.reserve(numberOfStreams());

    for (const auto& txn : streams_) {
      HQStreamTransportBase* pstream = findByStreamIdFn(txn.first);
      if (pstream) {
        streams.insert(pstream);
      }
    }

    for (const auto& txn : egressPushStreams_) {
      HQStreamTransportBase* pstream = findByStreamIdFn(txn.first);
      if (pstream) {
        streams.insert(pstream);
      }
    }

    for (const auto& txn : ingressPushStreams_) {
      HQStreamTransportBase* pstream = findByPushIdFn(txn.first);
      if (pstream) {
        streams.insert(pstream);
      }
    }

    for (HQStreamTransportBase* pstream : streams) {
      CHECK(pstream);
      fn(pstream);
    }
  }

  std::list<folly::AsyncTransport::ReplaySafetyCallback*>
      waitingForReplaySafety_;

  /**
   * With HTTP/1.1 codecs, graceful shutdown happens when the session has sent
   * and received a Connection: close header, and all streams have completed.
   *
   * The application can signal intent to drain by calling notifyPendingShutdown
   * (or its alias, drain).  The peer can signal intent to drain by including
   * a Connection: close header.
   *
   * closeWhenIdle will bypass the requirement to send/receive Connection:
   * close, and the socket will terminate as soon as the stream count reaches 0.
   *
   * dropConnection will forcibly close all streams and guarantee that the
   * HQSession has been deleted before exiting.
   *
   * The intent is that an application will first notifyPendingShutdown() all
   * open sessions.  Then after some period of time, it will call closeWhenIdle.
   * As a last resort, it will call dropConnection.
   *
   * Note we allow the peer to create streams after draining because of out
   * of order delivery.
   *
   * drainState_ tracks the progress towards shutdown.
   *
   *  NONE - no shutdown requested
   *  PENDING - shutdown requested but no Connection: close seen
   *  CLOSE_SENT - sent Connection: close but not received
   *  CLOSE_RECEIVED - received Connection: close but not sent
   *  DONE - sent and received Connection: close.
   *
   *  NONE ---> PENDING ---> CLOSE_SENT --+--> DONE
   *    |          |                      |
   *    +----------+-------> CLOSE_RECV --+
   *
   * For sessions with a control stream shutdown is driven by GOAWAYs.
   * Only the server can send GOAWAYs so the behavior is asymmetric between
   * upstream and downstream
   *
   *  NONE - no shutdown requested
   *  PENDING - shutdown requested but no GOAWAY sent/received yet
   *  FIRST_GOAWAY - first GOAWAY received/sent
   *  SECOND_GOAWAY - downstream only - second GOAWAY sent
   *  DONE - two GOAWAYs sent/received. can close when request streams are done
   *
   */
  enum DrainState : uint8_t {
    NONE = 0,
    PENDING = 1,
    CLOSE_SENT = 2,
    CLOSE_RECEIVED = 3,
    FIRST_GOAWAY = 4,
    SECOND_GOAWAY = 5,
    DONE = 6
  };

  DrainState drainState_{DrainState::NONE};
  bool started_ : 1;
  bool dropping_ : 1;
  bool inLoopCallback_ : 1;
  folly::Optional<
      std::pair<std::pair<quic::QuicErrorCode, std::string>, ProxygenError>>
      dropInNextLoop_;

  // A control stream is created as egress first, then the ingress counterpart
  // is linked as soon as we read the stream preface on the associated stream
  class HQControlStream
      : public detail::composite::CSBidir
      , public HQStreamBase
      , public hq::HQUnidirectionalCodec::Callback
      , public quic::QuicSocket::DeliveryCallback {
   public:
    HQControlStream() = delete;
    HQControlStream(HQSession& session,
                    quic::StreamId egressStreamId,
                    hq::UnidirectionalStreamType type)
        : detail::composite::CSBidir(egressStreamId, folly::none),
          HQStreamBase(session, session.codec_, type) {
      createEgressCodec();
    }

    void createEgressCodec() {
      CHECK(type_.hasValue());
      switch (*type_) {
        case hq::UnidirectionalStreamType::H1Q_CONTROL:
        case hq::UnidirectionalStreamType::CONTROL:
          realCodec_ =
              std::make_unique<hq::HQControlCodec>(getEgressStreamId(),
                                                   session_.direction_,
                                                   hq::StreamDirection::EGRESS,
                                                   session_.egressSettings_,
                                                   *type_);
          break;
        case hq::UnidirectionalStreamType::QPACK_ENCODER:
        case hq::UnidirectionalStreamType::QPACK_DECODER:
          // These are statically allocated in the session
          break;
        default:
          LOG(FATAL) << "Failed to create egress codec."
            << " unrecognized stream type=" << static_cast<uint64_t>(*type_);
      }
    }

    void setIngressCodec(std::unique_ptr<hq::HQUnidirectionalCodec> codec) {
      ingressCodec_ = std::move(codec);
    }

    void processReadData();

    // QuicSocket::DeliveryCallback
    void onDeliveryAck(quic::StreamId id,
                       uint64_t offset,
                       std::chrono::microseconds rtt) override;
    void onCanceled(quic::StreamId id, uint64_t offset) override;

    // HTTPCodec::Callback
    void onMessageBegin(HTTPCodec::StreamID /*stream*/,
                        HTTPMessage* /*msg*/) override {
      LOG(FATAL) << __func__ << " called on a Control Stream.";
    }

    void onHeadersComplete(HTTPCodec::StreamID /*stream*/,
                           std::unique_ptr<HTTPMessage> /*msg*/) override {
      LOG(FATAL) << __func__ << " called on a Control Stream.";
    }

    void onBody(HTTPCodec::StreamID /*stream*/,
                std::unique_ptr<folly::IOBuf> /*chain*/,
                uint16_t /*padding*/) override {
      LOG(FATAL) << __func__ << " called on a Control Stream.";
    }

    void onTrailersComplete(
        HTTPCodec::StreamID /*stream*/,
        std::unique_ptr<HTTPHeaders> /*trailers*/) override {
      LOG(FATAL) << __func__ << " called on a Control Stream.";
    }

    void onMessageComplete(HTTPCodec::StreamID /*stream*/,
                           bool /*upgrade*/) override {
      LOG(FATAL) << __func__ << " called on a Control Stream.";
    }

    void onError(HTTPCodec::StreamID /*stream*/,
                 const HTTPException& /*error*/,
                 bool /* newTxn */ = false) override;

    void onGoaway(uint64_t lastGoodStreamID,
                  ErrorCode code,
                  std::unique_ptr<folly::IOBuf> debugData = nullptr) override {
      session_.onGoaway(lastGoodStreamID, code, std::move(debugData));
    }

    void onSettings(const SettingsList& settings) override {
      session_.onSettings(settings);
    }

    std::unique_ptr<hq::HQUnidirectionalCodec> ingressCodec_;
    bool readEOF_{false};
  };

 protected:
  class HQStreamTransportBase
      : public HQStreamBase
      , public HTTPTransaction::Transport
      , public HTTP2PriorityQueueBase
      , public quic::QuicSocket::DeliveryCallback {
   protected:
    HQStreamTransportBase(
        HQSession& session,
        TransportDirection direction,
        quic::StreamId id,
        uint32_t seqNo,
        const WheelTimerInstance& timeout,
        HTTPSessionStats* stats = nullptr,
        http2::PriorityUpdate priority = hqDefaultPriority,
        folly::Optional<HTTPCodec::StreamID> parentTxnId = HTTPCodec::NoStream,
        folly::Optional<hq::UnidirectionalStreamType> type = folly::none);

    void initCodec(std::unique_ptr<HTTPCodec> /* codec */,
                   const std::string& /* where */);

    void initIngress(const std::string& /* where */);

   public:
    HQStreamTransportBase() = delete;

    bool hasCodec() const {
      return hasCodec_;
    }

    bool hasIngress() const {
      return hasIngress_;
    }

    // process data in the read buffer, returns true if the codec is blocked
    bool processReadData();

    // Process data from QUIC onDataAvailable callback.
    void processPeekData(
        const folly::Range<quic::QuicSocket::PeekIterator>& peekData);

    // Process QUIC onDataExpired callback.
    void processDataExpired(uint64_t streamOffset);

    // Process QUIC onDataRejected callback.
    void processDataRejected(uint64_t streamOffset);

    // Helper to handle ingress skip/reject offset errors.
    void onIngressSkipRejectError(hq::UnframedBodyOffsetTrackerError error);

    // QuicSocket::DeliveryCallback
    void onDeliveryAck(quic::StreamId id,
                       uint64_t offset,
                       std::chrono::microseconds rtt) override;

    void onCanceled(quic::StreamId id, uint64_t offset) override;

    // HTTPCodec::Callback methods
    void onMessageBegin(HTTPCodec::StreamID streamID,
                        HTTPMessage* /* msg */) override;

    void onPushMessageBegin(HTTPCodec::StreamID /* pushID */,
                            HTTPCodec::StreamID /* parentTxnId */,
                            HTTPMessage* /* msg */) override;

    void onExMessageBegin(HTTPCodec::StreamID /* streamID */,
                          HTTPCodec::StreamID /* controlStream */,
                          bool /* unidirectional */,
                          HTTPMessage* /* msg */) override {
      LOG(ERROR) << __func__ << " txn=" << txn_ << " TODO";
    }

    virtual void onPushPromiseHeadersComplete(
        hq::PushId /* pushID */,
        HTTPCodec::StreamID /* assoc streamID */,
        std::unique_ptr<HTTPMessage> /* msg */) {
      LOG(ERROR) << __func__ << " txn=" << txn_ << " TODO";
    }

    void onHeadersComplete(HTTPCodec::StreamID streamID,
                           std::unique_ptr<HTTPMessage> msg) override;

    void onBody(HTTPCodec::StreamID /* streamID */,
                std::unique_ptr<folly::IOBuf> chain,
                uint16_t padding) override {
      VLOG(4) << __func__ << " txn=" << txn_;
      CHECK(chain);
      auto len = chain->computeChainDataLength();
      session_.onBodyImpl(std::move(chain), len, padding, &txn_);
    }

    void onUnframedBodyStarted(HTTPCodec::StreamID streamID,
                               uint64_t streamOffset) override;

    void onChunkHeader(HTTPCodec::StreamID /* stream */,
                       size_t length) override {
      VLOG(4) << __func__ << " txn=" << txn_;
      txn_.onIngressChunkHeader(length);
    }

    void onChunkComplete(HTTPCodec::StreamID /* stream */) override {
      VLOG(4) << __func__ << " txn=" << txn_;
      txn_.onIngressChunkComplete();
    }

    void onTrailersComplete(HTTPCodec::StreamID /* streamID */,
                            std::unique_ptr<HTTPHeaders> trailers) override {
      VLOG(4) << __func__ << " txn=" << txn_;
      txn_.onIngressTrailers(std::move(trailers));
    }

    void onMessageComplete(HTTPCodec::StreamID /* streamID */,
                           bool /* upgrade */) override {
      VLOG(4) << __func__ << " txn=" << txn_;
      // for 1xx responses (excluding 101) onMessageComplete may be called
      // more than once
      if (txn_.isUpstream() && txn_.extraResponseExpected()) {
        return;
      }
      if (session_.infoCallback_) {
        session_.infoCallback_->onRequestEnd(session_,
                                             txn_.getMaxDeferredSize());
      }
      // Pause the parser, which will prevent more than one message from being
      // processed
      auto g = folly::makeGuard(setActiveCodec(__func__));
      codecFilterChain->setParserPaused(true);
      eomGate_.set(EOMType::CODEC);
    }

    void onIngressEOF() {
      // Can only call this once
      CHECK(!eomGate_.get(EOMType::TRANSPORT));
      if (ingressError_) {
        // This codec has already errored, no need to give it more input
        return;
      }
      auto g = folly::makeGuard(setActiveCodec(__func__));
      codecFilterChain->onIngressEOF();
      eomGate_.set(EOMType::TRANSPORT);
    }

    void onError(HTTPCodec::StreamID streamID,
                 const HTTPException& error,
                 bool newTxn) override;

    // Invoked when we get a RST_STREAM from the transport
    void onResetStream(HTTP3::ErrorCode error, HTTPException ex);

    void onAbort(HTTPCodec::StreamID /* streamID */,
                 ErrorCode /* code */) override {
      VLOG(4) << __func__ << " txn=" << txn_;
      // Can't really get here since no HQ codecs can produce aborts.
      // The entry point is onResetStream via readError()
      LOG(DFATAL) << "Unexpected abort";
    }

    void onFrameHeader(HTTPCodec::StreamID /* stream_id */,
                       uint8_t /* flags */,
                       uint64_t /* length */,
                       uint64_t /* type */,
                       uint16_t /* version */ = 0) override {
      VLOG(4) << __func__ << " txn=" << txn_;
    }

    void onGoaway(
        uint64_t /* lastGoodStreamID */,
        ErrorCode /* code */,
        std::unique_ptr<folly::IOBuf> /* debugData */ = nullptr) override {
      VLOG(4) << __func__ << " txn=" << txn_;
    }

    void onPingRequest(uint64_t /* uniqueID */) override {
      VLOG(4) << __func__ << " txn=" << txn_;
    }

    void onPingReply(uint64_t /* uniqueID */) override {
      // This method should not get called
      LOG(FATAL) << __func__ << " txn=" << txn_;
    }

    void onWindowUpdate(HTTPCodec::StreamID /* stream */,
                        uint32_t /* amount */) override {
      VLOG(4) << __func__ << " txn=" << txn_;
    }

    void onSettings(const SettingsList& /*settings*/) override {
      VLOG(4) << __func__ << " txn=" << txn_;
    }

    void onSettingsAck() override {
      VLOG(4) << __func__ << " txn=" << txn_;
    }

    void onPriority(HTTPCodec::StreamID /* stream */,
                    const HTTPMessage::HTTPPriority& /* priority */) override {
      VLOG(4) << __func__ << " txn=" << txn_;
    }

    bool onNativeProtocolUpgrade(HTTPCodec::StreamID /* stream */,
                                 CodecProtocol /* protocol */,
                                 const std::string& /* protocolString */,
                                 HTTPMessage& /* msg */) override {
      VLOG(4) << __func__ << " txn=" << txn_;
      return false;
    }

    uint32_t numOutgoingStreams() const override {
      VLOG(4) << __func__ << " txn=" << txn_;
      return 0;
    }

    uint32_t numIncomingStreams() const override {
      VLOG(4) << __func__ << " txn=" << txn_;
      return 0;
    }

    // HTTPTransaction::Transport methods
    void pauseIngress(HTTPTransaction* /* txn */) noexcept override {
      VLOG(4) << __func__ << " txn=" << txn_;
      if (session_.sock_) {
        if (hasIngressStreamId()) {
          session_.sock_->pauseRead(getIngressStreamId());
        }
      } // else this is being torn down
    }

    void resumeIngress(HTTPTransaction* /* txn */) noexcept override {
      VLOG(4) << __func__ << " txn=" << txn_;
      if (session_.sock_) {
        if (hasIngressStreamId()) {
          session_.sock_->resumeRead(getIngressStreamId());
        }
      } // else this is being torn down
    }

    void transactionTimeout(HTTPTransaction* /* txn */) noexcept override;

    void sendHeaders(HTTPTransaction* txn,
                     const HTTPMessage& headers,
                     HTTPHeaderSize* size,
                     bool includeEOM) noexcept override;

    size_t sendBody(HTTPTransaction* txn,
                    std::unique_ptr<folly::IOBuf> body,
                    bool includeEOM,
                    bool trackLastByteFlushed) noexcept override;

    size_t sendChunkHeader(HTTPTransaction* txn,
                           size_t length) noexcept override;

    size_t sendChunkTerminator(HTTPTransaction* txn) noexcept override;

    size_t sendEOM(HTTPTransaction* txn,
                   const HTTPHeaders* trailers) noexcept override;

    size_t sendAbort(HTTPTransaction* txn,
                     ErrorCode statusCode) noexcept override;

    size_t sendAbortImpl(HTTP3::ErrorCode errorCode, std::string errorMsg);

    size_t sendPriority(
        HTTPTransaction* /* txn */,
        const http2::PriorityUpdate& /* pri */) noexcept override {
      VLOG(4) << __func__ << " txn=" << txn_;
      return 0;
    }

    size_t sendWindowUpdate(HTTPTransaction* /* txn */,
                            uint32_t /* bytes */) noexcept override {
      VLOG(4) << __func__ << " txn=" << txn_;
      return 0;
    }

    void notifyPendingEgress() noexcept override;

    void detach(HTTPTransaction* /* txn */) noexcept override {
      VLOG(4) << __func__ << " txn=" << txn_;
      detached_ = true;
      session_.scheduleLoopCallback();
    }
    void checkForDetach();

    void notifyIngressBodyProcessed(uint32_t bytes) noexcept override {
      VLOG(4) << __func__ << " txn=" << txn_;
      session_.notifyBodyProcessed(bytes);
    }

    void notifyEgressBodyBuffered(int64_t bytes) noexcept override {
      session_.notifyEgressBodyBuffered(bytes);
    }

    const folly::SocketAddress& getLocalAddress() const noexcept override {
      return session_.getLocalAddress();
    }

    const folly::SocketAddress& getPeerAddress() const noexcept override {
      return session_.getPeerAddress();
    }

    void describe(std::ostream& os) const override {
      session_.describe(os);
    }

    const wangle::TransportInfo& getSetupTransportInfo() const
        noexcept override {
      VLOG(4) << __func__ << " txn=" << txn_;
      return session_.transportInfo_;
    }

    bool getCurrentTransportInfo(wangle::TransportInfo* tinfo) override;

    virtual const HTTPCodec& getCodec() const noexcept override {
      return HQStreamBase::getCodec();
    }

    void drain() override {
      VLOG(4) << __func__ << " txn=" << txn_;
    }

    bool isDraining() const override {
      VLOG(4) << __func__ << " txn=" << txn_;
      return false;
    }

    HTTPTransaction* newPushedTransaction(
        HTTPCodec::StreamID /* parentTxnId */,
        HTTPTransaction::PushHandler* /* handler */) noexcept override {
      VLOG(4) << __func__ << " txn=" << txn_;
      return nullptr;
    }

    HTTPTransaction* newExTransaction(
        HTTPTransactionHandler* /* handler */,
        HTTPCodec::StreamID /* controlStream */,
        bool /* unidirectional */) noexcept override {
      VLOG(4) << __func__ << " txn=" << txn_;
      return nullptr;
    }

    std::string getSecurityProtocol() const override {
      VLOG(4) << __func__ << " txn=" << txn_;
      return "quic/tls1.3";
    }

    void addWaitingForReplaySafety(folly::AsyncTransport::ReplaySafetyCallback*
                                       callback) noexcept override {
      VLOG(4) << __func__ << " txn=" << txn_;
      if (session_.sock_->replaySafe()) {
        callback->onReplaySafe();
      } else {
        session_.waitingForReplaySafety_.push_back(callback);
      }
    }

    void removeWaitingForReplaySafety(
        folly::AsyncTransport::ReplaySafetyCallback*
            callback) noexcept override {
      VLOG(4) << __func__ << " txn=" << txn_;
      session_.waitingForReplaySafety_.remove(callback);
    }

    bool needToBlockForReplaySafety() const override {
      VLOG(4) << __func__ << " txn=" << txn_;
      return false;
    }

    const folly::AsyncTransportWrapper* getUnderlyingTransport() const
        noexcept override {
      VLOG(4) << __func__ << " txn=" << txn_;
      return nullptr;
    }

    bool isReplaySafe() const override {
      return session_.isReplaySafe();
    }

    void setHTTP2PrioritiesEnabled(bool /* enabled */) override {
    }
    bool getHTTP2PrioritiesEnabled() const override {
      return false;
    }

    folly::Optional<const HTTPMessage::HTTPPriority> getHTTPPriority(
        uint8_t /* pri */) override {
      VLOG(4) << __func__ << " txn=" << txn_;
      return HTTPMessage::HTTPPriority(hqDefaultPriority.streamDependency,
                                       hqDefaultPriority.exclusive,
                                       hqDefaultPriority.weight);
    }

    void generateGoaway();

    // Partially reliable transport methods.
    folly::Expected<folly::Unit, ErrorCode> peek(
        HTTPTransaction::PeekCallback peekCallback) override;

    folly::Expected<folly::Unit, ErrorCode> consume(size_t amount) override;

    folly::Expected<folly::Optional<uint64_t>, ErrorCode> skipBodyTo(
        HTTPTransaction* txn, uint64_t nextBodyOffset) override;

    folly::Expected<folly::Optional<uint64_t>, ErrorCode> rejectBodyTo(
        HTTPTransaction* txn, uint64_t nextBodyOffset) override;

    uint64_t trimPendingEgressBody(uint64_t wireOffset);

    /**
     * Returns whether or no we have any body bytes buffered in the stream, or
     * the txn has any body bytes buffered.
     */
    bool hasPendingBody() const;
    bool hasPendingEOM() const;
    bool hasPendingEgress() const;

    /**
     * Adapter class for managing different enqueued state between
     * HTTPTransaction and HQStreamTransport.  The decouples whether the
     * transaction thinks it is enqueued for egress (which impacts txn lifetime)
     * and whether the HQStreamTransport is enqueued (which impacts the
     * actual egress algorithm).  Note all 4 states are possible.
     */
    class HQPriHandle : public HTTP2PriorityQueueBase::BaseNode {
     public:
      void init(HTTP2PriorityQueueBase::Handle handle) {
        egressQueueHandle_ = handle;
        enqueued_ = handle->isEnqueued();
      }

      HTTP2PriorityQueueBase::Handle getHandle() const {
        return egressQueueHandle_;
      }

      void clearHandle() {
        egressQueueHandle_ = nullptr;
      }

      // HQStreamTransport is enqueued
      bool isStreamTransportEnqueued() const {
        return egressQueueHandle_->isEnqueued();
      }

      bool isTransactionEnqueued() const {
        return isEnqueued();
      }

      void setEnqueued(bool enqueued) {
        enqueued_ = enqueued;
      }

      bool isEnqueued() const override {
        return enqueued_;
      }

      uint64_t calculateDepth(bool includeVirtual = true) const override {
        return egressQueueHandle_->calculateDepth(includeVirtual);
      }

     private:
      HTTP2PriorityQueueBase::Handle egressQueueHandle_;
      bool enqueued_;
    };

    HTTP2PriorityQueueBase::Handle addTransaction(HTTPCodec::StreamID id,
                                                  http2::PriorityUpdate pri,
                                                  HTTPTransaction* txn,
                                                  bool permanent,
                                                  uint64_t* depth) override {
      queueHandle_.init(session_.txnEgressQueue_.addTransaction(
          id, pri, txn, permanent, depth));
      return &queueHandle_;
    }

    // update the priority of an existing node
    HTTP2PriorityQueueBase::Handle updatePriority(
        HTTP2PriorityQueueBase::Handle handle,
        http2::PriorityUpdate pri,
        uint64_t* depth) override {
      CHECK_EQ(handle, &queueHandle_);
      return session_.txnEgressQueue_.updatePriority(
          queueHandle_.getHandle(), pri, depth);
    }

    // Remove the transaction from the priority tree
    void removeTransaction(HTTP2PriorityQueueBase::Handle handle) override {
      CHECK_EQ(handle, &queueHandle_);
      session_.txnEgressQueue_.removeTransaction(queueHandle_.getHandle());
      queueHandle_.clearHandle();
    }

    // Notify the queue when a transaction has egress
    void signalPendingEgress(HTTP2PriorityQueueBase::Handle h) override {
      CHECK_EQ(h, &queueHandle_);
      queueHandle_.setEnqueued(true);
      signalPendingEgressImpl();
    }

    void signalPendingEgressImpl() {
      auto flowControl =
          session_.sock_->getStreamFlowControl(getEgressStreamId());
      if (!flowControl.hasError() && flowControl->sendWindowAvailable > 0) {
        session_.txnEgressQueue_.signalPendingEgress(queueHandle_.getHandle());
      } else {
        VLOG(4) << "Delay pending egress signal on blocked txn=" << txn_;
      }
    }

    // Notify the queue when a transaction no longer has egress
    void clearPendingEgress(HTTP2PriorityQueueBase::Handle h) override {
      CHECK_EQ(h, &queueHandle_);
      CHECK(queueHandle_.isTransactionEnqueued());
      queueHandle_.setEnqueued(false);
      if (pendingEOM_ || !writeBuf_.empty()) {
        // no-op
        // Only HQSession can clearPendingEgress for these cases
        return;
      }
      // The transaction has pending body data, but it decided to remove itself
      // from the egress queue since it's rate-limited
      if (queueHandle_.isStreamTransportEnqueued()) {
        session_.txnEgressQueue_.clearPendingEgress(queueHandle_.getHandle());
      }
    }

    void addPriorityNode(HTTPCodec::StreamID id,
                         HTTPCodec::StreamID parent) override {
      session_.txnEgressQueue_.addPriorityNode(id, parent);
    }

    /**
     * How many egress bytes we committed to transport, both written and
     * skipped.
     */
    uint64_t streamEgressCommittedByteOffset() const {
      return bytesWritten_ + bytesSkipped_;
    }

    /**
     * streamEgressCommittedByteOffset() plus any pending bytes in the egress
     * queue.
     */
    uint64_t streamWriteByteOffset() const {
      return streamEgressCommittedByteOffset() + writeBuf_.chainLength();
    }

    void abortIngress();

    void abortEgress(bool checkForDetach);

    void errorOnTransaction(ProxygenError err, const std::string& errorMsg);
    void errorOnTransaction(HTTPException ex);

    bool wantsOnWriteReady(size_t canSend) const;

    HQPriHandle queueHandle_;
    HTTPTransaction txn_;
    // need to send EOM
    bool pendingEOM_{false};
    // have read EOF
    bool readEOF_{false};
    bool hasCodec_{false};
    bool hasIngress_{false};
    bool detached_{false};
    bool ingressError_{false};
    enum class EOMType { CODEC, TRANSPORT };
    ConditionalGate<EOMType, 2> eomGate_;

    folly::Optional<HTTPCodec::StreamID> codecStreamId_;

    ByteEventTracker byteEventTracker_;

    // Stream + session protocol info
    std::shared_ptr<QuicStreamProtocolInfo> quicStreamProtocolInfo_;

    void armEgressHeadersAckCb(uint64_t streamOffset);

    bool egressHeadersAckOffsetSet() const {
      return egressHeadersAckOffset_.hasValue();
    }

    void resetEgressHeadersAckOffset() {
      egressHeadersAckOffset_ = folly::none;
    }

    uint64_t numActiveDeliveryCallbacks() const {
      return numActiveDeliveryCallbacks_;
    }

  private:
    folly::Optional<uint64_t> egressHeadersAckOffset_;
    // Track number of armed QUIC delivery callbacks.
    uint64_t numActiveDeliveryCallbacks_{0};

    // Used to store last seen ingress push ID between
    // the invocations of onPushPromiseBegin / onHeadersComplete.
    // It is being reset by
    //  - "onNewMessage" (in which case the push promise is being abandoned),
    //  - "onPushMessageBegin" (which may be abandonned / duplicate message id)
    //  - "onHeadersComplete" (not pending anymore)
    folly::Optional<hq::PushId> ingressPushId_;
  }; // HQStreamTransportBase

  class HQStreamTransport
      : public detail::singlestream::SSBidir
      , public HQStreamTransportBase {
   public:
    HQStreamTransport(
        HQSession& session,
        TransportDirection direction,
        quic::StreamId streamId,
        uint32_t seqNo,
        std::unique_ptr<HTTPCodec> codec,
        const WheelTimerInstance& timeout,
        HTTPSessionStats* stats = nullptr,
        http2::PriorityUpdate priority = hqDefaultPriority,
        folly::Optional<HTTPCodec::StreamID> parentTxnId = HTTPCodec::NoStream)
        : detail::singlestream::SSBidir(streamId),
          HQStreamTransportBase(session,
                                direction,
                                static_cast<HTTPCodec::StreamID>(streamId),
                                seqNo,
                                timeout,
                                stats,
                                priority,
                                parentTxnId) {
      // Request streams are eagerly initialized
      initCodec(std::move(codec), __func__);
      initIngress(__func__);
    }

    void onPushPromiseHeadersComplete(
        hq::PushId /* pushID */,
        HTTPCodec::StreamID /* assoc streamID */,
        std::unique_ptr<HTTPMessage> /* promise */) override;

  }; // HQStreamTransport

  /**
   * Server side representation of a push stream
   * Does not support ingress
   */
  class HQEgressPushStream
      : public detail::singlestream::SSEgress
      , public HQStreamTransportBase {
   public:
    HQEgressPushStream(HQSession& session,
                       quic::StreamId streamId,
                       hq::PushId pushId,
                       folly::Optional<HTTPCodec::StreamID> parentTxnId,
                       uint32_t seqNo,
                       std::unique_ptr<HTTPCodec> codec,
                       const WheelTimerInstance& timeout,
                       HTTPSessionStats* stats = nullptr,
                       http2::PriorityUpdate priority = hqDefaultPriority)
        : detail::singlestream::SSEgress(streamId),
          HQStreamTransportBase(session,
                                TransportDirection::DOWNSTREAM,
                                static_cast<HTTPCodec::StreamID>(pushId),
                                seqNo,
                                timeout,
                                stats,
                                priority,
                                parentTxnId,
                                hq::UnidirectionalStreamType::PUSH),
          pushId_(pushId) {
      // Request streams are eagerly initialized
      initCodec(std::move(codec), __func__);
      // DONT init ingress on egress-only stream
    }

    hq::PushId getPushId() const {
      return pushId_;
    }

    // Unlike request streams and ingres push streams,
    // the egress push stream does not have to flush
    // ingress queues
    void transactionTimeout(HTTPTransaction* txn) noexcept override {
      VLOG(4) << __func__ << " txn=" << txn_;
      DCHECK(txn == &txn_);
    }

    // Egress only stream should not pause ingress
    void pauseIngress(HTTPTransaction* /* txn */) noexcept override {
      LOG(ERROR) << __func__ << "Ingress function called on egress-only stream";
      // Seems like an API problem - the handler called pause on txn?
      // Perhaps this should be a DCHECK?
      session_.dropConnectionWithError(
          std::make_pair(HTTP3::ErrorCode::HTTP_INTERNAL_ERROR,
                         "Ingress function called on egress-only stream"),
          kErrorUnknown);
    }

   private:
    hq::PushId pushId_; // The push id in context of which this stream is sent
  };                    // HQEgressPushStream

  /**
   * Client-side representation of a push stream.
   * Does not support egress operations.
   */
  class HQIngressPushStream
      : public detail::singlestream::SSIngress
      , public HQSession::HQStreamTransportBase {
   public:
    HQIngressPushStream(HQSession& session,
                        hq::PushId pushId,
                        folly::Optional<HTTPCodec::StreamID> parentTxnId,
                        uint32_t seqNo,
                        const WheelTimerInstance& timeout,
                        HTTPSessionStats* stats = nullptr,
                        http2::PriorityUpdate priority = hqDefaultPriority)
        : detail::singlestream::SSIngress(folly::none),
          HQStreamTransportBase(session,
                                TransportDirection::UPSTREAM,
                                static_cast<HTTPCodec::StreamID>(pushId),
                                seqNo,
                                timeout,
                                stats,
                                priority,
                                parentTxnId,
                                hq::UnidirectionalStreamType::PUSH),
          pushId_(pushId) {
      // Ingress push streams are not initialized
      // until after the nascent push stream
      // has been received
    }

    // Bind this stream to a transport stream
    void bindTo(quic::StreamId transportStreamId);

    void onPushMessageBegin(HTTPCodec::StreamID pushId,
                            HTTPCodec::StreamID parentTxnId,
                            HTTPMessage* /* msg */) override {
      LOG(ERROR) << __func__
                 << "Push streams are not allowed to receive push promises"
                 << " txn=" << txn_ << " pushID=" << pushId
                 << " parentTxnId=" << parentTxnId;
      session_.dropConnectionWithError(
          std::make_pair(HTTP3::ErrorCode::HTTP_WRONG_STREAM,
                         "Push promise over a push stream"),
          kErrorConnection);
    }

    hq::PushId getPushId() const {
      return pushId_;
    }

   private:
    hq::PushId pushId_; // The push id in context of which this stream is
                        // received
  };                    // HQIngressPushStream

 private:
  class VersionUtils {
   public:
    explicit VersionUtils(HQSession& session) : session_(session) {
    }
    virtual ~VersionUtils() {
    }

    // Checks whether it is allowed to process a new stream, depending on the
    // stream type, draining state/goaway. If not allowed, it resets the stream
    virtual CodecProtocol getCodecProtocol() const = 0;
    virtual bool checkNewStream(quic::StreamId id) = 0;
    virtual std::unique_ptr<HTTPCodec> createCodec(quic::StreamId id) = 0;
    virtual std::unique_ptr<hq::HQUnidirectionalCodec> createControlCodec(
        hq::UnidirectionalStreamType type, HQControlStream& controlStream) = 0;
    virtual folly::Optional<hq::UnidirectionalStreamType> parseStreamPreface(
        uint64_t preface) = 0;
    virtual void sendGoaway() = 0;
    virtual void sendGoawayOnRequestStream(
        HQSession::HQStreamTransport& stream) = 0;
    virtual void headersComplete(HTTPMessage* msg) = 0;
    virtual void checkSendingGoaway(const HTTPMessage& msg) = 0;
    virtual size_t sendSettings() = 0;
    virtual bool createEgressControlStreams() = 0;
    virtual void applySettings(const SettingsList& settings) = 0;
    virtual void onSettings(const SettingsList& settings) = 0;
    virtual void readDataProcessed() = 0;
    virtual void abortStream(quic::StreamId id) = 0;
    virtual void setMaxUncompressed(uint64_t) {
    }
    virtual void setHeaderCodecStats(HeaderCodec::Stats*) {
    }
    virtual bool isPartialReliabilityEnabled() const noexcept = 0;
    virtual folly::Expected<uint64_t, hq::UnframedBodyOffsetTrackerError>
    onIngressPeekDataAvailable(uint64_t /* streamOffset */) {
      LOG(FATAL) << ": called in base class";
    }
    virtual folly::Expected<uint64_t, hq::UnframedBodyOffsetTrackerError>
    onIngressDataExpired(uint64_t /* streamOffset */) {
      LOG(FATAL) << ": called in base class";
    }
    virtual folly::Expected<uint64_t, hq::UnframedBodyOffsetTrackerError>
    onIngressDataRejected(uint64_t /* streamOffset */) {
      LOG(FATAL) << ": called in base classn";
    }
    virtual folly::Expected<uint64_t, hq::UnframedBodyOffsetTrackerError>
    onEgressBodySkip(uint64_t /* bodyOffset */) {
      LOG(FATAL) << ": called in base class";
    }
    virtual folly::Expected<uint64_t, hq::UnframedBodyOffsetTrackerError>
    onEgressBodyReject(uint64_t /* bodyOffset */) {
      LOG(FATAL) << ": called in base class";
    }

    HQSession& session_;
  };

  class H1QFBV1VersionUtils : public VersionUtils {
   public:
    explicit H1QFBV1VersionUtils(HQSession& session) : VersionUtils(session) {
    }

    CodecProtocol getCodecProtocol() const override {
      return CodecProtocol::HTTP_1_1;
    }

    bool checkNewStream(quic::StreamId id) override;

    std::unique_ptr<HTTPCodec> createCodec(quic::StreamId id) override;

    std::unique_ptr<hq::HQUnidirectionalCodec> createControlCodec(
        hq::UnidirectionalStreamType, HQControlStream&) override {
      return nullptr; // no control streams
    }

    folly::Optional<hq::UnidirectionalStreamType> parseStreamPreface(
        uint64_t /*preface*/) override {
      LOG(FATAL) << "H1Q does not use stream preface";
      folly::assume_unreachable();
    }

    void sendGoaway() override;
    void sendGoawayOnRequestStream(
        HQSession::HQStreamTransport& stream) override;
    void headersComplete(HTTPMessage* msg) override;
    void checkSendingGoaway(const HTTPMessage& msg) override;

    size_t sendSettings() override {
      return 0;
    }

    bool createEgressControlStreams() override {
      return true;
    }

    void applySettings(const SettingsList& /*settings*/) override {
    }

    void onSettings(const SettingsList& /*settings*/) override {
      CHECK(false) << "SETTINGS frame received for h1q-fb-v1 protocol";
    }

    void readDataProcessed() override {
    }

    void abortStream(quic::StreamId /*id*/) override {
    }

    bool isPartialReliabilityEnabled() const noexcept override {
      return false;
    }
  };

  class GoawayUtils {
   public:
    static bool checkNewStream(HQSession& session, quic::StreamId id);
    static void sendGoaway(HQSession& session);
  };

  class HQVersionUtils : public VersionUtils {
   public:
    explicit HQVersionUtils(HQSession& session) : VersionUtils(session) {
    }

    CodecProtocol getCodecProtocol() const override {
      return CodecProtocol::HQ;
    }

    std::unique_ptr<HTTPCodec> createCodec(quic::StreamId id) override;

    std::unique_ptr<hq::HQUnidirectionalCodec> createControlCodec(
        hq::UnidirectionalStreamType type,
        HQControlStream& controlStream) override;

    bool checkNewStream(quic::StreamId id) override {
      return GoawayUtils::checkNewStream(session_, id);
    }
    folly::Optional<hq::UnidirectionalStreamType> parseStreamPreface(
        uint64_t preface) override;

    void sendGoaway() override {
      GoawayUtils::sendGoaway(session_);
    }

    void sendGoawayOnRequestStream(
        HQSession::HQStreamTransport& /*stream*/) override {
    }

    void headersComplete(HTTPMessage* /*msg*/) override;

    void checkSendingGoaway(const HTTPMessage& /*msg*/) override {
    }

    size_t sendSettings() override;

    bool createEgressControlStreams() override;

    void applySettings(const SettingsList& settings) override;

    void onSettings(const SettingsList& settings) override;

    void readDataProcessed() override;

    void abortStream(quic::StreamId /*id*/) override;

    void setMaxUncompressed(uint64_t value) override {
      qpackCodec_.setMaxUncompressed(value);
    }

    void setHeaderCodecStats(HeaderCodec::Stats* stats) override {
      qpackCodec_.setStats(stats);
    }

    bool isPartialReliabilityEnabled() const noexcept override {
      return session_.sock_ && session_.sock_->isPartiallyReliableTransport();
    }

    folly::Expected<uint64_t, hq::UnframedBodyOffsetTrackerError>
    onIngressPeekDataAvailable(uint64_t streamOffset) override;

    folly::Expected<uint64_t, hq::UnframedBodyOffsetTrackerError>
    onIngressDataExpired(uint64_t streamOffset) override;

    folly::Expected<uint64_t, hq::UnframedBodyOffsetTrackerError>
    onIngressDataRejected(uint64_t streamOffset) override;

    folly::Expected<uint64_t, hq::UnframedBodyOffsetTrackerError>
    onEgressBodySkip(uint64_t bodyOffset) override;

    folly::Expected<uint64_t, hq::UnframedBodyOffsetTrackerError>
    onEgressBodyReject(uint64_t bodyOffset) override;

   private:
    QPACKCodec qpackCodec_;
    hq::HQStreamCodec* hqStreamCodecPtr_{nullptr};
  };

  class H1QFBV2VersionUtils : public H1QFBV1VersionUtils {
   public:
    explicit H1QFBV2VersionUtils(HQSession& session)
        : H1QFBV1VersionUtils(session) {
    }

    bool checkNewStream(quic::StreamId id) override {
      return GoawayUtils::checkNewStream(session_, id);
    }
    std::unique_ptr<hq::HQUnidirectionalCodec> createControlCodec(
        hq::UnidirectionalStreamType, HQControlStream&) override;

    folly::Optional<hq::UnidirectionalStreamType> parseStreamPreface(
        uint64_t preface) override;

    bool createEgressControlStreams() override;

    void onSettings(const SettingsList& /*settings*/) override {
      session_.handleSessionError(
          CHECK_NOTNULL(session_.findControlStream(
              hq::UnidirectionalStreamType::H1Q_CONTROL)),
          hq::StreamDirection::INGRESS,
          HTTP3::ErrorCode::HTTP_GENERAL_PROTOCOL_ERROR,
          kErrorConnection);
    }

    void sendGoaway() override {
      GoawayUtils::sendGoaway(session_);
    }

    void sendGoawayOnRequestStream(
        HQSession::HQStreamTransport& /*stream*/) override {
    }

    void headersComplete(HTTPMessage* /*msg*/) override {
    }

    void checkSendingGoaway(const HTTPMessage& /*msg*/) override {
    }

    bool isPartialReliabilityEnabled() const noexcept override {
      return false;
    }
  };

  uint32_t getMaxConcurrentOutgoingStreamsRemote() const override {
    // need transport API
    return 100;
  }

  using HTTPCodecPtr = std::unique_ptr<HTTPCodec>;
  struct CodecStackEntry {
    HTTPCodecPtr* codecPtr;
    HTTPCodecPtr codec;
    HTTPCodec::Callback* callback;
    CodecStackEntry(HTTPCodecPtr* p, HTTPCodecPtr c, HTTPCodec::Callback* cb)
        : codecPtr(p), codec(std::move(c)), callback(cb) {
    }
  };
  std::vector<CodecStackEntry> codecStack_;

  /**
   * Container to hold the results of HTTP2PriorityQueue::nextEgress
   */
  HTTP2PriorityQueue::NextEgressResult nextEgressResults_;

  // Bidirectional transport streams
  std::unordered_map<quic::StreamId, HQStreamTransport> streams_;

  // Incoming server push streams. Since the incoming push streams
  // can be created before transport stream
  std::unordered_map<hq::PushId, HQIngressPushStream> ingressPushStreams_;

  // Lookup maps for matching ingress push streams to push ids
  PushToStreamMap streamLookup_;

  std::unordered_map<quic::StreamId, HQEgressPushStream> egressPushStreams_;

  // Cleanup all pending streams. Invoked in session timeout
  size_t cleanupPendingStreams();

  // Remove all callbacks from a stream during cleanup
  void clearStreamCallbacks(quic::StreamId /* id */);

  using ControlStreamsKey = std::pair<quic::StreamId, hq::StreamDirection>;
  std::unordered_map<hq::UnidirectionalStreamType, HQControlStream>
      controlStreams_;
  HQUnidirStreamDispatcher unidirectionalReadDispatcher_;
  // Callback pointer used for correctness testing. Not used
  // for session logic.
  ServerPushLifecycleCallback* serverPushLifecycleCb_{nullptr};

  // Maximum Stream ID received so far
  quic::StreamId maxIncomingStreamId_{0};
  // Maximum Stream ID that we are allowed to open, according to the remote
  quic::StreamId maxAllowedStreamId_{quic::kEightByteLimit};
  // Whether SETTINGS have been received
  bool receivedSettings_{false};

  /**
   * The maximum number of concurrent transactions that this session's peer
   * may create.
   */
  uint32_t maxConcurrentIncomingStreams_{100};
  folly::Optional<uint32_t> receiveStreamWindowSize_;

  uint64_t maxToSend_{0};
  bool scheduledWrite_{false};

  bool forceUpstream1_1_{true};

  /** Reads in the current loop iteration */
  uint16_t readsPerLoop_{0};
  std::unordered_set<quic::StreamId> pendingProcessReadSet_;
  std::shared_ptr<QuicProtocolInfo> quicInfo_;
  folly::Optional<HQVersion> version_;
  std::string alpn_;


 protected:
  HTTPSettings egressSettings_{
      {SettingsId::HEADER_TABLE_SIZE, hq::kDefaultEgressHeaderTableSize},
      {SettingsId::MAX_HEADER_LIST_SIZE, hq::kDefaultEgressMaxHeaderListSize},
      {SettingsId::_HQ_QPACK_BLOCKED_STREAMS,
       hq::kDefaultEgressQpackBlockedStream},
  };
  HTTPSettings ingressSettings_;

  std::unique_ptr<VersionUtils> versionUtils_;
  ReadyGate versionUtilsReady_;

  // NOTE: introduce better decoupling between the streams
  // and the containing session, then remove the friendship.
  friend class HQStreamBase;
}; // HQSession

} // namespace proxygen
