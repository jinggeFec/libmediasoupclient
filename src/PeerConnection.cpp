#define MSC_CLASS "PeerConnection"
// #define MSC_LOG_DEV

#include "PeerConnection.hpp"
#include "Exception.hpp"
#include "Logger.hpp"
#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/video_codecs/builtin_video_decoder_factory.h"
#include "api/video_codecs/builtin_video_encoder_factory.h"
#include "rtc_base/ssladapter.h"
#include <utility>

namespace mediasoupclient
{
/*
 * Helper to convert webrtc::RtpCapabilities to nlohmann::json
 */
void webrtcRtpCapabilities2Json(
  cricket::MediaType type, webrtc::RtpCapabilities nativeCapabilities, json& capabilities)
{
	if (type != cricket::MEDIA_TYPE_AUDIO && type != cricket::MEDIA_TYPE_VIDEO)
		return;

	auto kind = cricket::MediaTypeToString(type);

	for (auto& nativeCodec : nativeCapabilities.codecs)
	{
		if (nativeCodec.kind != cricket::MEDIA_TYPE_AUDIO && nativeCodec.kind != cricket::MEDIA_TYPE_VIDEO)
			continue;

		// clang-format off
		json codec =
		{
			{ "name",                 nativeCodec.name                           },
			{ "mimeType",             nativeCodec.mime_type()                    },
			{ "kind",                 kind                                       },
			{ "clockRate",            nativeCodec.clock_rate.value()             },
			{ "preferredPayloadType", nativeCodec.preferred_payload_type.value() },
			{ "rtcpFeedback",         json::array()                              },
			{ "parameters",           json::object()                             }
		};
		// clang-format on

		if (nativeCodec.num_channels && nativeCodec.num_channels > 1)
			codec["channels"] = nativeCodec.num_channels.value();

		for (auto& kv : nativeCodec.parameters)
			codec["parameters"].push_back({ kv.first, kv.second });

		for (auto& nativeRtcpFeedback : nativeCodec.rtcp_feedback)
		{
			json rtcpFeedback = json::object();
			std::string type;
			std::string messageType;

			switch (nativeRtcpFeedback.type)
			{
				case webrtc::RtcpFeedbackType::CCM:
				{
					type = "ccm";
					break;
				}
				case webrtc::RtcpFeedbackType::NACK:
				{
					type = "nack";
					break;
				}
				case webrtc::RtcpFeedbackType::REMB:
				{
					type = "goog-remb";
					break;
				}
				case webrtc::RtcpFeedbackType::TRANSPORT_CC:
				{
					break;
				}
			}

			if (type.empty())
				continue;

			rtcpFeedback["type"] = type;

			if (nativeRtcpFeedback.message_type)
			{
				switch (nativeRtcpFeedback.message_type.value())
				{
					case webrtc::RtcpFeedbackMessageType::GENERIC_NACK:
					{
						break;
					}
					case webrtc::RtcpFeedbackMessageType::PLI:
					{
						messageType = "pli";
						break;
					}
					case webrtc::RtcpFeedbackMessageType::FIR:
					{
						messageType = "fir";
						break;
					}
				}
			}

			if (!messageType.empty())
				rtcpFeedback["parameter"] = messageType;

			codec["rtcpFeedback"].push_back(rtcpFeedback);
		}

		capabilities["codecs"].push_back(codec);
	}

	for (auto& nativeHeaderExtension : nativeCapabilities.header_extensions)
	{
		/* clang-format off */
		json headerExtension =
		{
			{ "kind", kind                      },
			{ "uri",  nativeHeaderExtension.uri }
		};
		/* clang-format on */

		if (nativeHeaderExtension.preferred_id)
			headerExtension["preferredId"] = nativeHeaderExtension.preferred_id.value();

		capabilities["headerExtensions"].push_back(headerExtension);
	}
}

/* Static. */

/* clang-format off */
std::map<PeerConnection::SdpType, const std::string> PeerConnection::sdpType2String =
{
	{ PeerConnection::SdpType::OFFER,    "offer"    },
	{ PeerConnection::SdpType::PRANSWER, "pranswer" },
	{ PeerConnection::SdpType::ANSWER,   "answer"   }
};

std::map<webrtc::PeerConnectionInterface::IceConnectionState, const std::string>
	PeerConnection::iceConnectionState2String =
{
	{ webrtc::PeerConnectionInterface::IceConnectionState::kIceConnectionNew,          "new"          },
	{ webrtc::PeerConnectionInterface::IceConnectionState::kIceConnectionChecking,     "cheking"      },
	{ webrtc::PeerConnectionInterface::IceConnectionState::kIceConnectionConnected,    "connected"    },
	{ webrtc::PeerConnectionInterface::IceConnectionState::kIceConnectionCompleted,    "conmpleted"   },
	{ webrtc::PeerConnectionInterface::IceConnectionState::kIceConnectionFailed,       "failed"       },
	{ webrtc::PeerConnectionInterface::IceConnectionState::kIceConnectionDisconnected, "disconnected" },
	{ webrtc::PeerConnectionInterface::IceConnectionState::kIceConnectionClosed,       "closed"       }
};

std::map<webrtc::PeerConnectionInterface::IceGatheringState, const std::string>
	PeerConnection::iceGatheringState2String =
{
	{ webrtc::PeerConnectionInterface::IceGatheringState::kIceGatheringNew,       "new"       },
	{ webrtc::PeerConnectionInterface::IceGatheringState::kIceGatheringGathering, "gathering" },
	{ webrtc::PeerConnectionInterface::IceGatheringState::kIceGatheringComplete,  "complete"  }
};

std::map<webrtc::PeerConnectionInterface::SignalingState, const std::string>
	PeerConnection::signalingState2String =
{
	{ webrtc::PeerConnectionInterface::SignalingState::kStable,             "stable"               },
	{ webrtc::PeerConnectionInterface::SignalingState::kHaveLocalOffer,     "have-local-offer"     },
	{ webrtc::PeerConnectionInterface::SignalingState::kHaveLocalPrAnswer,  "have-local-pranswer"  },
	{ webrtc::PeerConnectionInterface::SignalingState::kHaveRemoteOffer,    "have-remote-offer"    },
	{ webrtc::PeerConnectionInterface::SignalingState::kHaveRemotePrAnswer, "have-remote-pranswer" },
	{ webrtc::PeerConnectionInterface::SignalingState::kClosed,             "closed"               }
};
/* clang-format on */

void PeerConnection::ClassInit()
{
	MSC_TRACE();

	// rtc::LogMessage::LogToDebug(rtc::LoggingSeverity::LS_WARNING);

	rtc::InitializeSSL();
	rtc::InitRandom(rtc::Time());
}

void PeerConnection::ClassCleanup()
{
	MSC_TRACE();

	rtc::CleanupSSL();
}

/* Instance methods. */

PeerConnection::PeerConnection(PeerConnection::Listener* listener, PeerConnection::Options* options)
  : listener(listener)
{
	MSC_TRACE();

	webrtc::PeerConnectionInterface::RTCConfiguration config;

	if (options)
		config = options->config;

	// PeerConnection factory provided.
	if (options && options->factory)
	{
		this->peerConnectionFactory =
		  rtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface>(options->factory);
	}
	else
	{
		this->signalingThread = new rtc::Thread();
		this->workerThread    = new rtc::Thread();

		this->signalingThread->SetName("signaling_thread", nullptr);
		this->workerThread->SetName("worker_thread", nullptr);

		if (!this->signalingThread->Start() || !this->workerThread->Start())
		{
			throw Exception("Thread start errored");
		}

		this->peerConnectionFactory = webrtc::CreatePeerConnectionFactory(
		  this->workerThread,
		  this->workerThread,
		  this->signalingThread,
		  /*default_adm=*/nullptr,
		  webrtc::CreateBuiltinAudioEncoderFactory(),
		  webrtc::CreateBuiltinAudioDecoderFactory(),
		  webrtc::CreateBuiltinVideoEncoderFactory(),
		  webrtc::CreateBuiltinVideoDecoderFactory(),
		  /*audio_mixer=*/nullptr,
		  /*audio_processing=*/nullptr);
	}

	// Set SDP semantics to Unified Plan.
	config.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;

	// Create the webrtc::Peerconnection.
	this->pc = peerConnectionFactory->CreatePeerConnection(config, nullptr, nullptr, listener);
}

std::string PeerConnection::CreateOffer(
  const webrtc::PeerConnectionInterface::RTCOfferAnswerOptions& options)
{
	MSC_TRACE();

	CreateSessionDescriptionObserver* sessionDescriptionObserver =
	  new rtc::RefCountedObject<CreateSessionDescriptionObserver>();

	auto future = sessionDescriptionObserver->GetFuture();

	this->pc->CreateOffer(sessionDescriptionObserver, options);

	return future.get();
}

std::string PeerConnection::CreateAnswer(
  const webrtc::PeerConnectionInterface::RTCOfferAnswerOptions& options)
{
	MSC_TRACE();

	CreateSessionDescriptionObserver* sessionDescriptionObserver =
	  new rtc::RefCountedObject<CreateSessionDescriptionObserver>();

	auto future = sessionDescriptionObserver->GetFuture();

	this->pc->CreateAnswer(sessionDescriptionObserver, options);

	return future.get();
}

void PeerConnection::SetLocalDescription(PeerConnection::SdpType type, const std::string& sdp)
{
	MSC_TRACE();

	webrtc::SdpParseError error;
	webrtc::SessionDescriptionInterface* sessionDescription;
	rtc::scoped_refptr<SetSessionDescriptionObserver> observer(
	  new rtc::RefCountedObject<SetSessionDescriptionObserver>());

	auto typeStr = sdpType2String[type];
	auto future  = observer->GetFuture();

	sessionDescription = webrtc::CreateSessionDescription(typeStr, sdp, &error);
	if (sessionDescription == nullptr)
	{
		MSC_WARN(
		  "webrtc::CreateSessionDescription failed [%s:%s]",
		  error.line.c_str(),
		  error.description.c_str());

		observer->Reject(error.description);

		return future.get();
	}

	this->pc->SetLocalDescription(observer, sessionDescription);

	return future.get();
}

void PeerConnection::SetRemoteDescription(PeerConnection::SdpType type, const std::string& sdp)
{
	MSC_TRACE();

	webrtc::SdpParseError error;
	webrtc::SessionDescriptionInterface* sessionDescription;
	rtc::scoped_refptr<SetSessionDescriptionObserver> observer(
	  new rtc::RefCountedObject<SetSessionDescriptionObserver>());

	auto typeStr = sdpType2String[type];
	auto future  = observer->GetFuture();

	sessionDescription = webrtc::CreateSessionDescription(typeStr, sdp, &error);
	if (sessionDescription == nullptr)
	{
		MSC_WARN(
		  "webrtc::CreateSessionDescription failed [%s:%s]",
		  error.line.c_str(),
		  error.description.c_str());

		observer->Reject(error.description);

		return future.get();
	}

	this->pc->SetRemoteDescription(observer, sessionDescription);

	return future.get();
}

const std::string PeerConnection::GetLocalDescription()
{
	MSC_TRACE();

	auto desc = this->pc->local_description();
	std::string sdp;

	desc->ToString(&sdp);

	return sdp;
}

const std::string PeerConnection::GetRemoteDescription()
{
	MSC_TRACE();

	auto desc = this->pc->remote_description();
	std::string sdp;

	desc->ToString(&sdp);

	return sdp;
}

rtc::scoped_refptr<webrtc::RtpTransceiverInterface> PeerConnection::AddTransceiver(
  rtc::scoped_refptr<webrtc::MediaStreamTrackInterface> track)
{
	MSC_TRACE();

	/*
	 * Define a stream id so the generated local description is correct.
	 * - with a stream id:    "a=ssrc:<ssrc-id> mslabel:<value>"
	 * - without a stream id: "a=ssrc:<ssrc-id> mslabel:"
	 *
	 * The second is incorrect (https://tools.ietf.org/html/rfc5576#section-4.1)
	 */
	webrtc::RtpTransceiverInit rtpTransceiverInit;
	rtpTransceiverInit.stream_ids.emplace_back("0");

	auto result = this->pc->AddTransceiver(std::move(track), rtpTransceiverInit);

	if (!result.ok())
	{
		rtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver = nullptr;

		return transceiver;
	}

	return result.value();
}

/* SetSessionDescriptionObserver */

void PeerConnection::SetSessionDescriptionObserver::OnFailure(webrtc::RTCError error)
{
	MSC_WARN(
	  "webtc::SetSessionDescriptionObserver failure [%s:%s]",
	  webrtc::ToString(error.type()).c_str(),
	  error.message());

	auto message = std::string(error.message());
	this->Reject(message);
};

/* CreateSessionDescriptionObserver */

void PeerConnection::CreateSessionDescriptionObserver::OnFailure(webrtc::RTCError error)
{
	MSC_WARN(
	  "webtc::CreateSessionDescriptionObserver failure [%s:%s]",
	  webrtc::ToString(error.type()).c_str(),
	  error.message());

	auto message = std::string(error.message());
	this->Reject(message);
};

/* PeerConnection */

bool PeerConnection::SetConfiguration(const webrtc::PeerConnectionInterface::RTCConfiguration& config)
{
	MSC_TRACE();

	webrtc::RTCError error;

	if (this->pc->SetConfiguration(config, &error))
	{
		return true;
	}

	MSC_WARN(
	  "webrtc::PeerConnection::SetConfiguration failed [%s:%s]",
	  webrtc::ToString(error.type()).c_str(),
	  error.message());

	return false;
}

json PeerConnection::GetNativeRtpCapabilities() const
{
	/* clang-format off */
	json capabilities =
	{
		{ "codecs",           json::array() },
		{ "headerExtensions", json::array() },
		{ "fecMechanisms",    json::array() } // TODO.
	};
	/* clang-format on */

	// Get audio capabilities.
	webrtcRtpCapabilities2Json(
	  cricket::MEDIA_TYPE_AUDIO,
	  this->peerConnectionFactory->GetRtpSenderCapabilities(cricket::MEDIA_TYPE_AUDIO),
	  capabilities);

	// Get video capabilities.
	webrtcRtpCapabilities2Json(
	  cricket::MEDIA_TYPE_VIDEO,
	  this->peerConnectionFactory->GetRtpSenderCapabilities(cricket::MEDIA_TYPE_VIDEO),
	  capabilities);

	return capabilities;
}

json PeerConnection::GetStats()
{
	MSC_TRACE();

	rtc::scoped_refptr<RTCStatsCollectorCallback> callback(
	  new rtc::RefCountedObject<RTCStatsCollectorCallback>());

	auto future = callback->GetFuture();

	this->pc->GetStats(callback);

	return future.get();
}

json PeerConnection::GetStats(rtc::scoped_refptr<webrtc::RtpSenderInterface> selector)
{
	MSC_TRACE();

	rtc::scoped_refptr<RTCStatsCollectorCallback> callback(
	  new rtc::RefCountedObject<RTCStatsCollectorCallback>());

	auto future = callback->GetFuture();

	this->pc->GetStats(std::move(selector), callback);

	return future.get();
}

json PeerConnection::GetStats(rtc::scoped_refptr<webrtc::RtpReceiverInterface> selector)
{
	MSC_TRACE();

	rtc::scoped_refptr<RTCStatsCollectorCallback> callback(
	  new rtc::RefCountedObject<RTCStatsCollectorCallback>());

	auto future = callback->GetFuture();

	this->pc->GetStats(std::move(selector), callback);

	return future.get();
}

/* PeerConnection::Listener. */

// Triggered when the SignalingState changed.
void PeerConnection::Listener::OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState newState)
{
	MSC_TRACE();

	MSC_DEBUG("new SignalingState:[%s]", PeerConnection::signalingState2String[newState].c_str());
}

// Triggered when media is received on a new stream from remote peer.
void PeerConnection::Listener::OnAddStream(rtc::scoped_refptr<webrtc::MediaStreamInterface> /*stream*/)
{
	MSC_TRACE();
}

// Triggered when a remote peer closes a stream.
void PeerConnection::Listener::OnRemoveStream(rtc::scoped_refptr<webrtc::MediaStreamInterface> /*stream*/)
{
	MSC_TRACE();
}

// Triggered when a remote peer opens a data channel.
void PeerConnection::Listener::OnDataChannel(
  rtc::scoped_refptr<webrtc::DataChannelInterface> /*dataChannel*/)
{
	MSC_TRACE();
};

// Triggered when renegotiation is needed. For example, an ICE restart has begun.
void PeerConnection::Listener::OnRenegotiationNeeded()
{
	MSC_TRACE();
}

// Triggered any time the IceConnectionState changes.
//
// Note that our ICE states lag behind the standard slightly. The most
// notable differences include the fact that "failed" occurs after 15
// seconds, not 30, and this actually represents a combination ICE + DTLS
// state, so it may be "failed" if DTLS fails while ICE succeeds.
void PeerConnection::Listener::OnIceConnectionChange(
  webrtc::PeerConnectionInterface::IceConnectionState newState)
{
	MSC_TRACE();

	MSC_DEBUG(
	  "new IceConnectionState:[%s]", PeerConnection::iceConnectionState2String[newState].c_str());
}

// Triggered any time the IceGatheringState changes.
void PeerConnection::Listener::OnIceGatheringChange(
  webrtc::PeerConnectionInterface::IceGatheringState newState)
{
	MSC_TRACE();

	MSC_DEBUG("new IceGatheringState:[%s]", PeerConnection::iceGatheringState2String[newState].c_str());
}

// Triggered when a new ICE candidate has been gathered.
void PeerConnection::Listener::OnIceCandidate(const webrtc::IceCandidateInterface* candidate)
{
	MSC_TRACE();

#ifndef MSC_LOG_DEV
	(void)candidate;
#else
	std::string candidateStr;

	candidate->ToString(&candidateStr);

	MSC_DEBUG("new IceCandidate:[%s]", candidateStr.c_str());
#endif
}

// Triggered when the ICE candidates have been removed.
void PeerConnection::Listener::OnIceCandidatesRemoved(const std::vector<cricket::Candidate>& /*candidates*/)
{
	MSC_TRACE();
}

// Triggered when the ICE connection receiving status changes.
void PeerConnection::Listener::OnIceConnectionReceivingChange(bool /*receiving*/)
{
	MSC_TRACE();
}

// Triggered when a receiver and its track are created.
// Note: This is called with both Plan B and Unified Plan semantics. Unified
// Plan users should prefer OnTrack, OnAddTrack is only called as backwards
// compatibility (and is called in the exact same situations as OnTrack).
void PeerConnection::Listener::OnAddTrack(
  rtc::scoped_refptr<webrtc::RtpReceiverInterface> /*receiver*/,
  const std::vector<rtc::scoped_refptr<webrtc::MediaStreamInterface>>& /*streams*/)
{
	MSC_TRACE();
}

// Triggered when signaling indicates a transceiver will be receiving
// media from the remote endpoint. This is fired during a call to
// SetRemoteDescription. The receiving track can be accessed by:
// |transceiver->receiver()->track()| and its associated streams by
// |transceiver->receiver()->streams()|.
// Note: This will only be called if Unified Plan semantics are specified.
// This behavior is specified in section 2.2.8.2.5 of the "Set the
// RTCSessionDescription" algorithm:
// https://w3c.github.io/webrtc-pc/#set-description
void PeerConnection::Listener::OnTrack(rtc::scoped_refptr<webrtc::RtpTransceiverInterface> /*transceiver*/)
{
	MSC_TRACE();
}

// Triggered when signaling indicates that media will no longer be received on a
// track.
// With Plan B semantics, the given receiver will have been removed from the
// PeerConnection and the track muted.
// With Unified Plan semantics, the receiver will remain but the transceiver
// will have changed direction to either sendonly or inactive.
// https://w3c.github.io/webrtc-pc/#process-remote-track-removal
void PeerConnection::Listener::OnRemoveTrack(rtc::scoped_refptr<webrtc::RtpReceiverInterface> /*receiver*/)
{
	MSC_TRACE();
}

// Triggered when an interesting usage is detected by WebRTC.
// An appropriate action is to add information about the context of the
// PeerConnection and write the event to some kind of "interesting events"
// log function.
// The heuristics for defining what constitutes "interesting" are
// implementation-defined.
void PeerConnection::Listener::OnInterestingUsage(int /*usagePattern*/)
{
	MSC_TRACE();
}
} // namespace mediasoupclient