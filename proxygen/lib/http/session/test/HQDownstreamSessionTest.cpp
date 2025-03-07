/*
 *  Copyright (c) 2019-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include <proxygen/lib/http/session/HQDownstreamSession.h>
#include <folly/io/async/EventBaseManager.h>
#include <proxygen/lib/http/codec/HQControlCodec.h>
#include <proxygen/lib/http/codec/HQStreamCodec.h>
#include <proxygen/lib/http/codec/HQUnidirectionalCodec.h>
#include <proxygen/lib/http/codec/HTTP1xCodec.h>
#include <proxygen/lib/http/session/test/HQSessionTestCommon.h>
#include <proxygen/lib/http/session/test/HTTPSessionMocks.h>
#include <proxygen/lib/http/session/test/HQSessionMocks.h>
#include <proxygen/lib/http/session/test/HTTPTransactionMocks.h>
#include <proxygen/lib/http/session/test/MockQuicSocketDriver.h>
#include <proxygen/lib/http/session/test/TestUtils.h>
#include <quic/api/test/MockQuicSocket.h>
#include <wangle/acceptor/ConnectionManager.h>

#include <folly/futures/Future.h>
#include <folly/portability/GTest.h>

using namespace proxygen;
using namespace proxygen::hq;
using namespace quic;
using namespace folly;
using namespace testing;
using namespace std::chrono;

constexpr quic::StreamId kQPACKEncoderIngressStreamId = 6;
constexpr quic::StreamId kQPACKEncoderEgressStreamId = 7;

class TestTransportCallback : public HTTPTransactionTransportCallback {
 public:
  void firstHeaderByteFlushed() noexcept override {}

  void firstByteFlushed() noexcept override {}

  void lastByteFlushed() noexcept override {
  }

  void trackedByteFlushed() noexcept override {
  }

  void lastByteAcked(
      std::chrono::milliseconds /* latency */) noexcept override {
  }

  void headerBytesGenerated(HTTPHeaderSize& size) noexcept override {
    headerBytesGenerated_ += size.compressedBlock;
  }

  void headerBytesReceived(const HTTPHeaderSize& /* size */) noexcept override {
  }

  void bodyBytesGenerated(size_t /* nbytes */) noexcept override {
  }

  void bodyBytesReceived(size_t /* size */) noexcept override {
  }

  void lastEgressHeaderByteAcked() noexcept override {
    lastEgressHeadersByteDelivered_ = true;
  }

  uint64_t headerBytesGenerated_{0};
  bool lastEgressHeadersByteDelivered_{false};
};

class HQDownstreamSessionTest : public HQSessionTest {
 public:
  HQDownstreamSessionTest()
      : HQSessionTest(proxygen::TransportDirection::DOWNSTREAM) {
  }

 protected:
  HTTPCodec::StreamID sendRequest(const std::string& url = "/",
                                  int8_t priority = 0,
                                  bool eom = true) {
    auto req = getGetRequest();
    req.setURL(url);
    req.setPriority(priority);
    return sendRequest(req, eom);
  }

  quic::StreamId nextStreamId() {
    auto id = nextStreamId_;
    nextStreamId_ += 4;
    return id;
  }

  quic::StreamId sendRequest(const HTTPMessage& req,
                             bool eom = true,
                             quic::StreamId id = quic::kEightByteLimit) {
    if (id == quic::kEightByteLimit) {
      id = nextStreamId();
    }
    auto res = requests_.emplace(std::piecewise_construct,
                                 std::forward_as_tuple(id),
                                 std::forward_as_tuple(makeCodec(id)));
    auto& request = res.first->second;
    request.id = request.codec->createStream();
    request.readEOF = eom;
    request.codec->generateHeader(request.buf, request.id, req, eom);
    return id;
  }

  quic::StreamId sendHeader() {
    return sendRequest("/", 0, false);
  }

  Promise<Unit> sendRequestLater(HTTPMessage req, bool eof = false) {
    Promise<Unit> reqp;
    reqp.getSemiFuture().via(&eventBase_).thenValue([=](auto&&) {
      auto id = sendRequest(req, eof);
      socketDriver_->addReadEvent(
          id, getStream(id).buf.move(), milliseconds(0));
      socketDriver_->addReadEOF(id, milliseconds(0));
      // note that eof=true used to terminate the connection and now it
      // no longer does
    });
    return reqp;
  }

  void SetUp() override {
    SetUpBase();
    SetUpOnTransportReady();
  }

  void SetUpBase() {
    folly::EventBaseManager::get()->clearEventBase();
    transportInfo_ = {.srtt = std::chrono::microseconds(100),
                      .rttvar = std::chrono::microseconds(0),
                      .writableBytes = 0,
                      .congestionWindow = 1500,
                      .packetsRetransmitted = 0,
                      .timeoutBasedLoss = 0,
                      .pto = std::chrono::microseconds(0),
                      .bytesSent = 0,
                      .bytesRecvd = 0,
                      .ptoCount = 0,
                      .totalPTOCount = 0};
    EXPECT_CALL(*socketDriver_->getSocket(), getTransportInfo())
        .WillRepeatedly(Return(transportInfo_));

    streamTransInfo_ = {.totalHeadOfLineBlockedTime =
                            std::chrono::milliseconds(100),
                        .holbCount = 2,
                        .isHolb = true};

    EXPECT_CALL(*socketDriver_->getSocket(), getStreamTransportInfo(testing::_))
        .WillRepeatedly(Return(streamTransInfo_));

    localAddress_.setFromIpPort("0.0.0.0", 0);
    peerAddress_.setFromIpPort("127.0.0.0", 443);
    EXPECT_CALL(*socketDriver_->getSocket(), getLocalAddress())
        .WillRepeatedly(ReturnRef(localAddress_));
    EXPECT_CALL(*socketDriver_->getSocket(), getPeerAddress())
        .WillRepeatedly(ReturnRef(peerAddress_));
    EXPECT_CALL(*socketDriver_->getSocket(), getAppProtocol())
        .WillRepeatedly(Return(getProtocolString()));
    HTTPSession::setDefaultWriteBufferLimit(65536);
    HTTP2PriorityQueue::setNodeLifetime(std::chrono::milliseconds(2));
  }

  void SetUpOnTransportReady() {
    hqSession_->onTransportReady();

    if (createControlStreams()) {
      eventBase_.loopOnce();
      if (IS_HQ) {
        EXPECT_EQ(httpCallbacks_.settings, 1);
      }
    }
  }

  void TearDown() override {
    if (!IS_H1Q_FB_V1) {
      // with these versions we need to wait for GOAWAY delivery on the control
      // stream
      eventBase_.loop();
    }
  }

  template <class HandlerType>
  std::unique_ptr<testing::StrictMock<HandlerType>>
  addSimpleStrictHandlerBase() {
    auto handler = std::make_unique<testing::StrictMock<HandlerType>>();

    // The ownership model here is suspect, but assume the callers won't destroy
    // handler before it's requested
    auto rawHandler = handler.get();
    EXPECT_CALL(getMockController(), getRequestHandler(testing::_, testing::_))
        .WillOnce(testing::Return(rawHandler))
        .RetiresOnSaturation();

    EXPECT_CALL(*handler, setTransaction(testing::_))
        .WillOnce(testing::SaveArg<0>(&handler->txn_));

    return handler;
  }

  std::unique_ptr<testing::StrictMock<MockHTTPHandler>>
  addSimpleStrictHandler() {
    return addSimpleStrictHandlerBase<MockHTTPHandler>();
  }

  std::unique_ptr<testing::StrictMock<MockHqPrDownstreamHTTPHandler>>
  addSimpleStrictPrHandler() {
    return addSimpleStrictHandlerBase<MockHqPrDownstreamHTTPHandler>();
  }

  std::pair<quic::StreamId,
            std::unique_ptr<testing::StrictMock<MockHTTPHandler>>>
  checkRequest(HTTPMessage req = getGetRequest()) {
    auto id = sendRequest(req);
    auto handler = addSimpleStrictHandler();
    handler->expectHeaders();
    handler->expectEOM(
        [hdlr = handler.get()] { hdlr->sendReplyWithBody(200, 100); });
    handler->expectDetachTransaction();
    return {id, std::move(handler)};
  }

  void flushRequestsAndWaitForReads(
      bool eof = false,
      milliseconds eofDelay = milliseconds(0),
      milliseconds initialDelay = milliseconds(0),
      std::function<void()> extraEventsFn = std::function<void()>()) {
    while (!flushRequests(eof, eofDelay, initialDelay, extraEventsFn)) {
      CHECK(eventBase_.loop());
    }
    CHECK(eventBase_.loop());
  }

  void flushRequestsAndLoop(
      bool eof = false,
      milliseconds eofDelay = milliseconds(0),
      milliseconds initialDelay = milliseconds(0),
      std::function<void()> extraEventsFn = std::function<void()>()) {
    flushRequests(eof, eofDelay, initialDelay, extraEventsFn);
    CHECK(eventBase_.loop());
  }

  void flushRequestsAndLoopN(
      uint64_t n,
      bool eof = false,
      milliseconds eofDelay = milliseconds(0),
      milliseconds initialDelay = milliseconds(0),
      std::function<void()> extraEventsFn = std::function<void()>()) {
    flushRequests(eof, eofDelay, initialDelay, extraEventsFn);
    for (uint64_t i = 0; i < n; i++) {
      eventBase_.loopOnce();
    }
  }

  bool flushRequests(
      bool eof = false,
      milliseconds eofDelay = milliseconds(0),
      milliseconds initialDelay = milliseconds(0),
      std::function<void()> extraEventsFn = std::function<void()>()) {
    bool done = true;

    if (!encoderWriteBuf_.empty()) {
      socketDriver_->addReadEvent(
          kQPACKEncoderIngressStreamId, encoderWriteBuf_.move(), initialDelay);
      initialDelay = milliseconds(0);
    }
    for (auto& req : requests_) {
      if (socketDriver_->isStreamIdle(req.first)) {
        continue;
      }
      if (req.second.buf.chainLength() > 0) {
        socketDriver_->addReadEvent(
            req.first, req.second.buf.move(), initialDelay);
        done = false;
      }
      // EOM -> stream EOF
      if (req.second.readEOF) {
        socketDriver_->addReadEOF(req.first, eofDelay);
        done = false;
      }
    }
    if (extraEventsFn) {
      extraEventsFn();
    }
    if (eof || eofDelay.count() > 0) {
      /*  wonkiness.  Should somehow close the connection?
       * socketDriver_->addReadEOF(1, eofDelay);
       */
    }
    return done;
  }

  StrictMock<MockController>& getMockController() {
    return controllerContainer_.mockController;
  }

  std::unique_ptr<HTTPCodec> makeCodec(HTTPCodec::StreamID id) {
    if (IS_HQ) {
      return std::make_unique<hq::HQStreamCodec>(
          id,
          TransportDirection::UPSTREAM,
          qpackCodec_,
          encoderWriteBuf_,
          decoderWriteBuf_,
          [] { return std::numeric_limits<uint64_t>::max(); },
          egressSettings_,
          ingressSettings_,
          GetParam().prParams.hasValue());
    } else {
      return std::make_unique<HTTP1xCodec>(TransportDirection::UPSTREAM, true);
    }
  }

  struct ClientStream {
    explicit ClientStream(std::unique_ptr<HTTPCodec> c) : codec(std::move(c)) {
    }

    HTTPCodec::StreamID id;
    IOBufQueue buf{IOBufQueue::cacheChainLength()};
    bool readEOF{false};
    std::unique_ptr<HTTPCodec> codec;
  };

  ClientStream& getStream(HTTPCodec::StreamID id) {
    auto it = requests_.find(id);
    CHECK(it != requests_.end());
    return it->second;
  }

  void expectTransactionTimeout(
      testing::StrictMock<MockHTTPHandler>& handler,
      folly::Function<void()> fn = folly::Function<void()>()) {
    EXPECT_CALL(getMockController(), getTransactionTimeoutHandler(_, _))
        .WillOnce(Return(&handler));
    EXPECT_CALL(handler, setTransaction(testing::_))
        .WillOnce(testing::SaveArg<0>(&handler.txn_));
    handler.expectError([&handler, &fn](const HTTPException& ex) mutable {
      if (fn) {
        fn();
      }
      EXPECT_FALSE(ex.hasHttpStatusCode());
      handler.sendHeaders(408, 100);
      handler.sendBody(100);
      handler.sendEOM();
    });
    handler.expectDetachTransaction();
  }

  std::unordered_map<quic::StreamId, ClientStream> requests_;
  quic::StreamId nextStreamId_{0};
  quic::QuicSocket::TransportInfo transportInfo_;
  quic::QuicSocket::StreamTransportInfo streamTransInfo_;
  TestTransportCallback transportCallback_;
};

class HQDownstreamSessionBeforeTransportReadyTest
    : public HQDownstreamSessionTest {
  void SetUp() override {
    // Just do a basic setup, but don't call onTransportReady nor create the
    // control streams just yet, so to give the test a chance to manipulate
    // the session before onTransportReady
    SetUpBase();
  }
};

// Use this test class for h1q-fb only tests
using HQDownstreamSessionTestH1q = HQDownstreamSessionTest;
// Use this test class for h1q-fb-v1 only tests
using HQDownstreamSessionTestH1qv1 = HQDownstreamSessionTest;
// Use this test class for h1q-fb-v2 only tests
using HQDownstreamSessionTestH1qv2 = HQDownstreamSessionTest;
// Use this test class for h1q-fb-v2/hq common tests (goaway)
using HQDownstreamSessionTestH1qv2HQ = HQDownstreamSessionTest;

// Use this test class for hq only tests
using HQDownstreamSessionTestHQ = HQDownstreamSessionTest;
// Use this test class for hq PR only tests
using HQDownstreamSessionTestHQPR = HQDownstreamSessionTest;
using HQDownstreamSessionTestHQPrBadOffset = HQDownstreamSessionTest;

// Use this test class for h3 server push tests
using HQDownstreamSessionTestHQPush = HQDownstreamSessionTest;

TEST_P(HQDownstreamSessionTest, SimpleGet) {
  auto idh = checkRequest();
  flushRequestsAndLoop();
  EXPECT_GT(socketDriver_->streams_[idh.first].writeBuf.chainLength(), 110);
  EXPECT_TRUE(socketDriver_->streams_[idh.first].writeEOF);
  if (IS_HQ) {
    // Checks that the server response is sent using the QPACK dynamic table
    CHECK_GE(qpackCodec_.getCompressionInfo().ingressHeaderTableSize_, 0);
  }
  hqSession_->closeWhenIdle();
}

TEST_P(HQDownstreamSessionTest, GetStopSending) {
  auto id = sendRequest(getGetRequest());
  auto handler = addSimpleStrictHandler();
  handler->expectHeaders();
  handler->expectEOM([hdlr = handler.get()] { hdlr->sendHeaders(200, 100); });
  handler->expectError([](const proxygen::HTTPException& ex) {
    EXPECT_EQ(ex.getCodecStatusCode(), ErrorCode::CANCEL);
    EXPECT_EQ(ex.getProxygenError(), kErrorStreamAbort);
  });
  handler->expectDetachTransaction();
  flushRequestsAndLoopN(1);
  socketDriver_->addStopSending(id, HTTP3::ErrorCode::HTTP_REQUEST_CANCELLED);
  flushRequestsAndLoop();
  hqSession_->closeWhenIdle();
}

TEST_P(HQDownstreamSessionTest, HttpRateLimitNormal) {
  // The rate-limiting code grabs the event base from the EventBaseManager,
  // so we need to set it.
  folly::EventBaseManager::get()->setEventBase(&eventBase_, false);
  uint32_t rspLengthBytes = 100000;

  // make sure we are not limited by connection flow control
  socketDriver_->getSocket()->setConnectionFlowControlWindow(rspLengthBytes *
                                                             2);
  // Create a request
  auto id = sendRequest();

  // Set a low rate-limit on the transaction
  auto handler1 = addSimpleStrictHandler();
  handler1->expectHeaders([&] {
    uint32_t rateLimit_kbps = 640;
    handler1->txn_->setEgressRateLimit(rateLimit_kbps * 1024);
  });
  // Send a somewhat big response that we know will get rate-limited
  handler1->expectEOM([&] {
    // At 640kbps, this should take slightly over 800ms
    handler1->sendHeaders(200, rspLengthBytes);
    handler1->sendBody(rspLengthBytes);
    handler1->txn_->sendEOM();
  });
  EXPECT_CALL(*handler1, onEgressPaused()).Times(AtLeast(1));
  EXPECT_CALL(*handler1, onEgressResumed()).Times(AtLeast(1));
  handler1->expectDetachTransaction();
  flushRequestsAndLoop();

  // Check that the write side got blocked
  socketDriver_->expectStreamWritesPaused(id);
  // Open flow control again
  socketDriver_->getSocket()->setStreamFlowControlWindow(id,
                                                         rspLengthBytes * 2);
  flushRequestsAndLoop();

  hqSession_->closeWhenIdle();
}

TEST_P(HQDownstreamSessionTest, SimplePost) {
  auto id = sendRequest(getPostRequest(10), false);
  auto& request = getStream(id);
  request.codec->generateBody(
      request.buf, request.id, makeBuf(10), HTTPCodec::NoPadding, true);
  request.readEOF = true;
  auto handler = addSimpleStrictHandler();
  handler->expectHeaders();
  handler->expectBody(); // should check length too but meh
  handler->expectEOM([&handler] { handler->sendReplyWithBody(200, 100); });
  handler->expectDetachTransaction();
  flushRequestsAndLoop();
  EXPECT_GT(socketDriver_->streams_[id].writeBuf.chainLength(), 110);
  EXPECT_TRUE(socketDriver_->streams_[id].writeEOF);
  hqSession_->closeWhenIdle();
}

// HQ doesn't have the notion of chunked
TEST_P(HQDownstreamSessionTestH1q, ChunkedPost) {
  InSequence enforceOrder;

  auto id = sendRequest(getChunkedPostRequest(), false);
  auto& request = getStream(id);
  auto handler = addSimpleStrictHandler();
  handler->expectHeaders();
  for (int i = 1; i <= 3; i++) {
    auto size = 10 * i;
    request.codec->generateChunkHeader(request.buf, request.id, size);
    handler->expectChunkHeader();
    request.codec->generateBody(
        request.buf, request.id, makeBuf(size), HTTPCodec::NoPadding, false);
    handler->expectBody([size](uint64_t, std::shared_ptr<folly::IOBuf> buf) {
      EXPECT_EQ(size, buf->length());
    });
    request.codec->generateChunkTerminator(request.buf, request.id);
    handler->expectChunkComplete();
  }
  request.codec->generateEOM(request.buf, request.id);
  request.readEOF = true;
  handler->expectEOM([&handler] {
    // Chunked Transfer Encoding for the response too
    handler->sendChunkedReplyWithBody(200, 400, 100, false);
  });
  handler->expectDetachTransaction();
  flushRequestsAndLoop();
  EXPECT_TRUE(socketDriver_->streams_[id].writeEOF);
  hqSession_->closeWhenIdle();
}

TEST_P(HQDownstreamSessionTest, SimpleGetEofDelay) {
  auto idh = checkRequest();
  flushRequestsAndLoop(false, std::chrono::milliseconds(10));
  EXPECT_GT(socketDriver_->streams_[idh.first].writeBuf.chainLength(), 110);
  EXPECT_TRUE(socketDriver_->streams_[idh.first].writeEOF);
  hqSession_->closeWhenIdle();
}

TEST_P(HQDownstreamSessionTest, UnfinishedPost) {
  auto id = sendRequest(getPostRequest(10), false);
  auto& request = getStream(id);
  request.codec->generateBody(
      request.buf, request.id, makeBuf(9), HTTPCodec::NoPadding, true);
  request.readEOF = true;
  auto handler = addSimpleStrictHandler();
  handler->expectHeaders();
  handler->expectBody();
  handler->expectError([this, &handler](const proxygen::HTTPException& ex) {
    if (IS_HQ) {
      // The HTTP/1.1 parser tracks content-length and 400's if it is short
      // The HQStreamCodec does no such thing, and it's caught by
      // HTTPTransaction, with a different error.
      EXPECT_EQ(ex.getProxygenError(), kErrorParseBody);
    } else {
      EXPECT_TRUE(ex.hasHttpStatusCode());
      EXPECT_EQ(ex.getHttpStatusCode(), 400);
    }
    handler->sendReplyWithBody(400, 100);
    // afrind: this logic is in HTTPSession so should move to base or
    // duplicate in HQSession (see also custom error handlers)
  });
  handler->expectDetachTransaction();
  flushRequestsAndLoop();
  EXPECT_GT(socketDriver_->streams_[id].writeBuf.chainLength(), 110);
  EXPECT_TRUE(socketDriver_->streams_[id].writeEOF);
  hqSession_->dropConnection();
}

// This is a bit weird.  Extra junk after an HTTP/1.1 message now gets ignored
// until more junk or an EOF arrives.  Had to split the test into two loops.
TEST_P(HQDownstreamSessionTestH1qv1, TwoMessages) {
  auto id = sendRequest(getGetRequest(), false);
  auto handler = addSimpleStrictHandler();
  handler->expectHeaders();
  flushRequestsAndLoopN(1);

  // add a second request to the stream with Connection: close
  auto& request = getStream(id);
  auto req2 = getGetRequest();
  req2.getHeaders().add(HTTP_HEADER_CONNECTION, "close");
  request.codec->generateHeader(request.buf, request.id, req2, true);
  request.readEOF = true;
  hqSession_->notifyPendingShutdown();
  handler->expectError(
      [&handler](const HTTPException&) { handler->txn_->sendAbort(); });
  handler->expectDetachTransaction();
  flushRequestsAndLoop();
  hqSession_->closeWhenIdle();
}

TEST_P(HQDownstreamSessionTest, Multiplexing) {
  std::vector<std::unique_ptr<StrictMock<MockHTTPHandler>>> handlers;
  for (auto n = 0; n < 10; n++) {
    auto idh = checkRequest();
    handlers.emplace_back(std::move(idh.second));
  }
  flushRequestsAndWaitForReads();
  for (auto& req : requests_) {
    EXPECT_GT(socketDriver_->streams_[req.first].writeBuf.chainLength(), 110);
    EXPECT_TRUE(socketDriver_->streams_[req.first].writeEOF);
  }
  hqSession_->closeWhenIdle();
}

TEST_P(HQDownstreamSessionTest, Maxreadsperloop) {
  std::vector<std::unique_ptr<StrictMock<MockHTTPHandler>>> handlers;
  for (auto n = 0; n < 20; n++) {
    auto idh = checkRequest();
    handlers.emplace_back(std::move(idh.second));
  }

  flushRequestsAndLoopN(1);
  // After one loop, reads on some streams will be idle
  // while on some other they will not
  int idleCount = 0;
  int nonIdleCount = 0;
  for (auto& req : requests_) {
    if (socketDriver_->isStreamIdle(req.first)) {
      idleCount++;
    } else {
      nonIdleCount++;
    }
  }
  EXPECT_GT(idleCount, 0);
  EXPECT_GT(nonIdleCount, 0);

  // Now finish all the reads
  eventBase_.loop();
  for (auto& req : requests_) {
    EXPECT_GT(socketDriver_->streams_[req.first].writeBuf.chainLength(), 110);
    EXPECT_TRUE(socketDriver_->streams_[req.first].writeEOF);
  }
  hqSession_->closeWhenIdle();
}

TEST_P(HQDownstreamSessionTest, OnFlowControlUpdate) {
  auto id = sendRequest();
  auto handler = addSimpleStrictHandler();
  handler->expectHeaders();
  handler->expectEOM([&handler] { handler->sendReplyWithBody(200, 100); });
  handler->expectEgressPaused();
  handler->expectEgressResumed();
  handler->expectDetachTransaction();

  // Initialize the flow control window to less than the response body
  socketDriver_->setStreamFlowControlWindow(id, 10);
  flushRequestsAndLoop();
  // Check that the write side got blocked
  socketDriver_->expectStreamWritesPaused(id);
  // Open the flow control window
  socketDriver_->getSocket()->setStreamFlowControlWindow(id, 200);
  CHECK(eventBase_.loop());
  EXPECT_GT(socketDriver_->streams_[id].writeBuf.chainLength(), 110);
  EXPECT_TRUE(socketDriver_->streams_[id].writeEOF);
  hqSession_->closeWhenIdle();
}

TEST_P(HQDownstreamSessionTest, OnFlowControlUpdateOnUnknownStream) {
  auto id = sendRequest();
  auto handler = addSimpleStrictHandler();
  handler->expectHeaders();
  handler->expectEOM([&handler] { handler->sendReplyWithBody(200, 100); });
  handler->expectDetachTransaction();

  // Call flowControlUpdate on a stream the Application doesn't know
  socketDriver_->sock_->cb_->onFlowControlUpdate(id + 4);
  flushRequestsAndLoop();
  hqSession_->closeWhenIdle();
}

// This test does not work with header compression
TEST_P(HQDownstreamSessionTest, OnConnectionWindowPartialHeaders) {
  // Only enough conn window to send headers initially.
  auto id = sendRequest();
  auto handler = addSimpleStrictHandler();
  handler->expectHeaders();
  handler->expectEOM([&handler] { handler->sendReplyWithBody(200, 100); });
  // TODO: we should probably pause egress on conn limited.
  // handler->expectEgressPaused();
  // handler->expectEgressResumed();
  handler->expectDetachTransaction();

  // Initialize the flow control window to less than the response body
  socketDriver_->setConnectionFlowControlWindow(10 + numCtrlStreams_);
  flushRequestsAndLoop();
  // Check that the write side got blocked
  socketDriver_->expectConnWritesPaused();
  if (!IS_HQ) {
    // We should have 10 bytes pending to be written out.
    EXPECT_GE(socketDriver_->streams_[id].writeBuf.chainLength(), 10);
  } else {
    // We should have some bytes pending to be written out in the QPACK Encoder
    // stream
    EXPECT_GT(socketDriver_->streams_[kQPACKEncoderEgressStreamId]
                  .writeBuf.chainLength(),
              0);
  }
  EXPECT_FALSE(socketDriver_->streams_[id].writeEOF);
  // Open the flow control window
  socketDriver_->getSocket()->setConnectionFlowControlWindow(200);
  CHECK(eventBase_.loop());
  EXPECT_GT(socketDriver_->streams_[id].writeBuf.chainLength(), 110);
  EXPECT_TRUE(socketDriver_->streams_[id].writeEOF);
  hqSession_->closeWhenIdle();
}

TEST_P(HQDownstreamSessionTest, OnConnectionWindowPartialBody) {
  flushRequestsAndLoop(); // loop once for SETTINGS, etc
  // Only enough conn window to send headers initially.
  auto id = sendRequest();
  auto handler = addSimpleStrictHandler();
  handler->expectHeaders();
  handler->expectEOM([&handler] { handler->sendReplyWithBody(200, 100); });
  // TODO: we should probably pause egress on conn limited.
  // handler->expectEgressPaused();
  // handler->expectEgressResumed();
  handler->expectDetachTransaction();

  // Initialize the flow control window to less than the response body
  socketDriver_->setConnectionFlowControlWindow(110 + numCtrlStreams_);
  flushRequestsAndLoop();
  // Check that the write side got blocked
  socketDriver_->expectConnWritesPaused();
  if (!IS_HQ) {
    // We should have 110 bytes pending to be written out.
    EXPECT_GE(socketDriver_->streams_[id].writeBuf.chainLength(), 110);
  } else {
    // We should have some bytes pending to be written out in the QPACK Encoder
    // stream
    EXPECT_GT(socketDriver_->streams_[kQPACKEncoderEgressStreamId]
                  .writeBuf.chainLength(),
              0);
    EXPECT_GT(qpackCodec_.getCompressionInfo().egressHeaderTableSize_, 0);
  }
  EXPECT_FALSE(socketDriver_->streams_[id].writeEOF);
  // Open the flow control window
  socketDriver_->getSocket()->setConnectionFlowControlWindow(200 +
                                                             numCtrlStreams_);
  CHECK(eventBase_.loop());
  EXPECT_GT(socketDriver_->streams_[id].writeBuf.chainLength(), 110);
  EXPECT_TRUE(socketDriver_->streams_[id].writeEOF);
  hqSession_->closeWhenIdle();
}

TEST_P(HQDownstreamSessionTest, SeparateEom) {
  // Only enough conn window to send headers initially.
  auto id = sendRequest();
  auto handler = addSimpleStrictHandler();
  handler->expectHeaders();
  handler->expectEOM([&handler] {
    handler->sendHeaders(200, 100);
    handler->sendBody(100);
  });
  handler->expectDetachTransaction();
  flushRequestsAndLoop();
  EXPECT_GT(socketDriver_->streams_[id].writeBuf.chainLength(), 110);
  EXPECT_FALSE(socketDriver_->streams_[id].writeEOF);

  handler->sendEOM();
  // Open the flow control window
  CHECK(eventBase_.loop());
  EXPECT_GT(socketDriver_->streams_[id].writeBuf.chainLength(), 110);
  EXPECT_TRUE(socketDriver_->streams_[id].writeEOF);
  hqSession_->closeWhenIdle();
}

std::unique_ptr<folly::IOBuf> getSimpleRequestData() {
  std::string req("GET / HTTP/1.1\nHost: www.facebook.com\n\n");
  return folly::IOBuf::copyBuffer(req);
}

std::pair<size_t, size_t> estimateResponseSize(HTTPMessage msg,
                                               size_t contentLength,
                                               size_t chunkSize) {
  folly::IOBufQueue estimateSizeBuf{folly::IOBufQueue::cacheChainLength()};
  HTTP1xCodec codec(TransportDirection::DOWNSTREAM);
  MockHTTPCodecCallback callback;
  EXPECT_CALL(callback, onHeadersComplete(_, _));
  EXPECT_CALL(callback, onMessageBegin(_, _));
  codec.setCallback(&callback);
  auto txn = codec.createStream();
  codec.onIngress(*getSimpleRequestData());

  codec.generateHeader(estimateSizeBuf, txn, msg);
  size_t currentLength = contentLength;

  bool chunking = (chunkSize != 0);
  if (!chunking) {
    chunkSize = std::numeric_limits<size_t>::max();
  }
  while (currentLength > 0) {
    uint32_t toSend = std::min(currentLength, chunkSize);
    std::vector<uint8_t> buf;
    buf.resize(toSend, 'a');
    if (chunking) {
      codec.generateChunkHeader(estimateSizeBuf, txn, toSend);
    }
    codec.generateBody(estimateSizeBuf,
                       txn,
                       folly::IOBuf::copyBuffer(folly::range(buf)),
                       HTTPCodec::NoPadding,
                       false);
    if (chunking) {
      codec.generateChunkTerminator(estimateSizeBuf, txn);
    }
    currentLength -= toSend;
  }
  auto currentSize = estimateSizeBuf.chainLength();
  codec.generateEOM(estimateSizeBuf, txn);

  size_t eomSize = estimateSizeBuf.chainLength() - currentSize;
  size_t estimatedSize = estimateSizeBuf.chainLength();
  return std::make_pair(estimatedSize, eomSize);
}

// estimateResponseSize only works for h1
TEST_P(HQDownstreamSessionTestH1q, PendingEomBuffered) {
  size_t contentLength = 100;
  size_t chunkSize = 5;

  auto reply = makeResponse(200);
  reply->setIsChunked(true);
  size_t estimatedSize = 0;
  size_t eomSize = 0;
  std::tie(estimatedSize, eomSize) =
      estimateResponseSize(*reply, contentLength, chunkSize);

  auto id = sendRequest();
  auto handler = addSimpleStrictHandler();
  handler->expectHeaders();
  handler->expectEOM([&handler, contentLength, chunkSize] {
    handler->sendChunkedReplyWithBody(200, contentLength, chunkSize, true);
  });

  // Initialize the flow control window to just less than the estimated size of
  // the eom codec which the codec generates..
  socketDriver_->setStreamFlowControlWindow(id, estimatedSize - eomSize);
  flushRequestsAndLoop();
  CHECK(eventBase_.loop());
  EXPECT_GE(socketDriver_->streams_[id].writeBuf.chainLength(),
            estimatedSize - eomSize);
  EXPECT_FALSE(socketDriver_->streams_[id].writeEOF);

  handler->expectDetachTransaction();
  socketDriver_->getSocket()->setStreamFlowControlWindow(id, estimatedSize);

  CHECK(eventBase_.loop());
  EXPECT_GE(socketDriver_->streams_[id].writeBuf.chainLength(), estimatedSize);
  EXPECT_TRUE(socketDriver_->streams_[id].writeEOF);
  hqSession_->closeWhenIdle();
}

// estimateResponseSize only works for h1
TEST_P(HQDownstreamSessionTestH1q, PendingEomQueuedNotFlushed) {
  auto reply = makeResponse(200);
  reply->setWantsKeepalive(true);
  reply->getHeaders().add(HTTP_HEADER_CONTENT_LENGTH,
                          folly::to<std::string>(1));
  size_t estimatedSize = 0;
  size_t eomSize = 0;
  std::tie(estimatedSize, eomSize) = estimateResponseSize(*reply, 1, 0);
  // There is no way to queue an EOM only anymore.  Add a body byte
  CHECK_EQ(eomSize, 0);
  eomSize = 1;

  auto id = sendRequest();
  auto handler = addSimpleStrictHandler();
  handler->expectHeaders();
  handler->expectEOM(
      [&handler] { handler->sendReplyWithBody(200, 1, true, true); });

  // Initialize the flow control window to just less than the estimated size of
  // the eom codec which the codec generates..
  socketDriver_->setStreamFlowControlWindow(id, estimatedSize - eomSize);
  handler->expectEgressPaused();
  flushRequestsAndLoop();
  CHECK(eventBase_.loop());
  EXPECT_GE(socketDriver_->streams_[id].writeBuf.chainLength(),
            estimatedSize - eomSize);
  EXPECT_FALSE(socketDriver_->streams_[id].writeEOF);

  handler->expectEgressResumed();
  handler->expectDetachTransaction();
  socketDriver_->getSocket()->setStreamFlowControlWindow(id, estimatedSize);

  CHECK(eventBase_.loop());
  EXPECT_GE(socketDriver_->streams_[id].writeBuf.chainLength(), estimatedSize);
  EXPECT_TRUE(socketDriver_->streams_[id].writeEOF);
  hqSession_->closeWhenIdle();
}

TEST_P(HQDownstreamSessionTest, SendEomLaterChunked) {
  size_t contentLength = 100;
  size_t chunkSize = 10;

  auto id = sendRequest();
  auto handler = addSimpleStrictHandler();
  handler->expectHeaders([&handler, contentLength, chunkSize] {
    handler->sendChunkedReplyWithBody(
        200, contentLength, chunkSize, false, false);
  });
  handler->expectEOM([&handler] { handler->sendEOM(); });
  handler->expectDetachTransaction();

  flushRequestsAndLoop();
  CHECK(eventBase_.loop());
  EXPECT_GE(socketDriver_->streams_[id].writeBuf.chainLength(), contentLength);
  EXPECT_TRUE(socketDriver_->streams_[id].writeEOF);
  hqSession_->closeWhenIdle();
}

TEST_P(HQDownstreamSessionTest, SendEomLater) {
  size_t contentLength = 100;
  auto id = sendRequest();
  auto handler = addSimpleStrictHandler();
  handler->expectHeaders([&handler, contentLength] {
    handler->sendHeaders(200, contentLength);
    handler->sendBody(contentLength);
  });
  handler->expectEOM([&handler] { handler->sendEOM(); });
  handler->expectDetachTransaction();

  flushRequestsAndLoop();
  CHECK(eventBase_.loop());
  EXPECT_GE(socketDriver_->streams_[id].writeBuf.chainLength(), contentLength);
  EXPECT_TRUE(socketDriver_->streams_[id].writeEOF);
  hqSession_->closeWhenIdle();
}

// Invoke notifyPendingShutdown, which will include an outgoing
// Connection: close header on the next outbound headers.  The next incoming
// request containing a Connection: close header will complete the drain state
// machine
// NOTE: this behavior is only valid for basic h1q
TEST_P(HQDownstreamSessionTestH1qv1, ShutdownNotify) {
  hqSession_->notifyPendingShutdown();
  EXPECT_FALSE(hqSession_->isReusable());
  auto idh1 = checkRequest();
  flushRequestsAndLoop();
  // we should write Connection: close in the outgoing headers
  auto resp =
      socketDriver_->streams_[idh1.first].writeBuf.move()->moveToFbString();
  EXPECT_TRUE(resp.find("Connection: close"));

  // Add connection: close
  auto req = getGetRequest();
  req.getHeaders().set(HTTP_HEADER_CONNECTION, "close");
  auto idh2 = checkRequest(req);
  flushRequestsAndLoop();
}

// closeWhenIdle on an idle conn - immediate delete
TEST_P(HQDownstreamSessionTest, ShutdownCloseIdle) {
  EXPECT_TRUE(hqSession_->isReusable());
  hqSession_->closeWhenIdle();
}

// closeWhenIdle invoked when a request is open, delete happens when it finishes
TEST_P(HQDownstreamSessionTest, ShutdownCloseIdleReq) {
  sendRequest();
  auto handler = addSimpleStrictHandler();
  handler->expectHeaders([this] {
    hqSession_->closeWhenIdle();
    EXPECT_TRUE(hqSession_->isClosing());
  });
  handler->expectEOM([&handler] { handler->sendReplyWithBody(200, 100); });
  handler->expectDetachTransaction();
  flushRequestsAndLoop();
}

// Peer initiates shutdown by sending Connection: close
// NOTE: this behavior is only valid for basic h1q
TEST_P(HQDownstreamSessionTestH1qv1, ShutdownFromPeer) {
  // client initiates shutdown by including Connection: close
  auto req = getGetRequest();
  req.getHeaders().set(HTTP_HEADER_CONNECTION, "close");
  auto idh = checkRequest(req);
  flushRequestsAndLoop();

  // session deleted when server emits connection: close
}

// dropConnection invoked while a request being processed, it receives an
// error
TEST_P(HQDownstreamSessionTest, ShutdownDropWithReq) {
  sendRequest();
  auto handler = addSimpleStrictHandler();
  handler->expectHeaders();
  handler->expectEOM();
  handler->expectError();
  handler->expectDetachTransaction();
  flushRequestsAndLoopN(1);
  hqSession_->dropConnection();
}

// dropConnection invoked while a request is partial, it receives an
// error from the transport
TEST_P(HQDownstreamSessionTest, ShutdownDropWithPartialReq) {
  sendRequest(getPostRequest(10), false);
  auto handler = addSimpleStrictHandler();
  handler->expectHeaders();
  handler->expectError();
  handler->expectDetachTransaction();
  flushRequestsAndLoopN(1);
  hqSession_->dropConnection();
}

// Call drop connection while there are bytes pending to egress
TEST_P(HQDownstreamSessionTest, DropConnectionPendingEgress) {
  // NOTE: this test assumes that dropConnection() gets called by the handler
  // before the session has the chance to write data.
  // This is not true anymore when there are control streams
  // So let's just loop a bit to give time to the Downstream Session to send the
  // control stream preface
  if (!IS_H1Q_FB_V1) {
    flushRequestsAndLoop();
  }

  sendRequest(getGetRequest());
  auto handler = addSimpleStrictHandler();
  handler->expectHeaders([&handler, this] {
    handler->sendReplyWithBody(200, 1);
    eventBase_.runInLoop([this] { hqSession_->dropConnection(); }, true);
  });
  handler->expectEOM();
  handler->expectError();
  handler->expectDetachTransaction();
  flushRequestsAndLoop();
}

TEST_P(HQDownstreamSessionTest, TestInfoCallbacks) {
  sendRequest();
  auto handler = addSimpleStrictHandler();
  handler->expectHeaders();
  handler->expectEOM([&handler] { handler->sendReplyWithBody(200, 100); });
  handler->expectDetachTransaction();
  EXPECT_CALL(infoCb_, onRequestBegin(_)).Times(1);
  EXPECT_CALL(infoCb_, onActivateConnection(_)).Times(1);
  EXPECT_CALL(infoCb_, onIngressMessage(_, _)).Times(1);
  EXPECT_CALL(infoCb_, onRead(_, _)).Times(AtLeast(2));
  EXPECT_CALL(infoCb_, onWrite(_, _)).Times(AtLeast(1));
  EXPECT_CALL(infoCb_, onDestroy(_)).Times(1);
  EXPECT_CALL(infoCb_, onRequestEnd(_, _)).Times(1);
  EXPECT_CALL(infoCb_, onDeactivateConnection(_)).Times(1);
  flushRequestsAndLoop();
  hqSession_->dropConnection();
}

TEST_P(HQDownstreamSessionTest, NotifyDropNoStreams) {
  hqSession_->notifyPendingShutdown();
  eventBase_.loop();
  // no need to explicitly drop in H1Q-V2
  if (IS_H1Q_FB_V1) {
    hqSession_->dropConnection();
  }
}

TEST_P(HQDownstreamSessionTest, ShutdownDropWithUnflushedResp) {
  auto id = sendRequest();
  // should be enough to trick HQSession into serializing the EOM into
  // HQStreamTransport but without enough to send it.
  socketDriver_->setStreamFlowControlWindow(id, 206);
  auto handler = addSimpleStrictHandler();
  handler->expectHeaders();
  handler->expectEOM([&handler] {
    handler->sendChunkedReplyWithBody(200, 100, 100, false, true);
  });
  handler->expectDetachTransaction();
  flushRequestsAndLoopN(1);
  hqSession_->dropConnection();
}

// rst_stream while a request is partial, terminate cleanly
TEST_P(HQDownstreamSessionTest, Cancel) {
  auto id = sendRequest(getPostRequest(10), false);
  auto handler = addSimpleStrictHandler();
  handler->expectHeaders([this, id] {
    socketDriver_->addReadError(id,
                                HTTP3::ErrorCode::HTTP_INTERNAL_ERROR,
                                std::chrono::milliseconds(0));
    hqSession_->closeWhenIdle();
  });
  handler->expectError();
  handler->expectDetachTransaction();
  flushRequestsAndLoop();
  EXPECT_EQ(*socketDriver_->streams_[id].error,
            HTTP3::ErrorCode::HTTP_NO_ERROR);
}

// read() returns a LocalErrorCode
TEST_P(HQDownstreamSessionTest, ReadErrorSync) {
  auto id = sendRequest(getPostRequest(10), false);
  auto handler = addSimpleStrictHandler();
  handler->expectHeaders([this, id] {
    // mark the stream in read error and trigger a readAvailable call
    socketDriver_->setReadError(id);
    // This is just to trigger readAvailable
    socketDriver_->addReadEvent(id, makeBuf(10), milliseconds(0));
    hqSession_->closeWhenIdle();
  });
  handler->expectError();
  handler->expectDetachTransaction();
  flushRequestsAndLoop();
}

// Connection dies in error with an open stream
TEST_P(HQDownstreamSessionTest, TransportErrorWithOpenStream) {
  sendRequest(getPostRequest(10), false);
  auto handler = addSimpleStrictHandler();
  handler->expectHeaders([this] {
    eventBase_.runInLoop([this] {
      // This should error out the stream first, then destroy the session
      socketDriver_->deliverConnectionError(
          std::make_pair(quic::TransportErrorCode::PROTOCOL_VIOLATION, ""));
    });
  });
  handler->expectError([](const HTTPException& ex) {
      EXPECT_EQ(ex.getProxygenError(), kErrorConnectionReset);
    });
  handler->expectDetachTransaction();
  flushRequestsAndLoop();
}

// writeChain() returns a LocalErrorCode with a half-closed stream
TEST_P(HQDownstreamSessionTest, WriteError) {
  auto id = sendRequest();
  auto handler = addSimpleStrictHandler();
  handler->expectHeaders();
  handler->expectEOM([&handler, this, id] {
    handler->sendHeaders(200, 100);
    socketDriver_->setWriteError(id);
    hqSession_->closeWhenIdle();
  });
  handler->expectError([](const HTTPException& ex) {
      EXPECT_EQ(ex.getProxygenError(), kErrorWrite);
    });
  handler->expectDetachTransaction();
  flushRequestsAndLoop();
}

// writeChain() returns a LocalErrorCode with stream open both ways
TEST_P(HQDownstreamSessionTest, WriteErrorPartialReq) {
  auto id = sendRequest(getPostRequest(10), false);
  auto handler = addSimpleStrictHandler();
  handler->expectHeaders([&handler, this, id] {
    handler->sendReplyWithBody(200, 100);
    socketDriver_->setWriteError(id);
    hqSession_->closeWhenIdle();
  });
  handler->expectError();
  handler->expectDetachTransaction();
  flushRequestsAndLoop();
}

// Test write on non writable stream
TEST_P(HQDownstreamSessionTest, WriteNonWritableStream) {
  auto idh = checkRequest();
  // delay the eof event so that we won't have to loop
  flushRequestsAndLoop(false, milliseconds(0), milliseconds(50), [&] {
    // Force the read in the loop, so that this will trigger a write.
    eventBase_.loop();
    socketDriver_->flowControlAccess_.clear();
  });
  // Once the eof is written and no more bytes remain, we should never
  // call flow control methods.
  EXPECT_EQ(socketDriver_->flowControlAccess_.count(idh.first), 0);
  hqSession_->closeWhenIdle();
}

TEST_P(HQDownstreamSessionTest, WriteErrorFlowControl) {
  auto id = sendRequest(getPostRequest(10), false);
  auto handler = addSimpleStrictHandler();
  handler->expectHeaders([&handler, this, id] {
    handler->sendReplyWithBody(200, 100);
    socketDriver_->forceStreamClose(id);
    hqSession_->closeWhenIdle();
  });
  handler->expectError();
  handler->expectDetachTransaction();
  flushRequestsAndLoop();
}

// Connection error on idle connection
TEST_P(HQDownstreamSessionTest, ConnectionErrorIdle) {
  socketDriver_->deliverConnectionError(
      std::make_pair(quic::TransportErrorCode::PROTOCOL_VIOLATION, ""));
  eventBase_.loopOnce();
}

// Connection End on an idle connection
TEST_P(HQDownstreamSessionTest, ConnectionEnd) {
  nextStreamId();
  socketDriver_->addOnConnectionEndEvent(10);
  CHECK(eventBase_.loop());
}

// invalid HTTP on stream before headers
// Might need an HQ test with unparseable junk?
TEST_P(HQDownstreamSessionTestH1q, BadHttp) {
  auto id = nextStreamId();
  auto buf = IOBuf::create(10);
  memset(buf->writableData(), 'a', 10);
  buf->append(10);
  testing::StrictMock<MockHTTPHandler> handler;
  EXPECT_CALL(getMockController(), getParseErrorHandler(_, _, _))
      .WillOnce(Return(&handler));
  EXPECT_CALL(handler, setTransaction(testing::_))
      .WillOnce(testing::SaveArg<0>(&handler.txn_));
  handler.expectError([&handler](const HTTPException& ex) {
    EXPECT_TRUE(ex.hasHttpStatusCode());
    handler.sendReplyWithBody(ex.getHttpStatusCode(), 100);
  });
  handler.expectDetachTransaction();
  socketDriver_->addReadEvent(id, std::move(buf), milliseconds(0));
  socketDriver_->addReadEOF(id);

  flushRequestsAndLoop();
  hqSession_->closeWhenIdle();
}

// Invalid HTTP headers
TEST_P(HQDownstreamSessionTestH1q, BadHttpHeaders) {
  auto id = nextStreamId();
  auto buf = IOBuf::copyBuffer("GET", 3);
  socketDriver_->addReadEvent(id, std::move(buf), milliseconds(0));
  socketDriver_->addReadEOF(id);
  testing::StrictMock<MockHTTPHandler> handler;
  EXPECT_CALL(getMockController(), getParseErrorHandler(_, _, _))
      .WillOnce(Return(&handler));
  EXPECT_CALL(handler, setTransaction(testing::_))
      .WillOnce(testing::SaveArg<0>(&handler.txn_));
  handler.expectError([&handler](const HTTPException& ex) {
    EXPECT_TRUE(ex.hasHttpStatusCode());
    handler.sendReplyWithBody(ex.getHttpStatusCode(), 100);
  });
  handler.expectDetachTransaction();

  flushRequestsAndLoop();
  hqSession_->closeWhenIdle();
}

TEST_P(HQDownstreamSessionTestHQ, BadHttpHeaders) {
  auto id = nextStreamId();
  std::array<uint8_t, 4> badHeaders{0x02, 0x01, 0x00, 0x81};
  auto buf = folly::IOBuf::copyBuffer(badHeaders.data(), badHeaders.size());
  socketDriver_->addReadEvent(id, std::move(buf), milliseconds(0));
  socketDriver_->addReadEOF(id);
  /* T35641532 -- Should QPACK errors be a session errors ?
  testing::StrictMock<MockHTTPHandler> handler;
  EXPECT_CALL(getMockController(), getParseErrorHandler(_, _, _))
      .WillOnce(Return(&handler));
  EXPECT_CALL(handler, setTransaction(testing::_))
      .WillOnce(testing::SaveArg<0>(&handler.txn_));
  handler.expectError();
  handler.expectDetachTransaction();
  */
  flushRequestsAndLoop();
  // The QPACK error will cause the connection to get dropped
}

// NOTE: this behavior is only valid for basic h1q
TEST_P(HQDownstreamSessionTestH1qv1, ShutdownWithTwoTxn) {
  sendRequest();
  auto req = getGetRequest();
  req.getHeaders().set(HTTP_HEADER_CONNECTION, "close");
  sendRequest(req);
  auto handler1 = addSimpleStrictHandler();
  auto handler2 = addSimpleStrictHandler();
  handler1->expectHeaders();
  handler1->expectEOM([&handler1] { handler1->sendReplyWithBody(200, 100); });
  handler1->expectDetachTransaction();
  handler2->expectHeaders();
  handler2->expectEOM([&handler2] { handler2->sendReplyWithBody(200, 100); });
  handler2->expectDetachTransaction();
  flushRequestsAndLoop();
}

TEST_P(HQDownstreamSessionTest, SendFinOnly) {
  HTTPMessage req;
  req.setMethod(HTTPMethod::GET);
  req.setHTTPVersion(0, 9);
  req.setURL("/");
  sendRequest(req);
  auto handler = addSimpleStrictHandler();
  handler->expectHeaders();
  handler->expectEOM([&handler] {
    HTTPMessage resp;
    resp.setStatusCode(200);
    resp.setHTTPVersion(0, 9);
    handler->txn_->sendHeaders(resp);
    handler->txn_->sendEOM();
  });
  handler->expectDetachTransaction();
  flushRequestsAndLoop();
  hqSession_->closeWhenIdle();
}

TEST_P(HQDownstreamSessionTest, PauseResume) {
  auto id = sendRequest(getPostRequest(10), false);
  auto& request = getStream(id);
  auto handler = addSimpleStrictHandler();
  handler->expectHeaders([&handler] { handler->txn_->pauseIngress(); });
  flushRequestsAndLoop();
  EXPECT_TRUE(socketDriver_->isStreamPaused(id));
  request.codec->generateBody(
      request.buf, request.id, makeBuf(10), HTTPCodec::NoPadding, true);
  request.readEOF = true;
  flushRequestsAndLoop();
  EXPECT_FALSE(socketDriver_->streams_[id].readBuf.empty());
  hqSession_->closeWhenIdle();

  // After resume, body and EOM delivered
  handler->expectBody();
  handler->expectEOM([&handler] { handler->sendReplyWithBody(200, 100); });
  handler->expectDetachTransaction();
  handler->txn_->resumeIngress();
  eventBase_.loop();
}

TEST_P(HQDownstreamSessionTest, EnqueuedAbort) {
  sendRequest();
  auto handler = addSimpleStrictHandler();
  handler->expectHeaders();
  handler->expectEOM([&handler] {
    handler->sendHeaders(200, 100);
    handler->txn_->sendBody(makeBuf(100));
    handler->txn_->sendAbort();
  });
  handler->expectDetachTransaction();
  flushRequestsAndLoop();
  hqSession_->closeWhenIdle();
}

TEST_P(HQDownstreamSessionTest, TransactionTimeout) {
  sendRequest(getPostRequest(10), false);
  auto handler = addSimpleStrictHandler();
  handler->expectHeaders([&handler] {
    // fire the timeout as soon as receiving the headers
    handler->txn_->setIdleTimeout(std::chrono::milliseconds(0));
  });
  handler->expectError([&handler](const HTTPException& ex) {
    EXPECT_FALSE(ex.hasHttpStatusCode());
    handler->terminate();
  });
  handler->expectDetachTransaction();
  flushRequestsAndLoop();
  hqSession_->closeWhenIdle();
}

TEST_P(HQDownstreamSessionTestH1q, ManagedTimeoutReadReset) {
  std::chrono::milliseconds connIdleTimeout{200};
  auto connManager = wangle::ConnectionManager::makeUnique(
      &eventBase_, connIdleTimeout, nullptr);
  connManager->addConnection(hqSession_, true);
  HQSession::DestructorGuard dg(hqSession_);
  auto handler = addSimpleStrictHandler();
  auto id = sendRequest(getPostRequest(10), false);
  auto& request = getStream(id);
  request.codec->generateBody(
      request.buf, request.id, makeBuf(3), HTTPCodec::NoPadding, true);
  request.readEOF = false;
  eventBase_.runAfterDelay(
      [&] {
        request.codec->generateBody(
            request.buf, request.id, makeBuf(3), HTTPCodec::NoPadding, true);
        request.readEOF = false;
        flushRequests();
      },
      100);
  eventBase_.runAfterDelay(
      [&] {
        EXPECT_NE(hqSession_->getConnectionCloseReason(),
                  ConnectionCloseReason::TIMEOUT);
        request.codec->generateBody(
            request.buf, request.id, makeBuf(4), HTTPCodec::NoPadding, true);
        request.readEOF = true;
        flushRequests();
      },
      250);
  handler->expectEOM([&handler] { handler->sendReplyWithBody(200, 100); });
  handler->expectHeaders();
  EXPECT_CALL(*handler, onBodyWithOffset(testing::_, testing::_)).Times(3);
  handler->expectDetachTransaction();
  flushRequestsAndLoop();
}

TEST_P(HQDownstreamSessionTestHQ, ManagedTimeoutUnidirectionalReadReset) {
  std::chrono::milliseconds connIdleTimeout{200};
  auto connManager = wangle::ConnectionManager::makeUnique(
      &eventBase_, connIdleTimeout, nullptr);
  connManager->addConnection(hqSession_, true);
  HQSession::DestructorGuard dg(hqSession_);

  // Just keep sending instructions to set the dynamic table capacity
  std::array<uint8_t, 1> data1{0b00100111};
  auto buf1 = folly::IOBuf::copyBuffer(data1.data(), data1.size());
  socketDriver_->addReadEvent(6, std::move(buf1), milliseconds(0));
  std::array<uint8_t, 1> data2{0b00100110};
  auto buf2 = folly::IOBuf::copyBuffer(data2.data(), data2.size());
  socketDriver_->addReadEvent(6, std::move(buf2), milliseconds(100));
  // Check that the session did not timeout, yet
  eventBase_.runAfterDelay(
      [&] {
        EXPECT_NE(hqSession_->getConnectionCloseReason(),
                  ConnectionCloseReason::TIMEOUT);
      },
      250);

  flushRequestsAndLoop();
}

TEST_P(HQDownstreamSessionTest, ManagedTimeoutActiveStreams) {
  std::chrono::milliseconds connIdleTimeout{300};
  auto connManager = wangle::ConnectionManager::makeUnique(
      &eventBase_, connIdleTimeout, nullptr);
  HQSession::DestructorGuard dg(hqSession_);
  sendRequest(getPostRequest(10), false);
  auto handler = addSimpleStrictHandler();
  connManager->addConnection(hqSession_, true);
  // Txn idle timer is > connIdleTimeout
  auto lastErrorTime = std::chrono::steady_clock::now();
  handler->expectHeaders([&handler] {
    handler->txn_->setIdleTimeout(std::chrono::milliseconds(500));
  });
  handler->expectError(
      [&handler, this, &lastErrorTime](const HTTPException& ex) {
        // we should txn timeout
        EXPECT_FALSE(ex.hasHttpStatusCode());
        EXPECT_EQ(ex.getProxygenError(), kErrorTimeout);
        EXPECT_TRUE(hqSession_->isScheduled());
        hqSession_->cancelTimeout();
        handler->terminate();
        lastErrorTime = std::chrono::steady_clock::now();
      });
  handler->expectDetachTransaction();
  flushRequestsAndLoop();
  auto now = std::chrono::steady_clock::now();
  EXPECT_GE(
      std::chrono::duration_cast<std::chrono::milliseconds>(now - lastErrorTime)
          .count(),
      connIdleTimeout.count());
  // Connection timeouts in the loop and closes.
  EXPECT_EQ(hqSession_->getConnectionCloseReason(),
            ConnectionCloseReason::TIMEOUT);
}

TEST_P(HQDownstreamSessionTest, ManagedTimeoutNoStreams) {
  std::chrono::milliseconds connIdleTimeout{300};
  auto connManager = wangle::ConnectionManager::makeUnique(
      &eventBase_, connIdleTimeout, nullptr);
  HQSession::DestructorGuard dg(hqSession_);
  connManager->addConnection(hqSession_, true);
  eventBase_.loop();
  EXPECT_EQ(hqSession_->getConnectionCloseReason(),
            ConnectionCloseReason::TIMEOUT);
}

// HQ can't do this case, because onMessageBegin is only called with full
// headers.
TEST_P(HQDownstreamSessionTestH1q, TransactionTimeoutNoHandler) {
  // test transaction timeout before receiving the full headers
  auto id = nextStreamId();
  auto res = requests_.emplace(std::piecewise_construct,
                               std::forward_as_tuple(id),
                               std::forward_as_tuple(makeCodec(id)));
  auto req = getGetRequest();
  auto& request = res.first->second;
  request.id = request.codec->createStream();
  request.codec->generateHeader(request.buf, request.id, req, false);
  // Send some bytes, but less than the whole headers, so that a stream gets
  // created but the handler does not get assigned
  request.buf.trimEnd(1);

  testing::StrictMock<MockHTTPHandler> handler;
  expectTransactionTimeout(handler);

  flushRequestsAndLoop();
  hqSession_->closeWhenIdle();
}

TEST_P(HQDownstreamSessionTest, TransactionTimeoutNoCodecId) {
  auto id = nextStreamId();
  auto res = requests_.emplace(std::piecewise_construct,
                               std::forward_as_tuple(id),
                               std::forward_as_tuple(makeCodec(id)));
  auto req = getGetRequest();
  auto& request = res.first->second;
  request.id = request.codec->createStream();
  request.codec->generateHeader(request.buf, request.id, req, false);
  // Send only a new line, so that onMessageBegin does not get called
  request.buf.split(request.buf.chainLength() - 1);
  testing::StrictMock<MockHTTPHandler> handler;
  expectTransactionTimeout(handler);
  flushRequestsAndLoop();
  hqSession_->closeWhenIdle();
}

TEST_P(HQDownstreamSessionTest, SendOnFlowControlPaused) {
  // 106 bytes of resp headers, 1 byte of body but 5 bytes of chunk overhead
  auto id = sendRequest();
  socketDriver_->setStreamFlowControlWindow(id, 100);
  auto handler = addSimpleStrictHandler();
  handler->expectHeaders();
  handler->expectEOM([&handler] {
    handler->sendHeaders(200, 100);
    handler->txn_->sendBody(makeBuf(100));
  });
  handler->expectEgressPaused([&handler] { handler->txn_->sendEOM(); });
  flushRequestsAndLoop();
  handler->expectEgressResumed();
  socketDriver_->setStreamFlowControlWindow(id, 100);
  handler->expectDetachTransaction();
  eventBase_.loop();
  hqSession_->closeWhenIdle();
}

TEST_P(HQDownstreamSessionTest, Http_100Continue) {
  auto req = getPostRequest(100);
  req.getHeaders().add(HTTP_HEADER_EXPECT, "100-continue");
  auto id = sendRequest(req, false);
  auto handler = addSimpleStrictHandler();
  handler->expectHeaders([&handler] {
    HTTPMessage continueResp;
    continueResp.setStatusCode(100);
    handler->txn_->sendHeaders(continueResp);
  });
  flushRequestsAndLoopN(1);
  auto& request = getStream(id);
  request.codec->generateBody(
      request.buf, request.id, makeBuf(100), HTTPCodec::NoPadding, true);
  request.readEOF = true;

  handler->expectBody();
  handler->expectEOM([&handler] { handler->sendReplyWithBody(200, 100); });
  handler->expectDetachTransaction();
  flushRequestsAndLoop();
  hqSession_->closeWhenIdle();
}

TEST_P(HQDownstreamSessionTest, ByteEvents) {
  sendRequest();
  auto handler = addSimpleStrictHandler();
  MockHTTPTransactionTransportCallback callback;
  handler->expectHeaders([&handler, &callback] {
    handler->txn_->setTransportCallback(&callback);
  });
  handler->expectEOM([&handler] { handler->sendReplyWithBody(200, 100); });
  handler->expectDetachTransaction();
  EXPECT_CALL(callback, headerBytesGenerated(_));
  EXPECT_CALL(callback, bodyBytesGenerated(_));
  EXPECT_CALL(callback, firstHeaderByteFlushed());
  EXPECT_CALL(callback, firstByteFlushed());
  EXPECT_CALL(callback, lastByteFlushed());
  EXPECT_CALL(callback, lastByteAcked(_));
  flushRequestsAndLoop();
  hqSession_->closeWhenIdle();
}

TEST_P(HQDownstreamSessionTest, LastByteEventZeroSize) {
  sendRequest();
  auto handler = addSimpleStrictHandler();
  MockHTTPTransactionTransportCallback callback;
  handler->expectHeaders([&handler, &callback] {
    handler->txn_->setTransportCallback(&callback);
  });
  handler->expectEOM([&handler] {
    handler->sendHeaders(200, 100);
    handler->txn_->sendBody(makeBuf(100));
  });
  EXPECT_CALL(callback, headerBytesGenerated(_));
  EXPECT_CALL(callback, bodyBytesGenerated(Ge(100))); // For HQ it's 103
  EXPECT_CALL(callback, firstHeaderByteFlushed());
  EXPECT_CALL(callback, firstByteFlushed());
  flushRequestsAndLoop();

  // Send the EOM, txn should not detach yet
  EXPECT_CALL(callback, bodyBytesGenerated(0));
  EXPECT_CALL(callback, lastByteFlushed());
  handler->txn_->sendEOM(); // 0 length EOM
  flushRequestsAndLoopN(1);

  // Let the delivery callback fire, now it can cleanup
  EXPECT_CALL(callback, lastByteAcked(_));
  handler->expectDetachTransaction();
  flushRequestsAndLoop();
  hqSession_->closeWhenIdle();
}

TEST_P(HQDownstreamSessionTest, DropWithByteEvents) {
  sendRequest();
  auto handler = addSimpleStrictHandler();
  MockHTTPTransactionTransportCallback callback;
  handler->expectHeaders([&handler, &callback] {
    handler->txn_->setTransportCallback(&callback);
  });
  handler->expectEOM([&handler] { handler->sendReplyWithBody(200, 100); });
  handler->expectDetachTransaction();
  EXPECT_CALL(callback, headerBytesGenerated(_));
  EXPECT_CALL(callback, bodyBytesGenerated(_));
  EXPECT_CALL(callback, firstHeaderByteFlushed());
  EXPECT_CALL(callback, firstByteFlushed());
  EXPECT_CALL(callback, lastByteFlushed());
  flushRequestsAndLoopN(1);
  hqSession_->dropConnection();
}

TEST_P(HQDownstreamSessionTest, TransportInfo) {
  wangle::TransportInfo transInfo;
  quic::QuicSocket::TransportInfo quicInfo;
  quicInfo.srtt = std::chrono::microseconds(135);
  quicInfo.rttvar = std::chrono::microseconds(246);
  quicInfo.writableBytes = 212;
  quicInfo.congestionWindow = 5 * quic::kDefaultUDPSendPacketLen;
  quicInfo.packetsRetransmitted = 513;
  quicInfo.timeoutBasedLoss = 90;
  quicInfo.pto = std::chrono::microseconds(34);
  quicInfo.bytesSent = 23;
  quicInfo.bytesRecvd = 123;
  quicInfo.ptoCount = 1;
  quicInfo.totalPTOCount = 2;
  EXPECT_CALL(*socketDriver_->getSocket(), getTransportInfo())
      .Times(3)
      .WillRepeatedly(Return(quicInfo));
  hqSession_->getCurrentTransportInfoWithoutUpdate(&transInfo);
  EXPECT_EQ(135, transInfo.rtt.count());
  EXPECT_EQ(246, transInfo.rtt_var);
  EXPECT_EQ(5, transInfo.cwnd);
  EXPECT_EQ(5 * quic::kDefaultUDPSendPacketLen, transInfo.cwndBytes);
  EXPECT_EQ(513, transInfo.rtx);
  EXPECT_EQ(90, transInfo.rtx_tm);
  EXPECT_EQ(34, transInfo.rto);
  EXPECT_EQ(23, transInfo.totalBytes);
  auto quicProtocolInfo =
      dynamic_cast<QuicProtocolInfo*>(transInfo.protocolInfo.get());
  EXPECT_EQ(0, quicProtocolInfo->ptoCount);
  EXPECT_EQ(0, quicProtocolInfo->totalPTOCount);
  EXPECT_EQ(0, quicProtocolInfo->totalTransportBytesSent);
  EXPECT_EQ(0, quicProtocolInfo->totalTransportBytesRecvd);
  hqSession_->getCurrentTransportInfo(&transInfo);
  EXPECT_EQ(1, quicProtocolInfo->ptoCount);
  EXPECT_EQ(2, quicProtocolInfo->totalPTOCount);
  EXPECT_EQ(23, quicProtocolInfo->totalTransportBytesSent);
  EXPECT_EQ(123, quicProtocolInfo->totalTransportBytesRecvd);
  hqSession_->dropConnection();
}

// Current Transport Info tests
TEST_P(HQDownstreamSessionTest, CurrentTransportInfo) {
  sendRequest();
  auto handler = addSimpleStrictHandler();
  MockHTTPTransactionTransportCallback callback;
  handler->expectHeaders([&handler, &callback] {
    handler->txn_->setTransportCallback(&callback);
  });

  QuicStreamProtocolInfo resultProtocolInfo;
  handler->expectEOM([&handler, &resultProtocolInfo] {
    wangle::TransportInfo transInfo;
    handler->txn_->getCurrentTransportInfo(&transInfo);
    auto quicStreamProtocolInfo =
        dynamic_cast<QuicStreamProtocolInfo*>(transInfo.protocolInfo.get());

    if (quicStreamProtocolInfo) {
      resultProtocolInfo.streamTransportInfo =
          quicStreamProtocolInfo->streamTransportInfo;
    }
  });

  handler->expectDetachTransaction();
  handler->expectError([&](const HTTPException& ex) {
      EXPECT_EQ(ex.getProxygenError(), kErrorDropped);
    });

  flushRequestsAndLoop();
  hqSession_->dropConnection();

  // The stream transport info field should be equal to
  // the mock object
  EXPECT_EQ(resultProtocolInfo.streamTransportInfo.totalHeadOfLineBlockedTime,
            streamTransInfo_.totalHeadOfLineBlockedTime);
  EXPECT_EQ(resultProtocolInfo.streamTransportInfo.holbCount,
            streamTransInfo_.holbCount);
  EXPECT_EQ(resultProtocolInfo.streamTransportInfo.isHolb,
            streamTransInfo_.isHolb);
}

TEST_P(HQDownstreamSessionTest, GetAddresses) {
  folly::SocketAddress localAddr("::", 65001);
  folly::SocketAddress remoteAddr("31.13.31.13", 3113);
  EXPECT_CALL(*socketDriver_->getSocket(), getLocalAddress())
      .WillRepeatedly(ReturnRef(localAddr));
  EXPECT_CALL(*socketDriver_->getSocket(), getPeerAddress())
      .WillRepeatedly(ReturnRef(remoteAddr));
  EXPECT_EQ(localAddr, hqSession_->getLocalAddress());
  EXPECT_EQ(remoteAddr, hqSession_->getPeerAddress());
  hqSession_->dropConnection();
}

TEST_P(HQDownstreamSessionTest, GetAddressesFromBase) {
  HTTPSessionBase* sessionBase = dynamic_cast<HTTPSessionBase*>(hqSession_);
  EXPECT_EQ(localAddress_, sessionBase->getLocalAddress());
  EXPECT_EQ(localAddress_, sessionBase->getLocalAddress());
  hqSession_->dropConnection();
}

TEST_P(HQDownstreamSessionTest, GetAddressesAfterDropConnection) {
  HQSession::DestructorGuard dg(hqSession_);
  hqSession_->dropConnection();
  EXPECT_EQ(localAddress_, hqSession_->getLocalAddress());
  EXPECT_EQ(peerAddress_, hqSession_->getPeerAddress());
}

TEST_P(HQDownstreamSessionTest, RstCancelled) {
  auto id = nextStreamId();
  auto buf = IOBuf::create(3);
  memcpy(buf->writableData(), "GET", 3);
  buf->append(3);
  socketDriver_->addReadEvent(id, std::move(buf), milliseconds(0));
  flushRequestsAndLoopN(1);
  socketDriver_->addReadError(id,
                              HTTP3::ErrorCode::HTTP_REQUEST_CANCELLED,
                              std::chrono::milliseconds(0));
  hqSession_->closeWhenIdle();
  flushRequestsAndLoop();
  EXPECT_EQ(*socketDriver_->streams_[id].error,
            HTTP3::ErrorCode::HTTP_REQUEST_REJECTED);
}

TEST_P(HQDownstreamSessionTest, LocalErrQueuedEgress) {
  sendRequest(getPostRequest(10), false);
  auto handler = addSimpleStrictHandler();
  handler->expectHeaders([&handler, this] {
    socketDriver_->setStreamFlowControlWindow(0, 0);
    socketDriver_->setConnectionFlowControlWindow(0);
    handler->sendHeaders(200, 65536 * 2);
    handler->sendBody(65536 * 2);
  });
  handler->expectEgressPaused();
  flushRequestsAndLoopN(2);
  handler->expectError([](const HTTPException& ex) {
      EXPECT_EQ(ex.getProxygenError(), kErrorShutdown);
    });
  handler->expectDetachTransaction();
  socketDriver_->deliverConnectionError(
      std::make_pair(quic::LocalErrorCode::CONNECTION_RESET, ""));
  flushRequestsAndLoop();
}

// Just open a stream and send nothing
TEST_P(HQDownstreamSessionTest, zeroBytes) {
  auto id = nextStreamId();
  socketDriver_->addReadEvent(
      id, folly::IOBuf::copyBuffer("", 0), milliseconds(0));
  testing::StrictMock<MockHTTPHandler> handler;
  expectTransactionTimeout(handler);
  eventBase_.loop();
  hqSession_->closeWhenIdle();
}

// For HQ, send an incomplete frame header
TEST_P(HQDownstreamSessionTestHQ, oneByte) {
  auto id = nextStreamId();
  socketDriver_->addReadEvent(
      id, folly::IOBuf::copyBuffer("", 1), milliseconds(0));
  testing::StrictMock<MockHTTPHandler> handler;
  expectTransactionTimeout(handler);
  eventBase_.loop();
  hqSession_->closeWhenIdle();
}

TEST_P(HQDownstreamSessionTestH1qv2HQ, TestGoawayID) {
  // This test check that unidirectional stream IDs are not accounted for
  // in the Goaway Max Stream ID
  auto req = getGetRequest();
  // Explicitly skip some stream IDs to simulate out of order delivery
  sendRequest(req, true, 4);
  auto handler = addSimpleStrictHandler();
  handler->expectHeaders();
  handler->expectEOM([hdlr = handler.get()] {
    // Delay sending EOM so the streams are active when draining
    hdlr->sendReplyWithBody(200, 100, true, false);
  });
  handler->expectDetachTransaction();
  flushRequestsAndLoopN(1);
  hqSession_->closeWhenIdle();
  // Give it some time to send the two goaways and receive the delivery callback
  flushRequestsAndLoopN(3);
  EXPECT_EQ(httpCallbacks_.goaways, 2);
  EXPECT_THAT(httpCallbacks_.goawayStreamIds,
              ElementsAre(quic::kEightByteLimit, 4));
  handler->sendEOM();
  flushRequestsAndLoop();
}

TEST_P(HQDownstreamSessionTestH1qv2HQ, TestGetGoaway) {
  std::vector<std::unique_ptr<StrictMock<MockHTTPHandler>>> handlers;
  auto numStreams = 3;
  for (auto n = 1; n <= numStreams; n++) {
    auto req = getGetRequest();
    // Explicitly skip some stream IDs to simulate out of order delivery
    sendRequest(req, true, n * 8);
    handlers.emplace_back(addSimpleStrictHandler());
    auto handler = handlers.back().get();
    handler->expectHeaders();
    handler->expectEOM([hdlr = handler] {
      // Delay sending EOM so the streams are active when draining
      hdlr->sendReplyWithBody(200, 100, true, false);
    });
    handler->expectDetachTransaction();
  }
  flushRequestsAndLoopN(1);
  hqSession_->closeWhenIdle();
  // Give it some time to send the two goaways and receive the delivery callback
  flushRequestsAndLoopN(3);
  EXPECT_EQ(httpCallbacks_.goaways, 2);
  EXPECT_THAT(httpCallbacks_.goawayStreamIds,
              ElementsAre(quic::kEightByteLimit, numStreams * 8));

  // Check that a new stream with id > lastStreamId gets rejected
  auto errReq = getGetRequest();
  quic::StreamId errStreamId = numStreams * 8 + 4;
  sendRequest(errReq, true, errStreamId);
  flushRequestsAndLoopN(1);
  auto& errStream = socketDriver_->streams_[errStreamId];
  EXPECT_EQ(errStream.writeState, MockQuicSocketDriver::StateEnum::ERROR);
  EXPECT_TRUE(errStream.error == HTTP3::ErrorCode::HTTP_REQUEST_REJECTED);

  // Check that a new stream with id <= lastStreamId is instead just fine
  auto okReq = getGetRequest();
  sendRequest(okReq, true, numStreams * 8 - 4);
  auto okHandler = addSimpleStrictHandler();
  okHandler->expectHeaders();
  okHandler->expectEOM([&] { okHandler->sendReplyWithBody(200, 100); });
  okHandler->expectDetachTransaction();
  flushRequestsAndLoopN(1);

  // now send response EOM on the pending transactions, to finish shutdown
  for (auto& handler : handlers) {
    handler->sendEOM();
  }
  flushRequestsAndLoop();
}

TEST_P(HQDownstreamSessionTestHQ, DelayedQPACK) {
  auto req = getGetRequest();
  req.getHeaders().add("X-FB-Debug", "rfccffgvtvnenjkbtitkfdufddnvbecu");
  auto id = sendRequest(req);
  auto handler = addSimpleStrictHandler();
  handler->expectHeaders();
  handler->expectEOM(
      [hdlr = handler.get()] { hdlr->sendReplyWithBody(200, 100); });
  handler->expectDetachTransaction();

  auto controlStream = encoderWriteBuf_.move();
  flushRequestsAndLoopN(1);
  encoderWriteBuf_.append(std::move(controlStream));
  flushRequestsAndLoop();
  EXPECT_GT(socketDriver_->streams_[id].writeBuf.chainLength(), 110);
  EXPECT_TRUE(socketDriver_->streams_[id].writeEOF);
  hqSession_->closeWhenIdle();
}

TEST_P(HQDownstreamSessionTestHQ, cancelQPACK) {
  auto req = getGetRequest();
  req.getHeaders().add("X-FB-Debug", "rfccffgvtvnenjkbtitkfdufddnvbecu");
  auto id = sendRequest(req);
  auto& request = getStream(id);
  // discard part of request, header won't get qpack-ack'd
  request.buf.trimEnd(request.buf.chainLength() - 3);
  request.readEOF = false;
  flushRequestsAndLoopN(1);
  socketDriver_->addReadError(id,
                              HTTP3::ErrorCode::HTTP_REQUEST_CANCELLED,
                              std::chrono::milliseconds(0));
  hqSession_->closeWhenIdle();
  flushRequestsAndLoop();
  // this will evict all headers, which is only legal if the cancellation is
  // emitted and processed.
  qpackCodec_.setEncoderHeaderTableSize(0);
  EXPECT_EQ(*socketDriver_->streams_[id].error,
            HTTP3::ErrorCode::HTTP_REQUEST_REJECTED);
  eventBase_.loopOnce();
}

TEST_P(HQDownstreamSessionTestHQ, DelayedQPACKCanceled) {
  auto req = getGetRequest();
  req.getHeaders().add("X-FB-Debug", "rfccffgvtvnenjkbtitkfdufddnvbecu");
  auto id = sendRequest(req);
  // This request never gets a handler

  auto controlStream = encoderWriteBuf_.move();
  // receive header block with unsatisfied dep
  flushRequestsAndLoopN(1);

  // cancel this request
  socketDriver_->addReadError(id,
                              HTTP3::ErrorCode::HTTP_REQUEST_CANCELLED,
                              std::chrono::milliseconds(0));
  flushRequestsAndLoopN(1);

  // Now send the dependency
  encoderWriteBuf_.append(std::move(controlStream));
  flushRequestsAndLoop();

  // This used to crash
  hqSession_->closeWhenIdle();
}

TEST_P(HQDownstreamSessionTestHQ, DelayedQPACKTimeout) {
  auto req = getPostRequest(10);
  req.getHeaders().add("X-FB-Debug", "rfccffgvtvnenjkbtitkfdufddnvbecu");
  auto id = sendRequest(req, false);
  auto& request = getStream(id);
  folly::IOBufQueue reqTail(folly::IOBufQueue::cacheChainLength());
  reqTail.append(request.buf.move());
  request.buf.append(reqTail.split(reqTail.chainLength() / 2));
  // reqTail now has the second half of request

  flushRequests();
  testing::StrictMock<MockHTTPHandler> handler;
  expectTransactionTimeout(handler, [&] {
    request.buf.append(reqTail.move());
    auto body = folly::IOBuf::wrapBuffer("\3\3\3\3\3\3\3\3\3\3", 10);
    request.codec->generateBody(
        request.buf, request.id, std::move(body), HTTPCodec::NoPadding, true);
    flushRequests();
  });
  eventBase_.loop();
  EXPECT_GT(socketDriver_->streams_[id].writeBuf.chainLength(), 110);
  EXPECT_TRUE(socketDriver_->streams_[id].writeEOF);
  hqSession_->closeWhenIdle();
}

TEST_P(HQDownstreamSessionTestHQ, QPACKEncoderLimited) {
  auto req = getGetRequest();
  socketDriver_->getSocket()->setStreamFlowControlWindow(
      kQPACKEncoderEgressStreamId, 10);
  auto id = sendRequest(req);
  auto handler = addSimpleStrictHandler();
  handler->expectHeaders();
  handler->expectEOM([hdlr = handler.get()] {
    HTTPMessage resp;
    resp.setStatusCode(200);
    resp.getHeaders().add("X-FB-Debug", "rfccffgvtvnenjkbtitkfdufddnvbecu");
    hdlr->txn_->sendHeaders(resp);
    hdlr->txn_->sendEOM();
  });
  handler->expectDetachTransaction();
  flushRequestsAndLoop();

  // QPACK will attempt to index the header, but cannot reference it because
  // it runs out of stream flow control
  EXPECT_GT(socketDriver_->streams_[id].writeBuf.chainLength(), 30);
  hqSession_->closeWhenIdle();
}

TEST_P(HQDownstreamSessionTestHQ, DelayedQPACKStopSendingReset) {
  auto req = getGetRequest();
  req.getHeaders().add("X-FB-Debug", "rfccffgvtvnenjkbtitkfdufddnvbecu");
  auto id = sendRequest(req);
  // This request never gets a handler

  auto controlStream = encoderWriteBuf_.move();
  // receive header block with unsatisfied dep
  flushRequestsAndLoopN(1);

  // cancel this request
  socketDriver_->addStopSending(id, HTTP3::ErrorCode::HTTP_REQUEST_CANCELLED);
  socketDriver_->addReadError(id,
                             HTTP3::ErrorCode::HTTP_REQUEST_CANCELLED,
                             std::chrono::milliseconds(0));
  flushRequestsAndLoopN(1);

  // Now send the dependency
  encoderWriteBuf_.append(std::move(controlStream));
  flushRequestsAndLoop();

  // This used to crash
  hqSession_->closeWhenIdle();
}

TEST_P(HQDownstreamSessionBeforeTransportReadyTest, NotifyPendingShutdown) {
  hqSession_->notifyPendingShutdown();
  SetUpOnTransportReady();
  // Give it some time to send the two goaways and receive the delivery callback
  flushRequestsAndLoopN(3);
  if (IS_HQ) {
    // There is a check for this already for all the tests, but adding this to
    // make it explicit that SETTINGS should be sent before GOAWAY even in this
    // corner case, otherwise the peer will error out the session
    EXPECT_EQ(httpCallbacks_.settings, 1);
  }
  EXPECT_EQ(httpCallbacks_.goaways, 2);
  EXPECT_THAT(httpCallbacks_.goawayStreamIds,
              ElementsAre(quic::kEightByteLimit, 0));
}

// NOTE: a failure for this test may cause an infinite loop in processReadData
TEST_P(HQDownstreamSessionTest, ProcessReadDataOnDetachedStream) {
  auto id = sendRequest("/", 0, false);
  auto handler = addSimpleStrictHandler();
  handler->expectHeaders([&] {
    eventBase_.runAfterDelay(
        [&] {
          // schedule a few events to run in the eventbase back-to-back
          // call readAvailable with just the EOF
          auto& stream = socketDriver_->streams_[id];
          CHECK(!stream.readEOF);
          stream.readEOF = true;
          CHECK(stream.readCB);
          stream.readCB->readAvailable(id);
          // now send an error so that the stream gets marked for detach
          stream.readCB->readError(
              id, std::make_pair(HTTP3::ErrorCode::HTTP_NO_ERROR, folly::none));
          // then closeWhenIdle (like during shutdown), this calls
          // checkForShutdown that calls checkForDetach and may detach a
          // transaction that was added to the pendingProcessReadSet in the same
          // loop
          hqSession_->closeWhenIdle();
        },
        10);
  });
  flushRequestsAndLoopN(1);

  handler->expectError();
  handler->expectDetachTransaction();

  flushRequestsAndLoop();
}

// Test Cases for which Settings are not sent in the test SetUp
using HQDownstreamSessionTestHQNoSettings = HQDownstreamSessionTest;
INSTANTIATE_TEST_CASE_P(HQDownstreamSessionTest,
                        HQDownstreamSessionTestHQNoSettings,
                        Values(TestParams({.alpn_ = "h3",
                                           .shouldSendSettings_ = false})),
                        paramsToTestName);
TEST_P(HQDownstreamSessionTestHQNoSettings, SimpleGet) {
  auto idh = checkRequest();
  flushRequestsAndLoop();
  EXPECT_GT(socketDriver_->streams_[idh.first].writeBuf.chainLength(), 110);
  EXPECT_TRUE(socketDriver_->streams_[idh.first].writeEOF);
  // Checks that the server response is sent without the QPACK dynamic table
  CHECK_EQ(qpackCodec_.getCompressionInfo().ingressHeaderTableSize_, 0);

  // TODO: Check that QPACK does not use the dynamic table for the response
  hqSession_->closeWhenIdle();
}

// This test is checking two different scenarios for different protocol
//   - in HQ we already have sent SETTINGS in SetUp, so tests that multiple
//     setting frames are not allowed
//   - in h1q-fb-v2 tests that receiving even a single SETTINGS frame errors
//     out the connection
TEST_P(HQDownstreamSessionTestH1qv2HQ, ExtraSettings) {
  sendRequest();
  auto handler = addSimpleStrictHandler();
  handler->expectHeaders();
  handler->expectEOM();
  handler->expectError([&](const HTTPException& ex) {
      EXPECT_EQ(ex.getProxygenError(), kErrorConnection);
    });
  handler->expectDetachTransaction();
  flushRequestsAndLoopN(1);

  // Need to use a new codec. Since generating settings twice is
  // forbidden
  HQControlCodec auxControlCodec_{0x0003,
                                  TransportDirection::UPSTREAM,
                                  StreamDirection::EGRESS,
                                  egressSettings_};
  folly::IOBufQueue writeBuf{folly::IOBufQueue::cacheChainLength()};
  auxControlCodec_.generateSettings(writeBuf);
  socketDriver_->addReadEvent(
      connControlStreamId_, writeBuf.move(), milliseconds(0));

  flushRequestsAndLoop();

  EXPECT_EQ(*socketDriver_->streams_[kConnectionStreamId].error,
            HTTP3::ErrorCode::HTTP_UNEXPECTED_FRAME);
}

using HQDownstreamSessionDeathTestH1qv2HQ = HQDownstreamSessionTestH1qv2HQ;
TEST_P(HQDownstreamSessionDeathTestH1qv2HQ, WriteExtraSettings) {
  EXPECT_EXIT(sendSettings(),
              ::testing::KilledBySignal(SIGABRT),
              "Check failed: !sentSettings_");
}

TEST_P(HQDownstreamSessionTest, httpPausedBuffered) {
  IOBufQueue rst{IOBufQueue::cacheChainLength()};
  auto id1 = sendRequest();

  InSequence handlerSequence;
  auto handler1 = addSimpleStrictHandler();
  handler1->expectHeaders();
  handler1->expectEOM([&handler1, this] {
    socketDriver_->setConnectionFlowControlWindow(0);
    handler1->sendHeaders(200, 65536 * 2);
    handler1->sendBody(65536 * 2);
  });
  handler1->expectEgressPaused();
  flushRequestsAndLoop();

  sendRequest();
  auto handler2 = addSimpleStrictHandler();
  handler2->expectEgressPaused();
  handler2->expectHeaders();
  handler2->expectEOM([&] {
    eventBase_.runInLoop([&] {
      socketDriver_->addReadError(id1,
                                  HTTP3::ErrorCode::HTTP_INTERNAL_ERROR,
                                  std::chrono::milliseconds(0));
    });
  });
  handler1->expectError([&](const HTTPException& ex) {
    EXPECT_EQ(ex.getProxygenError(), kErrorStreamAbort);
    eventBase_.runInLoop([this] {
      socketDriver_->setConnectionFlowControlWindow(65536 * 2 + 1000);
    });
  });
  handler1->expectDetachTransaction();
  handler2->expectEgressResumed(
      [&] { handler2->sendReplyWithBody(200, 32768); });
  handler2->expectDetachTransaction();
  flushRequestsAndLoop();
  hqSession_->closeWhenIdle();
}

TEST_P(HQDownstreamSessionTestH1q, httpPausedBufferedDetach) {
  IOBufQueue rst{IOBufQueue::cacheChainLength()};
  auto id1 = sendRequest();

  InSequence handlerSequence;
  auto handler1 = addSimpleStrictHandler();
  handler1->expectHeaders();
  handler1->expectEOM([&handler1, this, id1] {
    socketDriver_->setStreamFlowControlWindow(id1, 199);
    handler1->sendHeaders(200, 100);
    handler1->sendBody(100);
    eventBase_.runInLoop([&handler1] {
      handler1->expectDetachTransaction();
      handler1->sendEOM();
    });
  });
  handler1->expectEgressPaused();
  flushRequestsAndLoop();

  hqSession_->dropConnection();
}

TEST_P(HQDownstreamSessionTest, onErrorEmptyEnqueued) {
  IOBufQueue rst{IOBufQueue::cacheChainLength()};
  auto id1 = sendRequest();

  InSequence handlerSequence;
  auto handler1 = addSimpleStrictHandler();
  handler1->expectHeaders();
  handler1->expectEOM([&handler1, this, id1] {
    handler1->sendHeaders(200, 100);
    socketDriver_->setStreamFlowControlWindow(id1, 100);
    // After one loop, it will become stream flow control blocked, and txn
    // will think it is enqueued, but session will not.
    handler1->expectEgressPaused();
    handler1->sendBody(101);
    handler1->sendEOM();
    eventBase_.runInLoop([&handler1, this, id1] {
      handler1->expectError();
      handler1->expectDetachTransaction();
      socketDriver_->addReadError(id1,
                                  HTTP3::ErrorCode::HTTP_INTERNAL_ERROR,
                                  std::chrono::milliseconds(0));
    });
  });
  flushRequestsAndLoop();

  hqSession_->closeWhenIdle();
}

TEST_P(HQDownstreamSessionTest, dropWhilePaused) {
  IOBufQueue rst{IOBufQueue::cacheChainLength()};
  sendRequest();

  InSequence handlerSequence;
  auto handler1 = addSimpleStrictHandler();
  handler1->expectHeaders();
  handler1->expectEOM([&handler1, this] {
    // pause writes
    socketDriver_->setConnectionFlowControlWindow(0);
    // fill session buffer
    handler1->sendReplyWithBody(200, hqSession_->getWriteBufferLimit());
  });
  flushRequestsAndLoop();

  handler1->expectError([&](const HTTPException& ex) {
      EXPECT_EQ(ex.getProxygenError(), kErrorDropped);
    });
  handler1->expectDetachTransaction();
  hqSession_->dropConnection();
}

TEST_P(HQDownstreamSessionTestH1qv2HQ,
       StopSendingOnUnknownUnidirectionalStreams) {
  auto greaseStreamId = nextUnidirectionalStreamId();
  createControlStream(socketDriver_.get(),
                      greaseStreamId,
                      proxygen::hq::UnidirectionalStreamType(
                          *getGreaseId(folly::Random::rand32(16))));
  auto idh = checkRequest();
  flushRequestsAndLoop();

  EXPECT_EQ(*socketDriver_->streams_[greaseStreamId].error,
            HTTP3::ErrorCode::HTTP_UNKNOWN_STREAM_TYPE);
  // Also check that the request completes correctly
  EXPECT_GT(socketDriver_->streams_[idh.first].writeBuf.chainLength(), 110);
  EXPECT_TRUE(socketDriver_->streams_[idh.first].writeEOF);
  if (IS_HQ) {
    // Checks that the server response is sent using the QPACK dynamic table
    CHECK_GE(qpackCodec_.getCompressionInfo().ingressHeaderTableSize_, 0);
  }
  hqSession_->closeWhenIdle();
}

TEST_P(HQDownstreamSessionTestH1qv2HQ, eofControlStream) {
  sendRequest();

  InSequence handlerSequence;
  auto handler = addSimpleStrictHandler();
  handler->expectHeaders();
  handler->expectEOM();
  handler->expectError([&](const HTTPException& ex) {
      EXPECT_EQ(ex.getProxygenError(), kErrorConnection);
    });
  handler->expectDetachTransaction();
  flushRequestsAndLoopN(1);
  socketDriver_->addReadEOF(connControlStreamId_);
  flushRequestsAndLoop();
}

TEST_P(HQDownstreamSessionTestH1qv2HQ, resetControlStream) {
  sendRequest();

  InSequence handlerSequence;
  auto handler = addSimpleStrictHandler();
  handler->expectHeaders();
  handler->expectEOM();
  handler->expectError([&](const HTTPException& ex) {
      EXPECT_EQ(ex.getProxygenError(), kErrorConnection);
    });
  handler->expectDetachTransaction();
  flushRequestsAndLoopN(1);
  socketDriver_->addReadError(connControlStreamId_,
                             HTTP3::ErrorCode::HTTP_INTERNAL_ERROR);
  flushRequestsAndLoop();
  EXPECT_EQ(*socketDriver_->streams_[kConnectionStreamId].error,
            HTTP3::ErrorCode::HTTP_CLOSED_CRITICAL_STREAM);
}

TEST_P(HQDownstreamSessionTestHQ, controlStreamWriteError) {
  sendRequest();

  InSequence handlerSequence;
  auto handler = addSimpleStrictHandler();
  handler->expectHeaders();
  handler->expectEOM([&handler] {
      handler->sendHeaders(200, 100);
    });
  handler->expectError([&](const HTTPException& ex) {
      EXPECT_EQ(ex.getProxygenError(), kErrorWrite);
    });
  handler->expectDetachTransaction();
  socketDriver_->setWriteError(kQPACKEncoderEgressStreamId);
  flushRequestsAndLoop();
  EXPECT_EQ(*socketDriver_->streams_[kConnectionStreamId].error,
            HTTP3::ErrorCode::HTTP_CLOSED_CRITICAL_STREAM);
}

/**
 * Instantiate the Parametrized test cases
 */

// Make sure all the tests keep working with all the supported protocol versions
INSTANTIATE_TEST_CASE_P(
    HQDownstreamSessionTest,
    HQDownstreamSessionTest,
    Values(TestParams({.alpn_ = "h1q-fb"}),
           TestParams({.alpn_ = "h1q-fb-v2"}),
           TestParams({.alpn_ = "h3"}),
           TestParams({.alpn_ = "h3",
                       .prParams =
                           PartiallyReliableTestParams{
                               .bodyScript = std::vector<uint8_t>(),
                           }})),
    paramsToTestName);

// Instantiate h1q only tests that work on all versions
INSTANTIATE_TEST_CASE_P(HQDownstreamSessionTest,
                        HQDownstreamSessionTestH1q,
                        Values(TestParams({.alpn_ = "h1q-fb"}),
                               TestParams({.alpn_ = "h1q-fb-v2"})),
                        paramsToTestName);

// Instantiate common tests for h1q-fb-v2 and hq (goaway)
INSTANTIATE_TEST_CASE_P(HQDownstreamSessionTest,
                        HQDownstreamSessionTestH1qv2HQ,
                        Values(TestParams({.alpn_ = "h1q-fb-v2"}),
                               TestParams({.alpn_ = "h3"})),
                        paramsToTestName);

INSTANTIATE_TEST_CASE_P(HQDownstreamSessionBeforeTransportReadyTest,
                        HQDownstreamSessionBeforeTransportReadyTest,
                        Values(TestParams({.alpn_ = "h1q-fb-v2"}),
                               TestParams({.alpn_ = "h3"})),
                        paramsToTestName);

// Instantiate h1q-fb-v1 only tests
INSTANTIATE_TEST_CASE_P(HQDownstreamSessionTest,
                        HQDownstreamSessionTestH1qv1,
                        Values(TestParams({.alpn_ = "h1q-fb"})),
                        paramsToTestName);

// Instantiate h1q-fb-v2 only tests
INSTANTIATE_TEST_CASE_P(HQDownstreamSessionTest,
                        HQDownstreamSessionTestH1qv2,
                        Values(TestParams({.alpn_ = "h1q-fb-v2"})),
                        paramsToTestName);

// Instantiate hq only tests
INSTANTIATE_TEST_CASE_P(
    HQDownstreamSessionTest,
    HQDownstreamSessionTestHQ,
    Values(TestParams({.alpn_ = "h3"}),
           TestParams({.alpn_ = "h3",
                       .prParams =
                           PartiallyReliableTestParams{
                               .bodyScript = std::vector<uint8_t>(),
                           }})),
    paramsToTestName);

using DropConnectionInTransportReadyTest =
    HQDownstreamSessionBeforeTransportReadyTest;

INSTANTIATE_TEST_CASE_P(DropConnectionInTransportReadyTest,
                        DropConnectionInTransportReadyTest,
                        Values(TestParams({.alpn_ = "unsupported"}),
                               TestParams({.alpn_ = "h3",
                                           .unidirectionalStreamsCredit = 1}),
                               TestParams({.alpn_ = "h1q-fb-v2",
                                           .unidirectionalStreamsCredit = 0})),
                        paramsToTestName);
// Instantiate hq server push tests
INSTANTIATE_TEST_CASE_P(HQDownstreamSessionTest,
                        HQDownstreamSessionTestHQPush,
                        Values(TestParams({.alpn_ = "h3",
                                           .unidirectionalStreamsCredit = 8})),
                        paramsToTestName);

// Use this test class for mismatched alpn tests
class HQDownstreamSessionTestUnsupportedAlpn : public HQDownstreamSessionTest {
 public:
  void SetUp() override {
    SetUpBase();
  }
};

TEST_P(DropConnectionInTransportReadyTest, TransportReadyFailure) {
  HQDownstreamSession::DestructorGuard dg(hqSession_);
  EXPECT_CALL(infoCb_, onTransportReady(_)).Times(0);
  EXPECT_CALL(infoCb_, onConnectionError(_))
      .WillOnce(Invoke([](const HTTPSessionBase& session) {
        const auto hqSession = dynamic_cast<const HQSession*>(&session);
        ASSERT_NE(hqSession, nullptr);
        ASSERT_NE(hqSession->getQuicSocket(), nullptr);
      }));
  SetUpOnTransportReady();
  EXPECT_EQ(hqSession_->getQuicSocket(), nullptr);
}

// Instantiate hq PR only tests
INSTANTIATE_TEST_CASE_P(
    HQDownstreamSessionTest,
    HQDownstreamSessionTestHQPR,
    Values(TestParams({.alpn_ = "h3",
                       .prParams =
                           PartiallyReliableTestParams{
                               .bodyScript = std::vector<uint8_t>({PR_BODY}),
                           }}),
           TestParams({.alpn_ = "h3",
                       .prParams =
                           PartiallyReliableTestParams{
                               .bodyScript = std::vector<uint8_t>({PR_SKIP}),
                           }}),
           TestParams({.alpn_ = "h3",
                       .prParams =
                           PartiallyReliableTestParams{
                               .bodyScript = std::vector<uint8_t>({PR_BODY,
                                                                   PR_SKIP}),
                           }}),
           TestParams({.alpn_ = "h3",
                       .prParams =
                           PartiallyReliableTestParams{
                               .bodyScript = std::vector<uint8_t>({PR_SKIP,
                                                                   PR_BODY}),
                           }}),
           TestParams({.alpn_ = "h3",
                       .prParams =
                           PartiallyReliableTestParams{
                               .bodyScript = std::vector<uint8_t>(
                                   {PR_SKIP, PR_SKIP, PR_BODY, PR_SKIP}),
                           }}),
           TestParams({.alpn_ = "h3",
                       .prParams =
                           PartiallyReliableTestParams{
                               .bodyScript = std::vector<uint8_t>(
                                   {PR_BODY, PR_BODY, PR_SKIP, PR_BODY}),
                           }}),
           TestParams({.alpn_ = "h3",
                       .prParams =
                           PartiallyReliableTestParams{
                               .bodyScript = std::vector<uint8_t>({PR_BODY,
                                                                   PR_BODY,
                                                                   PR_SKIP,
                                                                   PR_BODY,
                                                                   PR_SKIP,
                                                                   PR_BODY,
                                                                   PR_SKIP,
                                                                   PR_SKIP}),
                           }})),
    paramsToTestName);

TEST_P(HQDownstreamSessionTestHQPR, GetPrScriptedReject) {
  InSequence enforceOrder;

  auto req = getGetRequest();
  req.setPartiallyReliable();
  auto streamId = sendRequest(req);
  auto handler = addSimpleStrictPrHandler();
  handler->expectHeaders();

  const auto& bodyScript = GetParam().prParams->bodyScript;
  uint64_t delta = 42;
  size_t responseLen = delta * bodyScript.size();

  // Start the response.
  handler->expectEOM([&]() {
    handler->txn_->setTransportCallback(&transportCallback_);
    handler->sendPrHeaders(200, responseLen);
  });
  flushRequestsAndLoop();

  EXPECT_TRUE(transportCallback_.lastEgressHeadersByteDelivered_);

  size_t c = 0;
  uint64_t bodyBytesProcessed = 0;
  uint64_t streamOffset = 0;

  const uint64_t startStreamOffset =
      socketDriver_->streams_[streamId].writeOffset;

  for (const auto& item : bodyScript) {
    bool eom = c == bodyScript.size() - 1;

    LOG(INFO) << "c: " << c << ", bodyBytesProcessed = " << bodyBytesProcessed;

    switch (item) {
      case PR_BODY:
        // Send <delta> bytes of the body.
        handler->sendBody(delta);
        break;
      case PR_SKIP:
        // Reject first <delta> bytes.
        handler->expectBodyRejected([&](uint64_t bodyOffset) {
          EXPECT_EQ(bodyOffset, bodyBytesProcessed + delta);
        });
        streamOffset = startStreamOffset + bodyBytesProcessed + delta;
        socketDriver_->deliverDataRejected(streamId, streamOffset);
        break;
      default:
        CHECK(false) << "Unknown PR body script item: " << item;
    }

    if (eom) {
      handler->sendEOM();
      handler->expectDetachTransaction();
      flushRequestsAndLoop();
    } else {
      flushRequestsAndLoopN(1);
    }

    Mock::VerifyAndClearExpectations(handler.get());

    bodyBytesProcessed += delta;
    c++;
  }

  hqSession_->closeWhenIdle();
}

TEST_P(HQDownstreamSessionTestHQPR, GetPrBodyScriptedExpire) {
  InSequence enforceOrder;

  auto req = getGetRequest();
  req.setPartiallyReliable();
  auto streamId = sendRequest(req);
  auto handler = addSimpleStrictPrHandler();
  handler->expectHeaders();

  const auto& bodyScript = GetParam().prParams->bodyScript;
  uint64_t delta = 42;
  size_t responseLen = delta * bodyScript.size();

  // Start the response.
  handler->expectEOM([&]() {
    handler->txn_->setTransportCallback(&transportCallback_);
    handler->sendPrHeaders(200, responseLen);
  });
  flushRequestsAndLoop();

  EXPECT_TRUE(transportCallback_.lastEgressHeadersByteDelivered_);

  size_t c = 0;
  uint64_t bodyBytesProcessed = 0;
  uint64_t oldWriteOffset = 0;
  folly::Expected<folly::Optional<uint64_t>, ErrorCode> expireRes;

  for (const auto& item : bodyScript) {
    bool eom = c == bodyScript.size() - 1;

    LOG(INFO) << "c: " << c << ", bodyBytesProcessed = " << bodyBytesProcessed;

    switch (item) {
      case PR_BODY:
        // Send <delta> bytes of the body.
        handler->sendBody(delta);
        break;
      case PR_SKIP:
        // Expire <delta> bytes.
        oldWriteOffset = socketDriver_->streams_[streamId].writeOffset;
        expireRes = handler->txn_->skipBodyTo(bodyBytesProcessed + delta);
        EXPECT_FALSE(expireRes.hasError());
        EXPECT_EQ(socketDriver_->streams_[streamId].writeOffset,
                  oldWriteOffset + delta);
        break;
      default:
        CHECK(false) << "Unknown PR body script item: " << item;
    }

    if (eom) {
      handler->sendEOM();
      handler->expectDetachTransaction();
      flushRequestsAndLoop();
    } else {
      flushRequestsAndLoopN(1);
    }

    Mock::VerifyAndClearExpectations(handler.get());

    bodyBytesProcessed += delta;
    c++;
}

  hqSession_->closeWhenIdle();
}

INSTANTIATE_TEST_CASE_P(
    HQUpstreamSessionTest,
    HQDownstreamSessionTestHQPrBadOffset,
    Values(TestParams({.alpn_ = "h3",
                       .prParams =
                           PartiallyReliableTestParams{
                               .bodyScript = std::vector<uint8_t>(),
                           }})),
    paramsToTestName);

TEST_P(HQDownstreamSessionTestHQPrBadOffset, TestWrongOffsetErrorCleanup) {
  InSequence enforceOrder;

  auto req = getGetRequest();
  req.setPartiallyReliable();
  auto streamId = sendRequest(req);
  auto handler = addSimpleStrictPrHandler();
  handler->expectHeaders();

  const size_t responseLen = 42;

  // Start the response.
  handler->expectEOM([&]() {
    handler->txn_->setTransportCallback(&transportCallback_);
    handler->sendPrHeaders(200, responseLen);
    handler->txn_->onLastEgressHeaderByteAcked();
    handler->sendBody(21);
  });
  flushRequestsAndLoopN(1);

  // Give wrong offset to the session and expect transaction to abort and
  // clean-up properly.
  uint64_t wrongOffset = 1;
  EXPECT_CALL(*handler, onError(_))
      .WillOnce(Invoke([](const HTTPException& error) {
        EXPECT_TRUE(std::string(error.what()).find("invalid offset") !=
                    std::string::npos);
      }));
  handler->expectDetachTransaction();
  hqSession_->getDispatcher()->onDataRejected(streamId, wrongOffset);

  flushRequestsAndLoop();
  hqSession_->closeWhenIdle();
}

TEST_P(HQDownstreamSessionTestHQPR, DropConnectionWithDeliveryAckCbSetError) {
  auto req = getGetRequest();
  req.setPartiallyReliable();
  auto streamId = sendRequest(req);
  auto handler = addSimpleStrictPrHandler();
  handler->expectHeaders();

  // Start the response.
  handler->expectEOM([&]() {
    handler->txn_->setTransportCallback(&transportCallback_);
    handler->sendPrHeaders(200, 1723);
  });

  auto sock = socketDriver_->getSocket();

  // This is a copy of the one in MockQuicSocketDriver, only hijacks data stream
  // and forces an error.
  EXPECT_CALL(*sock,
              registerDeliveryCallback(testing::_, testing::_, testing::_))
      .WillRepeatedly(
          testing::Invoke([streamId, &socketDriver = socketDriver_](
                              quic::StreamId id,
                              uint64_t offset,
                              MockQuicSocket::DeliveryCallback* cb)
                              -> folly::Expected<folly::Unit, LocalErrorCode> {
            if (id == streamId) {
              return folly::makeUnexpected(LocalErrorCode::INVALID_OPERATION);
            }

            socketDriver->checkNotReadOnlyStream(id);
            auto it = socketDriver->streams_.find(id);
            if (it == socketDriver->streams_.end() ||
                it->second.writeOffset >= offset) {
              return folly::makeUnexpected(LocalErrorCode::STREAM_NOT_EXISTS);
            }
            CHECK_NE(it->second.writeState,
                     MockQuicSocketDriver::StateEnum::CLOSED);
            it->second.deliveryCallbacks.push_back({offset, cb});
            return folly::unit;
          }));

  EXPECT_CALL(*handler, onError(_))
      .WillOnce(Invoke([](const HTTPException& error) {
        EXPECT_TRUE(std::string(error.what())
                        .find("failed to register delivery callback") !=
                    std::string::npos);
      }));
  handler->expectDetachTransaction();

  flushRequestsAndLoop();
  hqSession_->closeWhenIdle();
}
