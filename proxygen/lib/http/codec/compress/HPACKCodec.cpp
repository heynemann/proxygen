/*
 *  Copyright (c) 2014, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include <proxygen/lib/http/codec/compress/HPACKCodec.h>

#include <algorithm>
#include <folly/String.h>
#include <folly/io/Cursor.h>
#include <proxygen/lib/http/codec/compress/HPACKHeader.h>

using folly::IOBuf;
using folly::io::Cursor;
using proxygen::compress::Header;
using proxygen::compress::HeaderPiece;
using proxygen::compress::HeaderPieceList;
using std::unique_ptr;
using std::vector;

namespace proxygen {

const std::string kHpackNpn = "spdy/3.1-fb-0.5";

HPACKCodec::HPACKCodec(TransportDirection direction) {
  HPACK::MessageType encoderType;
  HPACK::MessageType decoderType;
  if (direction == TransportDirection::DOWNSTREAM) {
    decoderType = HPACK::MessageType::REQ;
    encoderType = HPACK::MessageType::RESP;
  } else {
    // UPSTREAM
    decoderType = HPACK::MessageType::RESP;
    encoderType = HPACK::MessageType::REQ;
  }
  encoder_ = folly::make_unique<HPACKEncoder>(encoderType, true);
  decoder_ = folly::make_unique<HPACKDecoder>(decoderType);
}

unique_ptr<IOBuf> HPACKCodec::encode(vector<Header>& headers) noexcept {
  vector<HPACKHeader> converted;
  // convert to HPACK API format
  uint32_t uncompressed = 0;
  for (const auto& h : headers) {
    HPACKHeader header(*h.name, *h.value);
    // This is ugly but since we're not changing the size
    // of the string I'm assuming this is OK
    char* mutableName = const_cast<char*>(header.name.data());
    folly::toLowerAscii(mutableName, header.name.size());
    converted.push_back(header);
    uncompressed += header.name.size() + header.value.size() + 2;
  }
  auto buf = encoder_->encode(converted, encodeHeadroom_);
  encodedSize_.compressed = 0;
  if (buf) {
    encodedSize_.compressed = buf->computeChainDataLength();
  }
  encodedSize_.uncompressed = uncompressed;
  if (stats_) {
    stats_->recordEncode(Type::HPACK, encodedSize_);
  }
  return std::move(buf);
}

Result<HeaderDecodeResult, HeaderDecodeError>
HPACKCodec::decode(Cursor& cursor, uint32_t length) noexcept {
  outHeaders_.clear();
  decodedHeaders_.clear();
  auto consumed = decoder_->decode(cursor, length, decodedHeaders_);
  if (decoder_->hasError()) {
    LOG(ERROR) << "decoder state: " << decoder_->getTable();
    if (stats_) {
      stats_->recordDecodeError(Type::HPACK);
    }
    if (decoder_->getError() == HPACKDecoder::Error::HEADERS_TOO_LARGE) {
      return HeaderDecodeError::HEADERS_TOO_LARGE;
    }
    return HeaderDecodeError::BAD_ENCODING;
  }
  // convert to HeaderPieceList
  uint32_t uncompressed = 0;
  for (uint32_t i = 0; i < decodedHeaders_.size(); i++) {
    const HPACKHeader& h = decodedHeaders_[i];
    // SPDYCodec uses this 'multi-valued' flag to detect illegal duplicates
    // Since HPACK does not preclude duplicates, pretend everything is
    // multi-valued
    bool multiValued = true;
    // one entry for the name and one for the value
    outHeaders_.emplace_back((char *)h.name.c_str(), h.name.size(),
                             false, multiValued);
    outHeaders_.emplace_back((char *)h.value.c_str(), h.value.size(),
                             false, multiValued);
    uncompressed += h.name.size() + h.value.size() + 2;
  }
  decodedSize_.compressed = consumed;
  decodedSize_.uncompressed = uncompressed;
  if (stats_) {
    stats_->recordDecode(Type::HPACK, decodedSize_);
  }
  return HeaderDecodeResult{outHeaders_, consumed};
}

}
