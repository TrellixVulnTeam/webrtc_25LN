/*
 *  Copyright 2016 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include <stdint.h>
#include <string.h>
#include <algorithm>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "absl/algorithm/container.h"
#include "absl/strings/match.h"
#include "api/audio_codecs/audio_decoder_factory.h"
#include "api/audio_codecs/audio_encoder_factory.h"
#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/audio_options.h"
#include "api/data_channel_interface.h"
#include "api/peer_connection_interface.h"
#include "api/rtp_receiver_interface.h"
#include "api/rtp_sender_interface.h"
#include "api/scoped_refptr.h"
#include "api/stats/rtc_stats.h"
#include "api/stats/rtc_stats_report.h"
#include "api/stats/rtcstats_objects.h"
#include "pc/rtc_stats_traversal.h"
#include "pc/test/peer_connection_test_wrapper.h"
#include "pc/test/rtc_stats_obtainer.h"
#include "rtc_base/checks.h"
#include "rtc_base/event_tracer.h"
#include "rtc_base/gunit.h"
#include "rtc_base/ref_counted_object.h"
#include "rtc_base/thread.h"
#include "rtc_base/trace_event.h"
#include "rtc_base/virtual_socket_server.h"
#include "test/gmock.h"
#include "test/gtest.h"

using ::testing::Contains;

namespace webrtc {

namespace {

const int64_t kGetStatsTimeoutMs = 10000;

const unsigned char* GetCategoryEnabledHandler(const char* name) {
  if (strcmp("webrtc_stats", name) != 0) {
    return reinterpret_cast<const unsigned char*>("");
  }
  return reinterpret_cast<const unsigned char*>(name);
}

class RTCStatsReportTraceListener {
 public:
  static void SetUp() {
    if (!traced_report_)
      traced_report_ = new RTCStatsReportTraceListener();
    traced_report_->last_trace_ = "";
    SetupEventTracer(&GetCategoryEnabledHandler,
                     &RTCStatsReportTraceListener::AddTraceEventHandler);
  }

  static const std::string& last_trace() {
    RTC_DCHECK(traced_report_);
    return traced_report_->last_trace_;
  }

 private:
  static void AddTraceEventHandler(
      char phase,
      const unsigned char* category_enabled,
      const char* name,
      unsigned long long id,  // NOLINT(runtime/int)
      int num_args,
      const char** arg_names,
      const unsigned char* arg_types,
      const unsigned long long* arg_values,  // NOLINT(runtime/int)
      unsigned char flags) {
    RTC_DCHECK(traced_report_);
    EXPECT_STREQ("webrtc_stats",
                 reinterpret_cast<const char*>(category_enabled));
    EXPECT_STREQ("webrtc_stats", name);
    EXPECT_EQ(1, num_args);
    EXPECT_STREQ("report", arg_names[0]);
    EXPECT_EQ(TRACE_VALUE_TYPE_COPY_STRING, arg_types[0]);

    traced_report_->last_trace_ = reinterpret_cast<const char*>(arg_values[0]);
  }

  static RTCStatsReportTraceListener* traced_report_;
  std::string last_trace_;
};

RTCStatsReportTraceListener* RTCStatsReportTraceListener::traced_report_ =
    nullptr;

class RTCStatsIntegrationTest : public ::testing::Test {
 public:
  RTCStatsIntegrationTest()
      : network_thread_(new rtc::Thread(&virtual_socket_server_)),
        worker_thread_(rtc::Thread::Create()) {
    RTCStatsReportTraceListener::SetUp();

    RTC_CHECK(network_thread_->Start());
    RTC_CHECK(worker_thread_->Start());

    caller_ = new rtc::RefCountedObject<PeerConnectionTestWrapper>(
        "caller", network_thread_.get(), worker_thread_.get());
    callee_ = new rtc::RefCountedObject<PeerConnectionTestWrapper>(
        "callee", network_thread_.get(), worker_thread_.get());
  }

  void StartCall() {
    // Create PeerConnections and "connect" sigslots
    PeerConnectionInterface::RTCConfiguration config;
    PeerConnectionInterface::IceServer ice_server;
    ice_server.uri = "stun:1.1.1.1:3478";
    config.servers.push_back(ice_server);
    EXPECT_TRUE(caller_->CreatePc(config, CreateBuiltinAudioEncoderFactory(),
                                  CreateBuiltinAudioDecoderFactory()));
    EXPECT_TRUE(callee_->CreatePc(config, CreateBuiltinAudioEncoderFactory(),
                                  CreateBuiltinAudioDecoderFactory()));
    PeerConnectionTestWrapper::Connect(caller_.get(), callee_.get());

    // Get user media for audio and video
    caller_->GetAndAddUserMedia(true, cricket::AudioOptions(), true);
    callee_->GetAndAddUserMedia(true, cricket::AudioOptions(), true);

    // Create data channels
    DataChannelInit init;
    caller_->CreateDataChannel("data", init);
    callee_->CreateDataChannel("data", init);

    // Negotiate and wait for call to establish
    caller_->CreateOffer(PeerConnectionInterface::RTCOfferAnswerOptions());
    caller_->WaitForCallEstablished();
    callee_->WaitForCallEstablished();
  }

  rtc::scoped_refptr<const RTCStatsReport> GetStatsFromCaller() {
    return GetStats(caller_->pc());
  }
  rtc::scoped_refptr<const RTCStatsReport> GetStatsFromCaller(
      rtc::scoped_refptr<RtpSenderInterface> selector) {
    return GetStats(caller_->pc(), selector);
  }
  rtc::scoped_refptr<const RTCStatsReport> GetStatsFromCaller(
      rtc::scoped_refptr<RtpReceiverInterface> selector) {
    return GetStats(caller_->pc(), selector);
  }

  rtc::scoped_refptr<const RTCStatsReport> GetStatsFromCallee() {
    return GetStats(callee_->pc());
  }
  rtc::scoped_refptr<const RTCStatsReport> GetStatsFromCallee(
      rtc::scoped_refptr<RtpSenderInterface> selector) {
    return GetStats(callee_->pc(), selector);
  }
  rtc::scoped_refptr<const RTCStatsReport> GetStatsFromCallee(
      rtc::scoped_refptr<RtpReceiverInterface> selector) {
    return GetStats(callee_->pc(), selector);
  }

 protected:
  static rtc::scoped_refptr<const RTCStatsReport> GetStats(
      PeerConnectionInterface* pc) {
    rtc::scoped_refptr<RTCStatsObtainer> stats_obtainer =
        RTCStatsObtainer::Create();
    pc->GetStats(stats_obtainer);
    EXPECT_TRUE_WAIT(stats_obtainer->report(), kGetStatsTimeoutMs);
    return stats_obtainer->report();
  }

  template <typename T>
  static rtc::scoped_refptr<const RTCStatsReport> GetStats(
      PeerConnectionInterface* pc,
      rtc::scoped_refptr<T> selector) {
    rtc::scoped_refptr<RTCStatsObtainer> stats_obtainer =
        RTCStatsObtainer::Create();
    pc->GetStats(selector, stats_obtainer);
    EXPECT_TRUE_WAIT(stats_obtainer->report(), kGetStatsTimeoutMs);
    return stats_obtainer->report();
  }

  // |network_thread_| uses |virtual_socket_server_| so they must be
  // constructed/destructed in the correct order.
  rtc::VirtualSocketServer virtual_socket_server_;
  std::unique_ptr<rtc::Thread> network_thread_;
  std::unique_ptr<rtc::Thread> worker_thread_;
  rtc::scoped_refptr<PeerConnectionTestWrapper> caller_;
  rtc::scoped_refptr<PeerConnectionTestWrapper> callee_;
};

class RTCStatsVerifier {
 public:
  RTCStatsVerifier(const RTCStatsReport* report, const RTCStats* stats)
      : report_(report), stats_(stats), all_tests_successful_(true) {
    RTC_CHECK(report_);
    RTC_CHECK(stats_);
    for (const RTCStatsMemberInterface* member : stats_->Members()) {
      untested_members_.insert(member);
    }
  }

  void MarkMemberTested(const RTCStatsMemberInterface& member,
                        bool test_successful) {
    untested_members_.erase(&member);
    all_tests_successful_ &= test_successful;
  }

  void TestMemberIsDefined(const RTCStatsMemberInterface& member) {
    EXPECT_TRUE(member.is_defined())
        << stats_->type() << "." << member.name() << "[" << stats_->id()
        << "] was undefined.";
    MarkMemberTested(member, member.is_defined());
  }

  void TestMemberIsUndefined(const RTCStatsMemberInterface& member) {
    EXPECT_FALSE(member.is_defined())
        << stats_->type() << "." << member.name() << "[" << stats_->id()
        << "] was defined (" << member.ValueToString() << ").";
    MarkMemberTested(member, !member.is_defined());
  }

  template <typename T>
  void TestMemberIsPositive(const RTCStatsMemberInterface& member) {
    EXPECT_TRUE(member.is_defined())
        << stats_->type() << "." << member.name() << "[" << stats_->id()
        << "] was undefined.";
    if (!member.is_defined()) {
      MarkMemberTested(member, false);
      return;
    }
    bool is_positive = *member.cast_to<RTCStatsMember<T>>() > T(0);
    EXPECT_TRUE(is_positive)
        << stats_->type() << "." << member.name() << "[" << stats_->id()
        << "] was not positive (" << member.ValueToString() << ").";
    MarkMemberTested(member, is_positive);
  }

  template <typename T>
  void TestMemberIsNonNegative(const RTCStatsMemberInterface& member) {
    EXPECT_TRUE(member.is_defined())
        << stats_->type() << "." << member.name() << "[" << stats_->id()
        << "] was undefined.";
    if (!member.is_defined()) {
      MarkMemberTested(member, false);
      return;
    }
    bool is_non_negative = *member.cast_to<RTCStatsMember<T>>() >= T(0);
    EXPECT_TRUE(is_non_negative)
        << stats_->type() << "." << member.name() << "[" << stats_->id()
        << "] was not non-negative (" << member.ValueToString() << ").";
    MarkMemberTested(member, is_non_negative);
  }

  void TestMemberIsIDReference(const RTCStatsMemberInterface& member,
                               const char* expected_type) {
    TestMemberIsIDReference(member, expected_type, false);
  }

  void TestMemberIsOptionalIDReference(const RTCStatsMemberInterface& member,
                                       const char* expected_type) {
    TestMemberIsIDReference(member, expected_type, true);
  }

  bool ExpectAllMembersSuccessfullyTested() {
    if (untested_members_.empty())
      return all_tests_successful_;
    for (const RTCStatsMemberInterface* member : untested_members_) {
      EXPECT_TRUE(false) << stats_->type() << "." << member->name() << "["
                         << stats_->id() << "] was not tested.";
    }
    return false;
  }

 private:
  void TestMemberIsIDReference(const RTCStatsMemberInterface& member,
                               const char* expected_type,
                               bool optional) {
    if (optional && !member.is_defined()) {
      MarkMemberTested(member, true);
      return;
    }
    bool valid_reference = false;
    if (member.is_defined()) {
      if (member.type() == RTCStatsMemberInterface::kString) {
        // A single ID.
        const RTCStatsMember<std::string>& id =
            member.cast_to<RTCStatsMember<std::string>>();
        const RTCStats* referenced_stats = report_->Get(*id);
        valid_reference =
            referenced_stats && referenced_stats->type() == expected_type;
      } else if (member.type() == RTCStatsMemberInterface::kSequenceString) {
        // A vector of IDs.
        valid_reference = true;
        const RTCStatsMember<std::vector<std::string>>& ids =
            member.cast_to<RTCStatsMember<std::vector<std::string>>>();
        for (const std::string& id : *ids) {
          const RTCStats* referenced_stats = report_->Get(id);
          if (!referenced_stats || referenced_stats->type() != expected_type) {
            valid_reference = false;
            break;
          }
        }
      }
    }
    EXPECT_TRUE(valid_reference)
        << stats_->type() << "." << member.name()
        << " is not a reference to an "
        << "existing dictionary of type " << expected_type << " ("
        << member.ValueToString() << ").";
    MarkMemberTested(member, valid_reference);
  }

  rtc::scoped_refptr<const RTCStatsReport> report_;
  const RTCStats* stats_;
  std::set<const RTCStatsMemberInterface*> untested_members_;
  bool all_tests_successful_;
};

class RTCStatsReportVerifier {
 public:
  static std::set<const char*> StatsTypes() {
    std::set<const char*> stats_types;
    stats_types.insert(RTCCertificateStats::kType);
    stats_types.insert(RTCCodecStats::kType);
    stats_types.insert(RTCDataChannelStats::kType);
    stats_types.insert(RTCIceCandidatePairStats::kType);
    stats_types.insert(RTCLocalIceCandidateStats::kType);
    stats_types.insert(RTCRemoteIceCandidateStats::kType);
    stats_types.insert(RTCMediaStreamStats::kType);
    stats_types.insert(RTCMediaStreamTrackStats::kType);
    stats_types.insert(RTCPeerConnectionStats::kType);
    stats_types.insert(RTCInboundRTPStreamStats::kType);
    stats_types.insert(RTCOutboundRTPStreamStats::kType);
    stats_types.insert(RTCTransportStats::kType);
    return stats_types;
  }

  explicit RTCStatsReportVerifier(const RTCStatsReport* report)
      : report_(report) {}

  void VerifyReport(std::vector<const char*> allowed_missing_stats) {
    std::set<const char*> missing_stats = StatsTypes();
    bool verify_successful = true;
    std::vector<const RTCTransportStats*> transport_stats =
        report_->GetStatsOfType<RTCTransportStats>();
    EXPECT_EQ(transport_stats.size(), 1U);
    std::string selected_candidate_pair_id =
        *transport_stats[0]->selected_candidate_pair_id;
    for (const RTCStats& stats : *report_) {
      missing_stats.erase(stats.type());
      if (stats.type() == RTCCertificateStats::kType) {
        verify_successful &=
            VerifyRTCCertificateStats(stats.cast_to<RTCCertificateStats>());
      } else if (stats.type() == RTCCodecStats::kType) {
        verify_successful &=
            VerifyRTCCodecStats(stats.cast_to<RTCCodecStats>());
      } else if (stats.type() == RTCDataChannelStats::kType) {
        verify_successful &=
            VerifyRTCDataChannelStats(stats.cast_to<RTCDataChannelStats>());
      } else if (stats.type() == RTCIceCandidatePairStats::kType) {
        verify_successful &= VerifyRTCIceCandidatePairStats(
            stats.cast_to<RTCIceCandidatePairStats>(),
            stats.id() == selected_candidate_pair_id);
      } else if (stats.type() == RTCLocalIceCandidateStats::kType) {
        verify_successful &= VerifyRTCLocalIceCandidateStats(
            stats.cast_to<RTCLocalIceCandidateStats>());
      } else if (stats.type() == RTCRemoteIceCandidateStats::kType) {
        verify_successful &= VerifyRTCRemoteIceCandidateStats(
            stats.cast_to<RTCRemoteIceCandidateStats>());
      } else if (stats.type() == RTCMediaStreamStats::kType) {
        verify_successful &=
            VerifyRTCMediaStreamStats(stats.cast_to<RTCMediaStreamStats>());
      } else if (stats.type() == RTCMediaStreamTrackStats::kType) {
        verify_successful &= VerifyRTCMediaStreamTrackStats(
            stats.cast_to<RTCMediaStreamTrackStats>());
      } else if (stats.type() == RTCPeerConnectionStats::kType) {
        verify_successful &= VerifyRTCPeerConnectionStats(
            stats.cast_to<RTCPeerConnectionStats>());
      } else if (stats.type() == RTCInboundRTPStreamStats::kType) {
        verify_successful &= VerifyRTCInboundRTPStreamStats(
            stats.cast_to<RTCInboundRTPStreamStats>());
      } else if (stats.type() == RTCOutboundRTPStreamStats::kType) {
        verify_successful &= VerifyRTCOutboundRTPStreamStats(
            stats.cast_to<RTCOutboundRTPStreamStats>());
      } else if (stats.type() == RTCTransportStats::kType) {
        verify_successful &=
            VerifyRTCTransportStats(stats.cast_to<RTCTransportStats>());
      } else {
        EXPECT_TRUE(false) << "Unrecognized stats type: " << stats.type();
        verify_successful = false;
      }
    }
    for (const char* missing : missing_stats) {
      if (!absl::c_linear_search(allowed_missing_stats, missing)) {
        verify_successful = false;
        EXPECT_TRUE(false) << "Missing expected stats type: " << missing;
      }
    }
    EXPECT_TRUE(verify_successful)
        << "One or more problems with the stats. This is the report:\n"
        << report_->ToJson();
  }

  bool VerifyRTCCertificateStats(const RTCCertificateStats& certificate) {
    RTCStatsVerifier verifier(report_, &certificate);
    verifier.TestMemberIsDefined(certificate.fingerprint);
    verifier.TestMemberIsDefined(certificate.fingerprint_algorithm);
    verifier.TestMemberIsDefined(certificate.base64_certificate);
    verifier.TestMemberIsOptionalIDReference(certificate.issuer_certificate_id,
                                             RTCCertificateStats::kType);
    return verifier.ExpectAllMembersSuccessfullyTested();
  }

  bool VerifyRTCCodecStats(const RTCCodecStats& codec) {
    RTCStatsVerifier verifier(report_, &codec);
    verifier.TestMemberIsDefined(codec.payload_type);
    verifier.TestMemberIsDefined(codec.mime_type);
    verifier.TestMemberIsPositive<uint32_t>(codec.clock_rate);
    verifier.TestMemberIsUndefined(codec.channels);
    verifier.TestMemberIsUndefined(codec.sdp_fmtp_line);
    verifier.TestMemberIsUndefined(codec.implementation);
    return verifier.ExpectAllMembersSuccessfullyTested();
  }

  bool VerifyRTCDataChannelStats(const RTCDataChannelStats& data_channel) {
    RTCStatsVerifier verifier(report_, &data_channel);
    verifier.TestMemberIsDefined(data_channel.label);
    verifier.TestMemberIsDefined(data_channel.protocol);
    verifier.TestMemberIsDefined(data_channel.datachannelid);
    verifier.TestMemberIsDefined(data_channel.state);
    verifier.TestMemberIsNonNegative<uint32_t>(data_channel.messages_sent);
    verifier.TestMemberIsNonNegative<uint64_t>(data_channel.bytes_sent);
    verifier.TestMemberIsNonNegative<uint32_t>(data_channel.messages_received);
    verifier.TestMemberIsNonNegative<uint64_t>(data_channel.bytes_received);
    return verifier.ExpectAllMembersSuccessfullyTested();
  }

  bool VerifyRTCIceCandidatePairStats(
      const RTCIceCandidatePairStats& candidate_pair,
      bool is_selected_pair) {
    RTCStatsVerifier verifier(report_, &candidate_pair);
    verifier.TestMemberIsIDReference(candidate_pair.transport_id,
                                     RTCTransportStats::kType);
    verifier.TestMemberIsIDReference(candidate_pair.local_candidate_id,
                                     RTCLocalIceCandidateStats::kType);
    verifier.TestMemberIsIDReference(candidate_pair.remote_candidate_id,
                                     RTCRemoteIceCandidateStats::kType);
    verifier.TestMemberIsDefined(candidate_pair.state);
    verifier.TestMemberIsNonNegative<uint64_t>(candidate_pair.priority);
    verifier.TestMemberIsDefined(candidate_pair.nominated);
    verifier.TestMemberIsDefined(candidate_pair.writable);
    verifier.TestMemberIsUndefined(candidate_pair.readable);
    verifier.TestMemberIsNonNegative<uint64_t>(candidate_pair.bytes_sent);
    verifier.TestMemberIsNonNegative<uint64_t>(candidate_pair.bytes_received);
    verifier.TestMemberIsNonNegative<double>(
        candidate_pair.total_round_trip_time);
    verifier.TestMemberIsNonNegative<double>(
        candidate_pair.current_round_trip_time);
    if (is_selected_pair) {
      verifier.TestMemberIsNonNegative<double>(
          candidate_pair.available_outgoing_bitrate);
      // A pair should be nominated in order to be selected.
      EXPECT_TRUE(*candidate_pair.nominated);
    } else {
      verifier.TestMemberIsUndefined(candidate_pair.available_outgoing_bitrate);
    }
    verifier.TestMemberIsUndefined(candidate_pair.available_incoming_bitrate);
    verifier.TestMemberIsNonNegative<uint64_t>(
        candidate_pair.requests_received);
    verifier.TestMemberIsNonNegative<uint64_t>(candidate_pair.requests_sent);
    verifier.TestMemberIsNonNegative<uint64_t>(
        candidate_pair.responses_received);
    verifier.TestMemberIsNonNegative<uint64_t>(candidate_pair.responses_sent);
    verifier.TestMemberIsUndefined(candidate_pair.retransmissions_received);
    verifier.TestMemberIsUndefined(candidate_pair.retransmissions_sent);
    verifier.TestMemberIsUndefined(candidate_pair.consent_requests_received);
    verifier.TestMemberIsNonNegative<uint64_t>(
        candidate_pair.consent_requests_sent);
    verifier.TestMemberIsUndefined(candidate_pair.consent_responses_received);
    verifier.TestMemberIsUndefined(candidate_pair.consent_responses_sent);
    return verifier.ExpectAllMembersSuccessfullyTested();
  }

  bool VerifyRTCIceCandidateStats(const RTCIceCandidateStats& candidate) {
    RTCStatsVerifier verifier(report_, &candidate);
    verifier.TestMemberIsIDReference(candidate.transport_id,
                                     RTCTransportStats::kType);
    verifier.TestMemberIsDefined(candidate.is_remote);
    if (*candidate.is_remote) {
      verifier.TestMemberIsUndefined(candidate.network_type);
    } else {
      verifier.TestMemberIsDefined(candidate.network_type);
    }
    verifier.TestMemberIsDefined(candidate.ip);
    verifier.TestMemberIsNonNegative<int32_t>(candidate.port);
    verifier.TestMemberIsDefined(candidate.protocol);
    verifier.TestMemberIsDefined(candidate.candidate_type);
    verifier.TestMemberIsNonNegative<int32_t>(candidate.priority);
    verifier.TestMemberIsUndefined(candidate.url);
    verifier.TestMemberIsDefined(candidate.deleted);
    verifier.TestMemberIsUndefined(candidate.relay_protocol);
    return verifier.ExpectAllMembersSuccessfullyTested();
  }

  bool VerifyRTCLocalIceCandidateStats(
      const RTCLocalIceCandidateStats& local_candidate) {
    return VerifyRTCIceCandidateStats(local_candidate);
  }

  bool VerifyRTCRemoteIceCandidateStats(
      const RTCRemoteIceCandidateStats& remote_candidate) {
    return VerifyRTCIceCandidateStats(remote_candidate);
  }

  bool VerifyRTCMediaStreamStats(const RTCMediaStreamStats& media_stream) {
    RTCStatsVerifier verifier(report_, &media_stream);
    verifier.TestMemberIsDefined(media_stream.stream_identifier);
    verifier.TestMemberIsIDReference(media_stream.track_ids,
                                     RTCMediaStreamTrackStats::kType);
    return verifier.ExpectAllMembersSuccessfullyTested();
  }

  bool VerifyRTCMediaStreamTrackStats(
      const RTCMediaStreamTrackStats& media_stream_track) {
    RTCStatsVerifier verifier(report_, &media_stream_track);
    verifier.TestMemberIsDefined(media_stream_track.track_identifier);
    verifier.TestMemberIsDefined(media_stream_track.remote_source);
    verifier.TestMemberIsDefined(media_stream_track.ended);
    verifier.TestMemberIsDefined(media_stream_track.detached);
    verifier.TestMemberIsDefined(media_stream_track.kind);
    // Video or audio media stream track?
    if (*media_stream_track.kind == RTCMediaStreamTrackKind::kVideo) {
      // Video-only members
      verifier.TestMemberIsNonNegative<uint32_t>(
          media_stream_track.frame_width);
      verifier.TestMemberIsNonNegative<uint32_t>(
          media_stream_track.frame_height);
      verifier.TestMemberIsUndefined(media_stream_track.frames_per_second);
      if (*media_stream_track.remote_source) {
        verifier.TestMemberIsUndefined(media_stream_track.frames_sent);
        verifier.TestMemberIsUndefined(media_stream_track.huge_frames_sent);
        verifier.TestMemberIsNonNegative<uint32_t>(
            media_stream_track.frames_received);
        verifier.TestMemberIsNonNegative<uint32_t>(
            media_stream_track.frames_decoded);
        verifier.TestMemberIsNonNegative<uint32_t>(
            media_stream_track.frames_dropped);
        verifier.TestMemberIsNonNegative<uint32_t>(
            media_stream_track.freeze_count);
        verifier.TestMemberIsNonNegative<uint32_t>(
            media_stream_track.pause_count);
        verifier.TestMemberIsNonNegative<double>(
            media_stream_track.total_freezes_duration);
        verifier.TestMemberIsNonNegative<double>(
            media_stream_track.total_pauses_duration);
        verifier.TestMemberIsNonNegative<double>(
            media_stream_track.total_frames_duration);
        verifier.TestMemberIsNonNegative<double>(
            media_stream_track.sum_squared_frame_durations);
      } else {
        verifier.TestMemberIsNonNegative<uint32_t>(
            media_stream_track.frames_sent);
        verifier.TestMemberIsNonNegative<uint32_t>(
            media_stream_track.huge_frames_sent);
        verifier.TestMemberIsUndefined(media_stream_track.frames_received);
        verifier.TestMemberIsUndefined(media_stream_track.frames_decoded);
        verifier.TestMemberIsUndefined(media_stream_track.frames_dropped);
        verifier.TestMemberIsUndefined(media_stream_track.freeze_count);
        verifier.TestMemberIsUndefined(media_stream_track.pause_count);
        verifier.TestMemberIsUndefined(
            media_stream_track.total_freezes_duration);
        verifier.TestMemberIsUndefined(
            media_stream_track.total_pauses_duration);
        verifier.TestMemberIsUndefined(
            media_stream_track.total_frames_duration);
        verifier.TestMemberIsUndefined(
            media_stream_track.sum_squared_frame_durations);
      }
      verifier.TestMemberIsUndefined(media_stream_track.frames_corrupted);
      verifier.TestMemberIsUndefined(media_stream_track.partial_frames_lost);
      verifier.TestMemberIsUndefined(media_stream_track.full_frames_lost);
      // Audio-only members should be undefined
      verifier.TestMemberIsUndefined(media_stream_track.audio_level);
      verifier.TestMemberIsUndefined(media_stream_track.echo_return_loss);
      verifier.TestMemberIsUndefined(
          media_stream_track.echo_return_loss_enhancement);
      verifier.TestMemberIsUndefined(media_stream_track.total_audio_energy);
      verifier.TestMemberIsUndefined(media_stream_track.total_samples_duration);
    } else {
      RTC_DCHECK_EQ(*media_stream_track.kind, RTCMediaStreamTrackKind::kAudio);
      // Video-only members should be undefined
      verifier.TestMemberIsUndefined(media_stream_track.frame_width);
      verifier.TestMemberIsUndefined(media_stream_track.frame_height);
      verifier.TestMemberIsUndefined(media_stream_track.frames_per_second);
      verifier.TestMemberIsUndefined(media_stream_track.frames_sent);
      verifier.TestMemberIsUndefined(media_stream_track.huge_frames_sent);
      verifier.TestMemberIsUndefined(media_stream_track.frames_received);
      verifier.TestMemberIsUndefined(media_stream_track.frames_decoded);
      verifier.TestMemberIsUndefined(media_stream_track.frames_dropped);
      verifier.TestMemberIsUndefined(media_stream_track.frames_corrupted);
      verifier.TestMemberIsUndefined(media_stream_track.partial_frames_lost);
      verifier.TestMemberIsUndefined(media_stream_track.full_frames_lost);
      verifier.TestMemberIsUndefined(media_stream_track.freeze_count);
      verifier.TestMemberIsUndefined(media_stream_track.pause_count);
      verifier.TestMemberIsUndefined(media_stream_track.total_freezes_duration);
      verifier.TestMemberIsUndefined(media_stream_track.total_pauses_duration);
      verifier.TestMemberIsUndefined(media_stream_track.total_frames_duration);
      verifier.TestMemberIsUndefined(
          media_stream_track.sum_squared_frame_durations);
      // Audio-only members
      verifier.TestMemberIsNonNegative<double>(media_stream_track.audio_level);
      verifier.TestMemberIsNonNegative<double>(
          media_stream_track.total_audio_energy);
      verifier.TestMemberIsNonNegative<double>(
          media_stream_track.total_samples_duration);
      // TODO(hbos): |echo_return_loss| and |echo_return_loss_enhancement| are
      // flaky on msan bot (sometimes defined, sometimes undefined). Should the
      // test run until available or is there a way to have it always be
      // defined? crbug.com/627816
      verifier.MarkMemberTested(media_stream_track.echo_return_loss, true);
      verifier.MarkMemberTested(media_stream_track.echo_return_loss_enhancement,
                                true);
    }
    // totalSamplesReceived, concealedSamples and concealmentEvents are only
    // present on inbound audio tracks.
    // jitterBufferDelay is currently only implemented for audio.
    if (*media_stream_track.kind == RTCMediaStreamTrackKind::kAudio &&
        *media_stream_track.remote_source) {
      verifier.TestMemberIsNonNegative<double>(
          media_stream_track.jitter_buffer_delay);
      verifier.TestMemberIsNonNegative<uint64_t>(
          media_stream_track.jitter_buffer_emitted_count);
      verifier.TestMemberIsNonNegative<uint64_t>(
          media_stream_track.total_samples_received);
      verifier.TestMemberIsNonNegative<uint64_t>(
          media_stream_track.concealed_samples);
      verifier.TestMemberIsNonNegative<uint64_t>(
          media_stream_track.concealment_events);
      verifier.TestMemberIsNonNegative<uint64_t>(
          media_stream_track.inserted_samples_for_deceleration);
      verifier.TestMemberIsNonNegative<uint64_t>(
          media_stream_track.removed_samples_for_acceleration);
      verifier.TestMemberIsNonNegative<uint64_t>(
          media_stream_track.silent_concealed_samples);
      verifier.TestMemberIsNonNegative<uint64_t>(
          media_stream_track.jitter_buffer_flushes);
      verifier.TestMemberIsNonNegative<uint64_t>(
          media_stream_track.delayed_packet_outage_samples);
      verifier.TestMemberIsNonNegative<double>(
          media_stream_track.relative_packet_arrival_delay);
      verifier.TestMemberIsNonNegative<uint32_t>(
          media_stream_track.interruption_count);
      verifier.TestMemberIsNonNegative<double>(
          media_stream_track.total_interruption_duration);
    } else {
      verifier.TestMemberIsUndefined(media_stream_track.jitter_buffer_delay);
      verifier.TestMemberIsUndefined(
          media_stream_track.jitter_buffer_emitted_count);
      verifier.TestMemberIsUndefined(media_stream_track.total_samples_received);
      verifier.TestMemberIsUndefined(media_stream_track.concealed_samples);
      verifier.TestMemberIsUndefined(media_stream_track.concealment_events);
      verifier.TestMemberIsUndefined(media_stream_track.jitter_buffer_flushes);
      verifier.TestMemberIsUndefined(
          media_stream_track.delayed_packet_outage_samples);
      verifier.TestMemberIsUndefined(
          media_stream_track.relative_packet_arrival_delay);
      verifier.TestMemberIsUndefined(media_stream_track.interruption_count);
      verifier.TestMemberIsUndefined(
          media_stream_track.total_interruption_duration);
    }
    return verifier.ExpectAllMembersSuccessfullyTested();
  }

  bool VerifyRTCPeerConnectionStats(
      const RTCPeerConnectionStats& peer_connection) {
    RTCStatsVerifier verifier(report_, &peer_connection);
    verifier.TestMemberIsNonNegative<uint32_t>(
        peer_connection.data_channels_opened);
    verifier.TestMemberIsNonNegative<uint32_t>(
        peer_connection.data_channels_closed);
    return verifier.ExpectAllMembersSuccessfullyTested();
  }

  void VerifyRTCRTPStreamStats(const RTCRTPStreamStats& stream,
                               RTCStatsVerifier* verifier) {
    verifier->TestMemberIsDefined(stream.ssrc);
    verifier->TestMemberIsUndefined(stream.associate_stats_id);
    verifier->TestMemberIsDefined(stream.is_remote);
    verifier->TestMemberIsDefined(stream.media_type);
    verifier->TestMemberIsDefined(stream.kind);
    verifier->TestMemberIsIDReference(stream.track_id,
                                      RTCMediaStreamTrackStats::kType);
    verifier->TestMemberIsIDReference(stream.transport_id,
                                      RTCTransportStats::kType);
    verifier->TestMemberIsIDReference(stream.codec_id, RTCCodecStats::kType);
    if (stream.media_type.is_defined() && *stream.media_type == "video") {
      verifier->TestMemberIsNonNegative<uint32_t>(stream.fir_count);
      verifier->TestMemberIsNonNegative<uint32_t>(stream.pli_count);
      verifier->TestMemberIsNonNegative<uint32_t>(stream.nack_count);
    } else {
      verifier->TestMemberIsUndefined(stream.fir_count);
      verifier->TestMemberIsUndefined(stream.pli_count);
      verifier->TestMemberIsUndefined(stream.nack_count);
    }
    verifier->TestMemberIsUndefined(stream.sli_count);
  }

  bool VerifyRTCInboundRTPStreamStats(
      const RTCInboundRTPStreamStats& inbound_stream) {
    RTCStatsVerifier verifier(report_, &inbound_stream);
    VerifyRTCRTPStreamStats(inbound_stream, &verifier);
    if (inbound_stream.media_type.is_defined() &&
        *inbound_stream.media_type == "video") {
      verifier.TestMemberIsNonNegative<uint64_t>(inbound_stream.qp_sum);
    } else {
      verifier.TestMemberIsUndefined(inbound_stream.qp_sum);
    }
    verifier.TestMemberIsNonNegative<uint32_t>(inbound_stream.packets_received);
    if (inbound_stream.media_type.is_defined() &&
        *inbound_stream.media_type == "audio") {
      verifier.TestMemberIsNonNegative<uint64_t>(
          inbound_stream.fec_packets_received);
      verifier.TestMemberIsNonNegative<uint64_t>(
          inbound_stream.fec_packets_discarded);
    }
    verifier.TestMemberIsNonNegative<uint64_t>(inbound_stream.bytes_received);
    // packets_lost is defined as signed, but this should never happen in
    // this test. See RFC 3550.
    verifier.TestMemberIsNonNegative<int32_t>(inbound_stream.packets_lost);
    verifier.TestMemberIsDefined(inbound_stream.last_packet_received_timestamp);
    if (inbound_stream.media_type.is_defined() &&
        *inbound_stream.media_type == "video") {
      verifier.TestMemberIsUndefined(inbound_stream.jitter);
    } else {
      verifier.TestMemberIsNonNegative<double>(inbound_stream.jitter);
    }
    verifier.TestMemberIsNonNegative<double>(inbound_stream.fraction_lost);
    verifier.TestMemberIsUndefined(inbound_stream.round_trip_time);
    verifier.TestMemberIsUndefined(inbound_stream.packets_discarded);
    verifier.TestMemberIsUndefined(inbound_stream.packets_repaired);
    verifier.TestMemberIsUndefined(inbound_stream.burst_packets_lost);
    verifier.TestMemberIsUndefined(inbound_stream.burst_packets_discarded);
    verifier.TestMemberIsUndefined(inbound_stream.burst_loss_count);
    verifier.TestMemberIsUndefined(inbound_stream.burst_discard_count);
    verifier.TestMemberIsUndefined(inbound_stream.burst_loss_rate);
    verifier.TestMemberIsUndefined(inbound_stream.burst_discard_rate);
    verifier.TestMemberIsUndefined(inbound_stream.gap_loss_rate);
    verifier.TestMemberIsUndefined(inbound_stream.gap_discard_rate);
    if (inbound_stream.media_type.is_defined() &&
        *inbound_stream.media_type == "video") {
      verifier.TestMemberIsDefined(inbound_stream.frames_decoded);
      // The integration test is not set up to test screen share; don't require
      // this to be present.
      verifier.MarkMemberTested(inbound_stream.content_type, true);
    } else {
      verifier.TestMemberIsUndefined(inbound_stream.frames_decoded);
      verifier.TestMemberIsUndefined(inbound_stream.content_type);
    }
    return verifier.ExpectAllMembersSuccessfullyTested();
  }

  bool VerifyRTCOutboundRTPStreamStats(
      const RTCOutboundRTPStreamStats& outbound_stream) {
    RTCStatsVerifier verifier(report_, &outbound_stream);
    VerifyRTCRTPStreamStats(outbound_stream, &verifier);
    if (outbound_stream.media_type.is_defined() &&
        *outbound_stream.media_type == "video") {
      verifier.TestMemberIsNonNegative<uint64_t>(outbound_stream.qp_sum);
    } else {
      verifier.TestMemberIsUndefined(outbound_stream.qp_sum);
    }
    verifier.TestMemberIsNonNegative<uint32_t>(outbound_stream.packets_sent);
    verifier.TestMemberIsNonNegative<uint64_t>(
        outbound_stream.retransmitted_packets_sent);
    verifier.TestMemberIsNonNegative<uint64_t>(outbound_stream.bytes_sent);
    verifier.TestMemberIsNonNegative<uint64_t>(
        outbound_stream.retransmitted_bytes_sent);
    verifier.TestMemberIsUndefined(outbound_stream.target_bitrate);
    if (outbound_stream.media_type.is_defined() &&
        *outbound_stream.media_type == "video") {
      verifier.TestMemberIsDefined(outbound_stream.frames_encoded);
      verifier.TestMemberIsNonNegative<double>(
          outbound_stream.total_encode_time);
      verifier.TestMemberIsNonNegative<double>(
          outbound_stream.total_packet_send_delay);
      // The integration test is not set up to test screen share; don't require
      // this to be present.
      verifier.MarkMemberTested(outbound_stream.content_type, true);
    } else {
      verifier.TestMemberIsUndefined(outbound_stream.frames_encoded);
      verifier.TestMemberIsUndefined(outbound_stream.total_encode_time);
      // TODO(https://crbug.com/webrtc/10635): Implement for audio as well.
      verifier.TestMemberIsUndefined(outbound_stream.total_packet_send_delay);
      verifier.TestMemberIsUndefined(outbound_stream.content_type);
    }
    return verifier.ExpectAllMembersSuccessfullyTested();
  }

  bool VerifyRTCTransportStats(const RTCTransportStats& transport) {
    RTCStatsVerifier verifier(report_, &transport);
    verifier.TestMemberIsNonNegative<uint64_t>(transport.bytes_sent);
    verifier.TestMemberIsNonNegative<uint64_t>(transport.bytes_received);
    verifier.TestMemberIsOptionalIDReference(transport.rtcp_transport_stats_id,
                                             RTCTransportStats::kType);
    verifier.TestMemberIsDefined(transport.dtls_state);
    verifier.TestMemberIsIDReference(transport.selected_candidate_pair_id,
                                     RTCIceCandidatePairStats::kType);
    verifier.TestMemberIsIDReference(transport.local_certificate_id,
                                     RTCCertificateStats::kType);
    verifier.TestMemberIsIDReference(transport.remote_certificate_id,
                                     RTCCertificateStats::kType);
    return verifier.ExpectAllMembersSuccessfullyTested();
  }

 private:
  rtc::scoped_refptr<const RTCStatsReport> report_;
};

#ifdef HAVE_SCTP
TEST_F(RTCStatsIntegrationTest, GetStatsFromCaller) {
  StartCall();

  rtc::scoped_refptr<const RTCStatsReport> report = GetStatsFromCaller();
  RTCStatsReportVerifier(report.get()).VerifyReport({});
  EXPECT_EQ(report->ToJson(), RTCStatsReportTraceListener::last_trace());
}

TEST_F(RTCStatsIntegrationTest, GetStatsFromCallee) {
  StartCall();

  rtc::scoped_refptr<const RTCStatsReport> report = GetStatsFromCallee();
  RTCStatsReportVerifier(report.get()).VerifyReport({});
  EXPECT_EQ(report->ToJson(), RTCStatsReportTraceListener::last_trace());
}

// These tests exercise the integration of the stats selection algorithm inside
// of PeerConnection. See rtcstatstraveral_unittest.cc for more detailed stats
// traversal tests on particular stats graphs.
TEST_F(RTCStatsIntegrationTest, GetStatsWithSenderSelector) {
  StartCall();
  ASSERT_FALSE(caller_->pc()->GetSenders().empty());
  rtc::scoped_refptr<const RTCStatsReport> report =
      GetStatsFromCaller(caller_->pc()->GetSenders()[0]);
  std::vector<const char*> allowed_missing_stats = {
      // TODO(hbos): Include RTC[Audio/Video]ReceiverStats when implemented.
      // TODO(hbos): Include RTCRemoteOutboundRtpStreamStats when implemented.
      // TODO(hbos): Include RTCRtpContributingSourceStats when implemented.
      RTCInboundRTPStreamStats::kType, RTCPeerConnectionStats::kType,
      RTCMediaStreamStats::kType, RTCDataChannelStats::kType,
  };
  RTCStatsReportVerifier(report.get()).VerifyReport(allowed_missing_stats);
  EXPECT_TRUE(report->size());
}

TEST_F(RTCStatsIntegrationTest, GetStatsWithReceiverSelector) {
  StartCall();

  ASSERT_FALSE(caller_->pc()->GetReceivers().empty());
  rtc::scoped_refptr<const RTCStatsReport> report =
      GetStatsFromCaller(caller_->pc()->GetReceivers()[0]);
  std::vector<const char*> allowed_missing_stats = {
      // TODO(hbos): Include RTC[Audio/Video]SenderStats when implemented.
      // TODO(hbos): Include RTCRemoteInboundRtpStreamStats when implemented.
      // TODO(hbos): Include RTCRtpContributingSourceStats when implemented.
      RTCOutboundRTPStreamStats::kType, RTCPeerConnectionStats::kType,
      RTCMediaStreamStats::kType, RTCDataChannelStats::kType,
  };
  RTCStatsReportVerifier(report.get()).VerifyReport(allowed_missing_stats);
  EXPECT_TRUE(report->size());
}

TEST_F(RTCStatsIntegrationTest, GetStatsWithInvalidSenderSelector) {
  StartCall();

  ASSERT_FALSE(callee_->pc()->GetSenders().empty());
  // The selector is invalid for the caller because it belongs to the callee.
  auto invalid_selector = callee_->pc()->GetSenders()[0];
  rtc::scoped_refptr<const RTCStatsReport> report =
      GetStatsFromCaller(invalid_selector);
  EXPECT_FALSE(report->size());
}

TEST_F(RTCStatsIntegrationTest, GetStatsWithInvalidReceiverSelector) {
  StartCall();

  ASSERT_FALSE(callee_->pc()->GetReceivers().empty());
  // The selector is invalid for the caller because it belongs to the callee.
  auto invalid_selector = callee_->pc()->GetReceivers()[0];
  rtc::scoped_refptr<const RTCStatsReport> report =
      GetStatsFromCaller(invalid_selector);
  EXPECT_FALSE(report->size());
}

// TODO(bugs.webrtc.org/10041) For now this is equivalent to the following
// test GetsStatsWhileClosingPeerConnection, because pc() is closed by
// PeerConnectionTestWrapper. See: bugs.webrtc.org/9847
TEST_F(RTCStatsIntegrationTest,
       DISABLED_GetStatsWhileDestroyingPeerConnection) {
  StartCall();

  rtc::scoped_refptr<RTCStatsObtainer> stats_obtainer =
      RTCStatsObtainer::Create();
  caller_->pc()->GetStats(stats_obtainer);
  // This will destroy the peer connection.
  caller_ = nullptr;
  // Any pending stats requests should have completed in the act of destroying
  // the peer connection.
  ASSERT_TRUE(stats_obtainer->report());
  EXPECT_EQ(stats_obtainer->report()->ToJson(),
            RTCStatsReportTraceListener::last_trace());
}

TEST_F(RTCStatsIntegrationTest, GetsStatsWhileClosingPeerConnection) {
  StartCall();

  rtc::scoped_refptr<RTCStatsObtainer> stats_obtainer =
      RTCStatsObtainer::Create();
  caller_->pc()->GetStats(stats_obtainer);
  caller_->pc()->Close();

  ASSERT_TRUE(stats_obtainer->report());
  EXPECT_EQ(stats_obtainer->report()->ToJson(),
            RTCStatsReportTraceListener::last_trace());
}

// GetStatsReferencedIds() is optimized to recognize what is or isn't a
// referenced ID based on dictionary type information and knowing what members
// are used as references, as opposed to iterating all members to find the ones
// with the "Id" or "Ids" suffix. As such, GetStatsReferencedIds() is tested as
// an integration test instead of a unit test in order to guard against adding
// new references and forgetting to update GetStatsReferencedIds().
TEST_F(RTCStatsIntegrationTest, GetStatsReferencedIds) {
  StartCall();

  rtc::scoped_refptr<const RTCStatsReport> report = GetStatsFromCallee();
  for (const RTCStats& stats : *report) {
    // Find all references by looking at all string members with the "Id" or
    // "Ids" suffix.
    std::set<const std::string*> expected_ids;
    for (const auto* member : stats.Members()) {
      if (!member->is_defined())
        continue;
      if (member->type() == RTCStatsMemberInterface::kString) {
        if (absl::EndsWith(member->name(), "Id")) {
          const auto& id = member->cast_to<const RTCStatsMember<std::string>>();
          expected_ids.insert(&(*id));
        }
      } else if (member->type() == RTCStatsMemberInterface::kSequenceString) {
        if (absl::EndsWith(member->name(), "Ids")) {
          const auto& ids =
              member->cast_to<const RTCStatsMember<std::vector<std::string>>>();
          for (const std::string& id : *ids)
            expected_ids.insert(&id);
        }
      }
    }

    std::vector<const std::string*> neighbor_ids = GetStatsReferencedIds(stats);
    EXPECT_EQ(neighbor_ids.size(), expected_ids.size());
    for (const std::string* neighbor_id : neighbor_ids) {
      EXPECT_THAT(expected_ids, Contains(neighbor_id));
    }
    for (const std::string* expected_id : expected_ids) {
      EXPECT_THAT(neighbor_ids, Contains(expected_id));
    }
  }
}
#endif  // HAVE_SCTP

}  // namespace

}  // namespace webrtc
