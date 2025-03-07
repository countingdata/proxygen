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

#include <folly/io/async/EventBase.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <quic/api/test/MockQuicSocket.h>
#include <unordered_map>

namespace quic {

using PeekIterator = std::deque<StreamBuffer>::const_iterator;

// The driver stores connection state in a Stream State structure
// so use an id outside the on the wire id space
constexpr uint64_t kConnectionStreamId = std::numeric_limits<uint64_t>::max();

class MockQuicSocketDriver : public folly::EventBase::LoopCallback {
 public:
  enum StateEnum { NEW, OPEN, PAUSED, CLOSED, ERROR };
  enum TransportEnum { CLIENT, SERVER };

  // support giving a callback to the caller (i.e. the actual test code)
  // whenever the socket receives data, so that the test driver can parse
  // that data and do stuff with it; i.e. detect control streams and feed the
  // incoming data to a codec
  class LocalAppCallback {
   public:
    virtual ~LocalAppCallback() {
    }
    virtual void unidirectionalReadCallback(
        quic::StreamId id, std::unique_ptr<folly::IOBuf> buf) = 0;
    virtual void readCallback(quic::StreamId id,
                              std::unique_ptr<folly::IOBuf> buf) = 0;
  };

 private:
  struct StreamState {
    uint64_t writeOffset{0};
    // data to be read by application
    folly::IOBufQueue readBuf{folly::IOBufQueue::cacheChainLength()};
    uint64_t readBufOffset{0};
    uint64_t readOffset{0};
    uint64_t writeBufOffset{0};
    bool readEOF{false};
    bool writeEOF{false};
    QuicSocket::WriteCallback* pendingWriteCb{nullptr};
    // data written by application
    folly::IOBufQueue pendingWriteBuf{folly::IOBufQueue::cacheChainLength()};
    // data 'delivered' to peer
    folly::IOBufQueue writeBuf{folly::IOBufQueue::cacheChainLength()};
    StateEnum readState{NEW};
    StateEnum writeState{OPEN};
    folly::Optional<quic::ApplicationErrorCode> error;
    QuicSocket::ReadCallback* readCB{nullptr};
    QuicSocket::PeekCallback* peekCB{nullptr};
    std::list<std::pair<uint64_t, QuicSocket::DeliveryCallback*>>
        deliveryCallbacks;
    uint64_t flowControlWindow{65536};
    bool isControl{false};
  };
  bool partiallyReliableTransport_{false};

 public:
  explicit MockQuicSocketDriver(
      folly::EventBase* eventBase,
      QuicSocket::ConnectionCallback& cb,
      QuicSocket::DataExpiredCallback* dataExpiredCb,
      QuicSocket::DataRejectedCallback* dataRejectedCb,
      TransportEnum transportType,
      bool partiallyReliableTransport = false)
      : partiallyReliableTransport_(partiallyReliableTransport),
        eventBase_(eventBase),
        transportType_(transportType),
        sock_(std::make_shared<MockQuicSocket>(eventBase, cb)),
        dataExpiredCb_(dataExpiredCb),
        dataRejectedCb_(dataRejectedCb) {

    if (transportType_ == TransportEnum::SERVER) {
      nextBidirectionalStreamId_ = 1;
      nextUnidirectionalStreamId_ = 3;
    } else {
      nextBidirectionalStreamId_ = 0;
      nextUnidirectionalStreamId_ = 2;
    }

    EXPECT_CALL(*sock_, isClientStream(testing::_))
        .WillRepeatedly(testing::Invoke(
            [](quic::StreamId stream) { return (stream & 0b01) == 0; }));

    EXPECT_CALL(*sock_, isServerStream(testing::_))
        .WillRepeatedly(testing::Invoke(
            [](quic::StreamId stream) { return stream & 0b01; }));

    EXPECT_CALL(*sock_, isUnidirectionalStream(testing::_))
        .WillRepeatedly(testing::Invoke(
            [](quic::StreamId stream) { return stream & 0b10; }));

    EXPECT_CALL(*sock_, isBidirectionalStream(testing::_))
        .WillRepeatedly(testing::Invoke(
            [](quic::StreamId stream) { return !(stream & 0b10); }));

    EXPECT_CALL(*sock_, getState()).WillRepeatedly(testing::Return(nullptr));

    EXPECT_CALL(*sock_, getTransportSettings())
        .WillRepeatedly(testing::ReturnRef(transportSettings_));

    EXPECT_CALL(*sock_, getClientConnectionId())
        .WillRepeatedly(
            testing::Return(quic::ConnectionId({0x11, 0x11, 0x11, 0x11})));

    EXPECT_CALL(*sock_, getServerConnectionId())
        .WillRepeatedly(
            testing::Return(quic::ConnectionId({0x11, 0x11, 0x11, 0x11})));

    EXPECT_CALL(*sock_, getAppProtocol())
        .WillRepeatedly(testing::Return(alpn_));

    EXPECT_CALL(*sock_, good())
        .WillRepeatedly(testing::ReturnPointee(&sockGood_));

    EXPECT_CALL(*sock_, getEventBase())
        .WillRepeatedly(testing::ReturnPointee(&eventBase_));

    EXPECT_CALL(*sock_, setControlStream(testing::_))
        .WillRepeatedly(testing::Invoke(
            [this](quic::StreamId id) -> folly::Optional<quic::LocalErrorCode> {
              auto stream = streams_.find(id);
              if (id == kConnectionStreamId || stream == streams_.end()) {
                return quic::LocalErrorCode::STREAM_NOT_EXISTS;
              }
              stream->second.isControl = true;
              return folly::none;
            }));

    EXPECT_CALL(*sock_, getConnectionFlowControl())
        .WillRepeatedly(testing::Invoke([this]() {
          auto& connection = streams_[kConnectionStreamId];
          flowControlAccess_.emplace(kConnectionStreamId);
          return QuicSocket::FlowControlState(
              {connection.flowControlWindow,
               connection.writeOffset + connection.flowControlWindow,
               0,
               0});
        }));

    EXPECT_CALL(*sock_, getStreamFlowControl(testing::_))
        .WillRepeatedly(testing::Invoke(
            [this](StreamId id)
                -> folly::Expected<quic::QuicSocket::FlowControlState,
                                   quic::LocalErrorCode> {
              checkNotReadOnlyStream(id);
              auto& stream = streams_[id];
              if (stream.writeState == CLOSED) {
                return folly::makeUnexpected(
                    quic::LocalErrorCode::INTERNAL_ERROR);
              }
              flowControlAccess_.emplace(id);
              return QuicSocket::FlowControlState(
                  {stream.flowControlWindow,
                   stream.writeOffset + stream.flowControlWindow,
                   0,
                   0});
            }));

    EXPECT_CALL(*sock_, setConnectionFlowControlWindow(testing::_))
        .WillRepeatedly(testing::Invoke(
            [this](uint64_t windowSize)
                -> folly::Expected<folly::Unit, quic::LocalErrorCode> {
              setConnectionFlowControlWindow(windowSize);
              return folly::unit;
            }));

    EXPECT_CALL(*sock_, setStreamFlowControlWindow(testing::_, testing::_))
        .WillRepeatedly(testing::Invoke(
            [this](StreamId id, uint64_t windowSize)
                -> folly::Expected<folly::Unit, quic::LocalErrorCode> {
              checkNotReadOnlyStream(id);
              setStreamFlowControlWindow(id, windowSize);
              sock_->cb_->onFlowControlUpdate(id);
              return folly::unit;
            }));

    using ReadCBResult = folly::Expected<folly::Unit, LocalErrorCode>;
    EXPECT_CALL(*sock_, setReadCallback(testing::_, testing::_))
        .WillRepeatedly(testing::Invoke(
            [this](StreamId id, QuicSocket::ReadCallback* cb) -> ReadCBResult {
              checkNotWriteOnlyStream(id);
              auto& stream = streams_[id];
              stream.readCB = cb;
              if (cb && stream.readState == NEW) {
                stream.readState = OPEN;
              } else if (stream.readState == ERROR) {
                return folly::makeUnexpected(
                    quic::LocalErrorCode::INTERNAL_ERROR);
              }
              return folly::unit;
            }));

    EXPECT_CALL(*sock_, pauseRead(testing::_))
        .WillRepeatedly(testing::Invoke([this](StreamId id) -> ReadCBResult {
          checkNotWriteOnlyStream(id);
          auto& stream = streams_[id];
          if (stream.readState == OPEN) {
            stream.readState = PAUSED;
            return folly::unit;
          } else {
            return folly::makeUnexpected(quic::LocalErrorCode::INTERNAL_ERROR);
          }
        }));

    EXPECT_CALL(*sock_, resumeRead(testing::_))
        .WillRepeatedly(testing::Invoke([this](StreamId id) -> ReadCBResult {
          checkNotWriteOnlyStream(id);
          auto& stream = streams_[id];
          if (stream.readState == PAUSED) {
            stream.readState = OPEN;
            if (!stream.readBuf.empty() || stream.readEOF) {
              // error is delivered immediately even if paused
              CHECK(stream.readCB);
              stream.readCB->readAvailable(id);
            }
            return folly::unit;
          } else {
            return folly::makeUnexpected(quic::LocalErrorCode::INTERNAL_ERROR);
          }
        }));

    using PeekCBResult = folly::Expected<folly::Unit, LocalErrorCode>;
    EXPECT_CALL(*sock_, setPeekCallback(testing::_, testing::_))
        .WillRepeatedly(testing::Invoke(
            [this](StreamId id, QuicSocket::PeekCallback* cb) -> PeekCBResult {
              checkNotWriteOnlyStream(id);
              auto& stream = streams_[id];
              stream.peekCB = cb;
              if (cb && stream.readState == NEW) {
                stream.readState = OPEN;
              } else if (stream.readState == ERROR) {
                return folly::makeUnexpected(
                    quic::LocalErrorCode::INTERNAL_ERROR);
              }
              return folly::unit;
            }));

    EXPECT_CALL(*sock_, consume(testing::_, testing::_))
        .WillRepeatedly(testing::Invoke(
            [this](StreamId id, size_t amount)
                -> folly::Expected<folly::Unit, LocalErrorCode> {
              auto& stream = streams_[id];
              stream.readBuf.splitAtMost(amount);
              return folly::unit;
            }));

    EXPECT_CALL(*sock_, readNaked(testing::_, testing::_))
        .WillRepeatedly(testing::Invoke(
            [this](StreamId id, size_t maxLen) -> MockQuicSocket::ReadResult {
              auto& stream = streams_[id];
              std::pair<folly::IOBuf*, bool> result;
              if (stream.readState == OPEN) {
                if (maxLen == 0) {
                  maxLen = std::numeric_limits<size_t>::max();
                }
                // Gather all buffers in the queue so that split won't
                // run dry
                stream.readBuf.gather(stream.readBuf.chainLength());

                result.first = stream.readBuf.splitAtMost(maxLen).release();
                result.second = stream.readBuf.empty() && stream.readEOF;
                if (result.second) {
                  stream.readState = CLOSED;
                }
              } else if (stream.readState == ERROR) {
                // If reads return a LocalErrorCode, writes are also in error
                stream.writeState = ERROR;
                return folly::makeUnexpected(
                    quic::LocalErrorCode::INTERNAL_ERROR);
              } else {
                result.second = true;
              }
              stream.readOffset += result.first->length();
              return result;
            }));

    EXPECT_CALL(*sock_, notifyPendingWriteOnStream(testing::_, testing::_))
        .WillRepeatedly(testing::Invoke(
            [this](StreamId id, QuicSocket::WriteCallback* wcb)
                -> folly::Expected<folly::Unit, quic::LocalErrorCode> {
              checkNotReadOnlyStream(id);
              return notifyPendingWriteImpl(id, wcb);
            }));

    EXPECT_CALL(*sock_, notifyPendingWriteOnConnection(testing::_))
        .WillRepeatedly(testing::Invoke(
            [this](QuicSocket::WriteCallback* wcb)
                -> folly::Expected<folly::Unit, quic::LocalErrorCode> {
              return notifyPendingWriteImpl(quic::kConnectionStreamId, wcb);
            }));

    EXPECT_CALL(
        *sock_,
        writeChain(testing::_, testing::_, testing::_, testing::_, testing::_))
        .WillRepeatedly(
            testing::Invoke([this](StreamId id,
                                   MockQuicSocket::SharedBuf data,
                                   bool eof,
                                   bool cork,
                                   QuicSocket::DeliveryCallback* cb)
                                -> quic::MockQuicSocket::WriteResult {
              CHECK_NE(id, kConnectionStreamId);
              checkNotReadOnlyStream(id);
              auto& stream = streams_[id];
              auto& connState = streams_[kConnectionStreamId];
              CHECK_NE(connState.writeState, CLOSED);
              // check stream.writeState == ERROR -> deliver error
              if (stream.writeState == ERROR) {
                // if writes return a LocalErrorCode, reads are also in error
                stream.readState = ERROR;
                return folly::makeUnexpected(
                    quic::LocalErrorCode::INTERNAL_ERROR);
              }
              if (!data) {
                data = folly::IOBuf::create(0);
              }
              // clip to FCW
              size_t length = std::min(data->computeChainDataLength(),
                                       stream.flowControlWindow);
              length = std::min(length, connState.flowControlWindow);
              folly::IOBufQueue dataBuf{folly::IOBufQueue::cacheChainLength()};
              dataBuf.append(data->clone());
              std::unique_ptr<folly::IOBuf> readBuf =
                  dataBuf.splitAtMost(length);
              if (localAppCb_) {
                if (sock_->isUnidirectionalStream(id)) {
                  localAppCb_->unidirectionalReadCallback(id, readBuf->clone());
                } else {
                  localAppCb_->readCallback(id, readBuf->clone());
                }
              }
              stream.pendingWriteBuf.append(std::move(readBuf));
              setStreamFlowControlWindow(id, stream.flowControlWindow - length);
              setConnectionFlowControlWindow(connState.flowControlWindow -
                                             length);
              // handle non-zero -> 0 transition, call flowControlUpdate
              stream.writeOffset += length;
              if (dataBuf.empty() && eof) {
                stream.writeEOF = true;
              }
              if (dataBuf.empty() && cb) {
                stream.deliveryCallbacks.push_back({stream.writeOffset, cb});
              }
              eventBase_->runInLoop([this, deleted = deleted_] {
                if (!*deleted) {
                  flushWrites();
                }
              });
              return dataBuf.move().release();
            }));

    EXPECT_CALL(*sock_, closeGracefully())
        .WillRepeatedly(testing::Invoke([this]() {
          flushWrites();
          auto& connState = streams_[kConnectionStreamId];
          connState.readState = CLOSED;
          connState.writeState = CLOSED;
          expectStreamsIdle();
        }));

    EXPECT_CALL(*sock_, close(testing::_))
        .WillOnce(testing::Invoke(
            [this](folly::Optional<std::pair<QuicErrorCode, std::string>>
                       errorCode) {
              flushWrites();
              auto& connState = streams_[kConnectionStreamId];
              connState.readState = CLOSED;
              connState.writeState = CLOSED;
              if (errorCode) {
                folly::variant_match(errorCode->first,
                                     [&](quic::ApplicationErrorCode err) {
                                       connState.error = err;
                                     },
                                     [](auto err) {});
              }
              sock_->cb_ = nullptr;
              deliverConnectionError(errorCode.value_or(std::make_pair(
                  LocalErrorCode::NO_ERROR, "Closing socket with no error")));
            }));
    EXPECT_CALL(*sock_, resetStream(testing::_, testing::_))
        .WillRepeatedly(testing::Invoke(
            [this](quic::StreamId id, quic::ApplicationErrorCode error) {
              checkNotReadOnlyStream(id);
              auto& stream = streams_[id];
              stream.error = error;
              stream.writeState = ERROR;
              stream.pendingWriteBuf.move();
              cancelDeliveryCallbacks(id, stream);
              return folly::unit;
            }));
    EXPECT_CALL(*sock_, stopSending(testing::_, testing::_))
        .WillRepeatedly(testing::Invoke(
            [this](quic::StreamId id, quic::ApplicationErrorCode error) {
              checkNotWriteOnlyStream(id);
              auto& stream = streams_[id];
              stream.error = error;
              // This doesn't set readState to error, because we can
              // still receive after sending STOP_SENDING
              return folly::unit;
            }));
    EXPECT_CALL(*sock_, createBidirectionalStream(testing::_))
        .WillRepeatedly(testing::Invoke([this](bool /*replaySafe*/) {
          auto streamId = nextBidirectionalStreamId_;
          nextBidirectionalStreamId_ += 4;
          streams_[streamId];
          return streamId;
        }));
    EXPECT_CALL(*sock_, createUnidirectionalStream(testing::_))
        .WillRepeatedly(
            testing::Invoke([this](bool /*replaySafe*/)
                                -> folly::Expected<StreamId, LocalErrorCode> {
              uint64_t activeUniStreams = count_if(
                  streams_.begin(), streams_.end(), [&](const auto& item) {
                    auto id = item.first;
                    const auto& s = item.second;
                    return (sock_->isUnidirectionalStream(id) &&
                            s.readState != CLOSED && s.writeState != CLOSED);
                  });
              if (activeUniStreams >= unidirectionalStreamsCredit_) {
                return folly::makeUnexpected(
                    quic::LocalErrorCode::STREAM_LIMIT_EXCEEDED);
              }

              auto streamId = nextUnidirectionalStreamId_;
              nextUnidirectionalStreamId_ += 4;
              streams_[streamId];
              return streamId;
            }));
    EXPECT_CALL(*sock_, getStreamWriteOffset(testing::_))
        .WillRepeatedly(testing::Invoke(
            [this](
                quic::StreamId id) -> folly::Expected<size_t, LocalErrorCode> {
              checkNotReadOnlyStream(id);
              auto it = streams_.find(id);
              if (it == streams_.end()) {
                return folly::makeUnexpected(LocalErrorCode::STREAM_NOT_EXISTS);
              }
              CHECK_NE(it->second.writeState, CLOSED);
              return it->second.writeOffset -
                     it->second.pendingWriteBuf.chainLength();
            }));
    EXPECT_CALL(*sock_, getStreamWriteBufferedBytes(testing::_))
        .WillRepeatedly(testing::Invoke(
            [this](
                quic::StreamId id) -> folly::Expected<size_t, LocalErrorCode> {
              checkNotReadOnlyStream(id);
              auto it = streams_.find(id);
              if (it == streams_.end()) {
                return folly::makeUnexpected(LocalErrorCode::STREAM_NOT_EXISTS);
              }
              CHECK_NE(it->second.writeState, CLOSED);
              return it->second.pendingWriteBuf.chainLength();
            }));
    EXPECT_CALL(*sock_,
                registerDeliveryCallback(testing::_, testing::_, testing::_))
        .WillRepeatedly(testing::Invoke(
            [this](quic::StreamId id,
                   uint64_t offset,
                   MockQuicSocket::DeliveryCallback* cb)
                -> folly::Expected<folly::Unit, LocalErrorCode> {
              checkNotReadOnlyStream(id);
              auto it = streams_.find(id);
              if (it == streams_.end() || it->second.writeOffset >= offset) {
                return folly::makeUnexpected(LocalErrorCode::STREAM_NOT_EXISTS);
              }
              CHECK_NE(it->second.writeState, CLOSED);
              it->second.deliveryCallbacks.push_back({offset, cb});
              return folly::unit;
            }));

    EXPECT_CALL(*sock_, isPartiallyReliableTransport())
        .WillRepeatedly(testing::Invoke(
            [this]() -> bool { return partiallyReliableTransport_; }));

    EXPECT_CALL(*sock_, sendDataExpired(testing::_, testing::_))
        .WillRepeatedly(
            testing::Invoke([this](quic::StreamId id, uint64_t streamOffset)
                                -> folly::Expected<folly::Optional<uint64_t>,
                                                   LocalErrorCode> {
              checkNotReadOnlyStream(id);
              auto it = streams_.find(id);
              if (it == streams_.end()) {
                return folly::makeUnexpected(
                    LocalErrorCode::STREAM_NOT_EXISTS);
              }
              CHECK_NE(it->second.writeState, CLOSED);

              it->second.writeOffset = streamOffset;
              return folly::makeExpected<LocalErrorCode>(streamOffset);
            }));

    EXPECT_CALL(*sock_, sendDataRejected(testing::_, testing::_))
        .WillRepeatedly(testing::Invoke(
            [this](quic::StreamId id, uint64_t streamOffset)
                -> folly::Expected<folly::Optional<uint64_t>, LocalErrorCode> {
              checkNotReadOnlyStream(id);
              auto it = streams_.find(id);
              if (it == streams_.end()) {
                return folly::makeUnexpected(LocalErrorCode::STREAM_NOT_EXISTS);
              }
              CHECK_NE(it->second.readState, CLOSED);
              it->second.readOffset = streamOffset;
              return folly::makeExpected<LocalErrorCode>(streamOffset);
            }));
  }

  quic::StreamId getMaxStreamId() {
    return std::max_element(
               streams_.begin(),
               streams_.end(),
               [](const std::pair<const StreamId, StreamState>& a,
                  const std::pair<const StreamId, StreamState>& b) -> bool {
                 return a.first < b.first && b.first != kConnectionStreamId;
               })
        ->first;
  }

  bool isClosed() {
    return streams_[kConnectionStreamId].readState != OPEN &&
           streams_[kConnectionStreamId].writeState != OPEN;
  }

  void setLocalAppCallback(LocalAppCallback* localAppCb) {
    localAppCb_ = localAppCb;
  }

  void checkNotReadOnlyStream(quic::StreamId id) {
    CHECK(!(sock_->isUnidirectionalStream(id) && isReceivingStream(id)))
        << "API not supported on read-only unidirectional stream. streamID="
        << id;
  }

  void checkNotWriteOnlyStream(quic::StreamId id) {
    CHECK(!(sock_->isUnidirectionalStream(id) && isSendingStream(id)))
        << "API not supported on write-only unidirectional stream. streamID="
        << id;
  }

  bool isSendingStream(StreamId stream) {
    return sock_->isUnidirectionalStream(stream) &&
           ((transportType_ == TransportEnum::CLIENT &&
             sock_->isClientStream(stream)) ||
            (transportType_ == TransportEnum::SERVER &&
             sock_->isServerStream(stream)));
  }

  bool isReceivingStream(StreamId stream) {
    return sock_->isUnidirectionalStream(stream) &&
           ((transportType_ == TransportEnum::CLIENT &&
             sock_->isServerStream(stream)) ||
            (transportType_ == TransportEnum::SERVER &&
             sock_->isClientStream(stream)));
  }

  static bool isIdle(StateEnum state) {
    return state == CLOSED || state == ERROR;
  }

  bool isStreamIdle(StreamId id) {
    return isIdle(streams_[id].readState);
  }

  bool isStreamPaused(StreamId id) {
    return streams_[id].readState == PAUSED;
  }

  void deliverErrorOnAllStreams(std::pair<QuicErrorCode, std::string> error) {
    for (auto& it : streams_) {
      auto& stream = it.second;
      if (it.first == kConnectionStreamId) {
        deliverWriteError(it.first, stream, error.first);
        continue;
      }
      if (!isIdle(stream.readState)) {
        if (stream.readCB) {
          auto cb = stream.readCB;
          stream.readCB = nullptr;
          stream.peekCB = nullptr;
          cb->readError(
              it.first,
              std::make_pair(error.first, folly::StringPiece(error.second)));
        }
        stream.readState = ERROR;
      }
      if (!isIdle(stream.writeState)) {
        deliverWriteError(it.first, stream, error.first);
      }
      cancelDeliveryCallbacks(it.first, stream);
    }
  }

  void deliverConnectionError(std::pair<QuicErrorCode, std::string> error) {
    deliverErrorOnAllStreams(error);
    auto cb = sock_->cb_;
    sock_->cb_ = nullptr;
    if (cb) {
      bool noError =
          folly::variant_match(error.first,
                               [](const LocalErrorCode& err) {
                                 return err == LocalErrorCode::NO_ERROR ||
                                        err == LocalErrorCode::IDLE_TIMEOUT;
                               },
                               [](const TransportErrorCode& err) {
                                 return err == TransportErrorCode::NO_ERROR;
                               },
                               [](const auto&) { return false; });
      if (noError) {
        cb->onConnectionEnd();
      } else {
        cb->onConnectionError(std::move(error));
      }
    }
  }

  void deliverWriteError(quic::StreamId id,
                         StreamState& stream,
                         QuicErrorCode errorCode) {
    if (stream.pendingWriteCb) {
      auto cb = stream.pendingWriteCb;
      stream.pendingWriteCb = nullptr;
      cb->onConnectionWriteError(std::make_pair(errorCode, folly::none));
    }
    stream.writeState = ERROR;
  }

  void deliverDataExpired(quic::StreamId id, uint64_t offset) {
    auto it = streams_.find(id);
    if (it == streams_.end()) {
      return;
    }

    auto& stream = it->second;
    if (offset > stream.readOffset) {
      stream.readOffset = offset;
    }

    CHECK(dataExpiredCb_);
    dataExpiredCb_->onDataExpired(id, offset);
  }

  void deliverDataRejected(quic::StreamId id, uint64_t offset) {
    auto it = streams_.find(id);
    if (it == streams_.end()) {
      return;
    }

    auto& stream = it->second;
    if (offset > stream.writeOffset) {
      stream.writeOffset = offset;
    }

    CHECK(dataRejectedCb_);
    dataRejectedCb_->onDataRejected(id, offset);
  }

  void cancelDeliveryCallbacks(quic::StreamId id, StreamState& stream) {
    while (!stream.deliveryCallbacks.empty()) {
      stream.deliveryCallbacks.front().second->onCanceled(
          id, stream.deliveryCallbacks.front().first);
      stream.deliveryCallbacks.pop_front();
    }
  }

  folly::Expected<folly::Unit, quic::LocalErrorCode> notifyPendingWriteImpl(
      StreamId id, QuicSocket::WriteCallback* wcb) {
    auto& stream = streams_[id];
    if (stream.writeState == PAUSED) {
      stream.pendingWriteCb = wcb;
      return folly::unit;
    } else if (stream.writeState == OPEN) {
      // Be a bit more unforgiving than the real transport of logical errors.
      CHECK(!stream.pendingWriteCb) << "Called notifyPendingWrite twice";
      stream.pendingWriteCb = wcb;
      eventBase_->runInLoop(
          [this, id, &stream, deleted = deleted_] {
            if (*deleted) {
              return;
            }
            // This callback was scheduled to be delivered when the stream
            // writeState was OPEN, do not deliver the callback if the state
            // changed in the meantime
            if (stream.writeState != OPEN) {
              return;
            }
            CHECK_NOTNULL(stream.pendingWriteCb);
            auto writeCb = stream.pendingWriteCb;
            stream.pendingWriteCb = nullptr;
            auto window = streams_[id].flowControlWindow;
            // TODO: support stream write ready calls as well. Currently the
            // only consumer of MockQuicSocketDriver is HQSession which only
            // notifies the connection ready call.
            writeCb->onConnectionWriteReady(window);
          },
          true);
    } else {
      // closed, error
      return folly::makeUnexpected(LocalErrorCode::CONNECTION_CLOSED);
    }
    return folly::unit;
  }

  void expectStreamsIdle(bool connection = false) {
    for (auto& it : streams_) {
      if ((!it.second.isControl && it.first != kConnectionStreamId) ||
          connection) {
        EXPECT_TRUE(isIdle(it.second.readState))
            << "stream=" << it.first << " readState=" << it.second.readState;
        EXPECT_TRUE(isIdle(it.second.writeState))
            << "stream=" << it.first << " writeState=" << it.second.writeState;
      }
    }
  }

  void expectStreamWritesPaused(StreamId id) {
    EXPECT_EQ(streams_[id].writeState, PAUSED);
  }

  void expectConnWritesPaused() {
    EXPECT_EQ(streams_[quic::kConnectionStreamId].writeState, PAUSED);
  }

  ~MockQuicSocketDriver() {
    expectStreamsIdle(true);
    *deleted_ = true;
  }

  void writePendingDataAndAck(StreamState& stream, StreamId id) {
    stream.writeBuf.append(stream.pendingWriteBuf.move());
    if (stream.writeEOF) {
      stream.writeState = CLOSED;
    }

    // delay delivery callbacks 50ms
    eventBase_->runAfterDelay(
        [&stream, id, deleted = deleted_] {
          if (*deleted) {
            return;
          }
          while (!stream.deliveryCallbacks.empty() &&
                 stream.deliveryCallbacks.front().first <= stream.writeOffset) {
            stream.deliveryCallbacks.front().second->onDeliveryAck(
                id,
                stream.deliveryCallbacks.front().first,
                std::chrono::milliseconds(0));
            stream.deliveryCallbacks.pop_front();
          }
        },
        50);
  }

  void flushWrites(StreamId id = kConnectionStreamId) {
    auto& connState = streams_[kConnectionStreamId];
    for (auto& it : streams_) {
      if (it.first == kConnectionStreamId ||
          (id != kConnectionStreamId && it.first != id)) {
        continue;
      }
      auto& stream = it.second;
      if (connState.writeState == OPEN && stream.writeState == OPEN &&
          (!stream.pendingWriteBuf.empty() || stream.writeEOF)) {
        // handle 0->non-zero transition, call flowControlUpdate
        setStreamFlowControlWindow(
            it.first,
            stream.flowControlWindow + stream.pendingWriteBuf.chainLength());
        setConnectionFlowControlWindow(connState.flowControlWindow +
                                       stream.pendingWriteBuf.chainLength());
        writePendingDataAndAck(stream, it.first);
      } else if (!stream.pendingWriteBuf.empty() || stream.writeEOF) {
        // If we are paused only write the data that we have pending and don't
        // trigger flow control updates to simulate reads from the other side
        writePendingDataAndAck(stream, it.first);
      }
    }
  }

  void addReadEvent(StreamId streamId,
                    std::unique_ptr<folly::IOBuf> buf,
                    std::chrono::milliseconds delayFromPrevious =
                        std::chrono::milliseconds(0)) {
    addReadEventInternal(
        streamId, std::move(buf), false, folly::none, delayFromPrevious);
  }

  void addReadEOF(StreamId streamId,
                  std::chrono::milliseconds delayFromPrevious =
                      std::chrono::milliseconds(0)) {
    addReadEventInternal(
        streamId, nullptr, true, folly::none, delayFromPrevious);
  }

  void addReadError(StreamId streamId,
                    QuicErrorCode error,
                    std::chrono::milliseconds delayFromPrevious =
                        std::chrono::milliseconds(0)) {
    addReadEventInternal(streamId, nullptr, false, error, delayFromPrevious);
  }

  void addStopSending(StreamId streamId,
                      ApplicationErrorCode error,
                      std::chrono::milliseconds delayFromPrevious =
                          std::chrono::milliseconds(0)) {
    QuicErrorCode qec = error;
    addReadEventInternal(
        streamId, nullptr, false, qec, delayFromPrevious, true);
  }

  void setReadError(StreamId streamId) {
    streams_[streamId].readState = ERROR;
  }

  void setWriteError(StreamId streamId) {
    streams_[streamId].writeState = ERROR;
    cancelDeliveryCallbacks(streamId, streams_[streamId]);
  }

  void addOnConnectionEndEvent(uint32_t millisecondsDelay) {
    eventBase_->runAfterDelay(
        [this, deleted = deleted_] {
          if (!*deleted && sock_->cb_) {
            deliverErrorOnAllStreams(
                {quic::LocalErrorCode::NO_ERROR, "onConnectionEnd"});
            auto& connState = streams_[kConnectionStreamId];
            connState.readState = CLOSED;
            connState.writeState = CLOSED;
            auto cb = sock_->cb_;
            // clear or cancel all the callbacks
            sock_->cb_ = nullptr;
            for (auto& it : streams_) {
              auto& stream = it.second;
              stream.readCB = nullptr;
              stream.peekCB = nullptr;
              stream.pendingWriteCb = nullptr;
            }
            cb->onConnectionEnd();
          }
        },
        millisecondsDelay);
  }

  // Schedules a callback in this loop if the delay is zero,
  // otherwise sets a timeout
  void runInThisLoopOrAfterDelay(folly::Func cob, uint32_t millisecondsDelay) {
    if (millisecondsDelay == 0) {
      eventBase_->runInLoop(std::move(cob), true);
    } else {
      // runAfterDelay doesn't guarantee order if two events run after the same
      // delay.  So queue the function and only use runAfterDelay to signal
      // the event.
      events_.emplace_back(std::move(cob));
      eventBase_->runAfterDelay(
          [this] {
            CHECK(!events_.empty());
            auto event = std::move(events_.front());
            events_.pop_front();
            event();
          },
          millisecondsDelay);
    }
  }

  struct ReadEvent {
    ReadEvent(StreamId s,
              std::unique_ptr<folly::IOBuf> b,
              bool e,
              folly::Optional<QuicErrorCode> er,
              bool ss)
        : streamId(s), buf(std::move(b)), eof(e), error(er), stopSending(ss) {
    }

    StreamId streamId;
    std::unique_ptr<folly::IOBuf> buf;
    bool eof;
    folly::Optional<QuicErrorCode> error;
    bool stopSending;
  };

  void addReadEventInternal(StreamId streamId,
                            std::unique_ptr<folly::IOBuf> buf,
                            bool eof,
                            folly::Optional<QuicErrorCode> error,
                            std::chrono::milliseconds delayFromPrevious =
                                std::chrono::milliseconds(0),
                            bool stopSending = false) {
    std::vector<ReadEvent> events;
    events.emplace_back(streamId, std::move(buf), eof, error, stopSending);
    addReadEvents(std::move(events), delayFromPrevious);
  }

  void addReadEvents(std::vector<ReadEvent> events,
                     std::chrono::milliseconds delayFromPrevious =
                         std::chrono::milliseconds(0)) {
    ASSERT_NE(streams_[kConnectionStreamId].readState, CLOSED);
    cumulativeDelay_ += delayFromPrevious;
    runInThisLoopOrAfterDelay(
        [events = std::move(events), this, deleted = deleted_]() mutable {
          // zero out cumulative delay
          cumulativeDelay_ = std::chrono::milliseconds(0);
          if (*deleted) {
            return;
          }
          // This read event was scheduled to run in the evb, when it was
          // scheduled the connection state was not CLOSED for reads.
          // let's make sure this still holds
          if (streams_[kConnectionStreamId].readState == CLOSED) {
            return;
          }
          for (auto& event : events) {
            auto& stream = streams_[event.streamId];
            if (!event.error) {
              CHECK_NE(stream.readState, CLOSED);
            } else {
              CHECK(!event.buf || event.buf->empty());
              CHECK(!event.eof);
            }
            auto bufLen = event.buf ? event.buf->computeChainDataLength() : 0;
            stream.readBufOffset += bufLen;
            stream.readBuf.append(std::move(event.buf));
            stream.readEOF = event.eof;
            if (stream.readState == NEW) {
              stream.readState = OPEN;
              if (sock_->cb_) {
                if (sock_->isUnidirectionalStream(event.streamId)) {
                  sock_->cb_->onNewUnidirectionalStream(event.streamId);
                } else {
                  sock_->cb_->onNewBidirectionalStream(event.streamId);
                }
              }
            }
            if (event.error && event.stopSending) {
              if (sock_->cb_) {
                folly::variant_match(*event.error,
                                     [&](quic::ApplicationErrorCode err) {
                                       sock_->cb_->onStopSending(event.streamId,
                                                                 err);
                                     },
                                     [](auto err) {});
              }
              return;
            }
            if (stream.peekCB && stream.readState != PAUSED &&
                stream.readBuf.front()) {
              std::deque<StreamBuffer> fakeReadBuffer;
              auto tmpBuf = stream.readBuf.move();
              tmpBuf->coalesce();
              stream.readBuf.append(std::move(tmpBuf));
              auto copyBuf = stream.readBuf.front()->clone();
              fakeReadBuffer.emplace_back(
                  std::move(copyBuf), stream.readOffset, false);
              stream.peekCB->onDataAvailable(
                  event.streamId,
                  folly::Range<PeekIterator>(fakeReadBuffer.cbegin(),
                                             fakeReadBuffer.size()));
            }
            if (stream.readCB) {
              if (event.error) {
                stream.readCB->readError(
                    event.streamId, std::make_pair(*event.error, folly::none));
                stream.readState = ERROR;
              } else if (stream.readState != PAUSED) {
                stream.readCB->readAvailable(event.streamId);
                eventBase_->runInLoop(this);
              } // else if PAUSED, no-op
            }
          }
        },
        cumulativeDelay_.count());
  }

  void pauseOrResumeWrites(StreamState& stream, quic::StreamId streamId) {
    if (stream.writeState == OPEN && stream.flowControlWindow == 0) {
      pauseWrites(streamId);
    } else if (stream.writeState == PAUSED && stream.flowControlWindow > 0) {
      resumeWrites(streamId);
    }
  }

  void setConnectionFlowControlWindow(uint64_t windowSize) {
    auto& stream = streams_[kConnectionStreamId];
    CHECK_NE(stream.writeState, CLOSED);
    stream.flowControlWindow = windowSize;
    pauseOrResumeWrites(stream, kConnectionStreamId);
  }

  void setStreamFlowControlWindow(StreamId streamId, uint64_t windowSize) {
    auto& stream = streams_[streamId];
    CHECK_NE(stream.writeState, CLOSED);
    stream.flowControlWindow = windowSize;
    pauseOrResumeWrites(stream, streamId);
  }

  void pauseWrites(StreamId streamId) {
    auto& stream = streams_[streamId];
    CHECK_EQ(stream.writeState, OPEN);
    stream.writeState = PAUSED;
  }

  // This is to model the fact that the transport may close a stream without
  // giving a readError callback
  void forceStreamClose(StreamId streamId) {
    auto& stream = streams_[streamId];
    stream.readState = CLOSED;
    stream.writeState = CLOSED;
    cancelDeliveryCallbacks(streamId, stream);
  }

  void resumeWrites(StreamId streamId) {
    auto& stream = streams_[streamId];
    CHECK_EQ(stream.writeState, PAUSED);
    stream.writeState = OPEN;
    // first flush any buffered writes
    flushWrites(streamId);
    // now check onConnectionWriteReady call is warranted.
    if (stream.writeState == OPEN && stream.pendingWriteCb &&
        streams_[kConnectionStreamId].flowControlWindow > 0) {
      eventBase_->runInLoop(
          [this, wcb = stream.pendingWriteCb, deleted = deleted_] {
            if (!*deleted) {
              // TODO: support stream write ready calls as well. Currently the
              // only consumer of MockQuicSocketDriver is HQSession which only
              // notifies the connection ready call.
              wcb->onConnectionWriteReady(
                  streams_[kConnectionStreamId].flowControlWindow);
            }
          },
          true);
    }
    if (streamId != quic::kConnectionStreamId) {
      sock_->cb_->onFlowControlUpdate(streamId);
    }
    stream.pendingWriteCb = nullptr;
  }

  std::shared_ptr<MockQuicSocket> getSocket() {
    return sock_;
  }

  void enablePartialReliability() {
    EXPECT_CALL(*sock_, setDataExpiredCallback(testing::_, testing::_))
      .WillRepeatedly(testing::Invoke(
            [this](StreamId id, QuicSocket::DataExpiredCallback* cb) {
              dataExpiredCb_ = cb;
              return folly::unit;
              }));

    EXPECT_CALL(*sock_, setDataRejectedCallback(testing::_, testing::_))
      .WillRepeatedly(testing::Invoke(
            [this](StreamId id, QuicSocket::DataRejectedCallback* cb) {
              dataRejectedCb_ = cb;
              return folly::unit;
              }));
  }

  void runLoopCallback() noexcept override {
    bool reschedule = false;
    for (auto& it : streams_) {
      if (it.first != kConnectionStreamId &&
          (it.second.readCB || it.second.peekCB) &&
          it.second.readState == OPEN &&
          (!it.second.readBuf.empty() || it.second.readEOF)) {
        if (it.second.peekCB) {
          std::deque<StreamBuffer> fakeReadBuffer;
          auto copyBuf = it.second.readBuf.front()->clone();
          auto copyBufLen = copyBuf->computeChainDataLength();
          CHECK_GE(it.second.readBufOffset, copyBufLen);
          fakeReadBuffer.emplace_back(std::move(copyBuf), copyBufLen, false);
          it.second.peekCB->onDataAvailable(
              it.first,
              folly::Range<PeekIterator>(fakeReadBuffer.cbegin(),
                                         fakeReadBuffer.size()));
        }
        if (it.second.readCB) {
          it.second.readCB->readAvailable(it.first);
          reschedule = true;
        }
      }
    }
    if (reschedule) {
      eventBase_->runInLoop(this);
    }
  }

  folly::EventBase* eventBase_;
  TransportSettings transportSettings_;
  // keeping this ordered for better debugging
  std::map<StreamId, StreamState> streams_;
  std::list<folly::Func> events_;
  TransportEnum transportType_;
  std::shared_ptr<MockQuicSocket> sock_;
  std::chrono::milliseconds cumulativeDelay_{std::chrono::milliseconds(0)};
  bool sockGood_{true};
  std::set<StreamId> flowControlAccess_;
  uint64_t nextBidirectionalStreamId_;
  uint64_t nextUnidirectionalStreamId_;
  uint64_t unidirectionalStreamsCredit_;
  std::shared_ptr<bool> deleted_{new bool(false)};
  std::string alpn_ = "h1q-fb";
  LocalAppCallback* localAppCb_{nullptr};
  QuicSocket::DataExpiredCallback* dataExpiredCb_;
  QuicSocket::DataRejectedCallback* dataRejectedCb_;
};

} // namespace quic
