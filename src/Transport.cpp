#define MSC_CLASS "Transport"
// #define MSC_LOG_DEV

#include "Transport.hpp"
#include "Logger.hpp"
#include "ortc.hpp"
#include <string>
#include <utility>

namespace mediasoupclient
{
/* SendTransport static variables. */

/* clang-format off */
const json SendTransport::DefaultSimulcast =
{
	{
		{ "maxBitrate", 100000 }
	},
	{
		{ "maxBitrate", 500000 }
	},
	{
		{ "maxBitrate", 1500000 }
	}
};
/* clang-format on */

/* SendTransport instance methods. */

SendTransport::SendTransport(
  Listener* listener,
  const json& transportRemoteParameters,
  PeerConnection::Options* peerConnectionOptions,
  const json& extendedRtpCapabilities,
  std::map<std::string, bool> canProduceByKind,
  json appData)
  : listener(listener),
    Transport(listener, transportRemoteParameters, extendedRtpCapabilities, std::move(appData)),
    canProduceByKind(std::move(canProduceByKind))
{
	MSC_TRACE();

	json rtpParametersByKind = {
		{ "audio", ortc::getSendingRtpParameters("audio", extendedRtpCapabilities) },
		{ "video", ortc::getSendingRtpParameters("video", extendedRtpCapabilities) }
	};

	this->handler.reset(
	  new SendHandler(this, transportRemoteParameters, peerConnectionOptions, rtpParametersByKind));

	Transport::SetHandler(this->handler.get());
}

/*
 * Produce a track
 */
Producer* SendTransport::Produce(
  Producer::PublicListener* producerPublicListener,
  webrtc::MediaStreamTrackInterface* track,
  json simulcast,
  uint8_t maxSpatialLayer,
  json appData)
{
	MSC_TRACE();

	if (this->closed)
		throw Exception("Invalid state");
	if (track == nullptr)
		throw Exception("Track cannot be null");
	if (track->state() == webrtc::MediaStreamTrackInterface::TrackState::kEnded)
		throw Exception("Track ended");
	if (!simulcast.is_array())
		throw Exception("Invalid simulcast");
	if (track->kind() != "video" && simulcast.size() > 0)
		throw Exception("Cannot set simulcast on audio track");
	if (track->kind() != "video" && maxSpatialLayer > 0)
		throw Exception("Cannot set simulcast on audio track");

	json rtpParameters;
	json producerRemoteParameters;

	auto normalizedSimulcast = json::array();

	if (track->kind() == "video" && simulcast.size() > 1)
	{
		std::for_each(simulcast.begin(), simulcast.end(), [&normalizedSimulcast](const json& entry) {
			auto it = entry.find("maxBitrate");
			if (it == entry.end())
				throw Exception("Invalid simulcast entry");

			if (!(*it).is_number())
				throw Exception("Invalid simulcast entry");

			normalizedSimulcast.push_back({ "maxBitrate", *it });
		});
	}

	// May throw.
	rtpParameters = this->handler->Send(track, simulcast);

	try
	{
		/* clang-format off */
		json producerLocalParameters =
		{
			{ "kind",          track->kind() },
			{ "rtpParameters", rtpParameters },
			{ "appData",       appData       }
		};
		/* clang-format on */

		// May throw.
		producerRemoteParameters = this->listener->OnProduce(producerLocalParameters).get();
	}
	catch (Exception& error)
	{
		this->handler->StopSending(track);

		throw;
	}

	auto* producer = new Producer(
	  this,
	  producerPublicListener,
	  producerRemoteParameters["id"].get<std::string>(),
	  track,
	  rtpParameters,
	  maxSpatialLayer,
	  appData);

	this->producers[producer->GetId()] = producer;

	return producer;
}

/* Handler::Listener methods */
void Transport::OnConnect(json& transportLocalParameters)
{
	MSC_TRACE();

	if (this->closed)
		throw Exception("Invalid state");

	transportLocalParameters["id"] = this->id;

	return this->listener->OnConnect(transportLocalParameters).get();
}

/* Producer::Listener methods */
void SendTransport::OnClose(Producer* producer)
{
	MSC_TRACE();

	this->producers.erase(producer->GetId());

	if (this->closed)
		return;

	// May throw.
	this->handler->StopSending(producer->GetTrack());
}

void SendTransport::OnReplaceTrack(const Producer* producer, webrtc::MediaStreamTrackInterface* newTrack)
{
	MSC_TRACE();

	return this->handler->ReplaceTrack(producer->GetTrack(), newTrack);
}

void SendTransport::OnSetMaxSpatialLayer(const Producer* producer, uint8_t maxSpatialLayer)
{
	MSC_TRACE();

	return this->handler->SetMaxSpatialLayer(producer->GetTrack(), maxSpatialLayer);
}

json SendTransport::OnGetStats(const Producer* producer)
{
	MSC_TRACE();

	if (this->closed)
		throw Exception("Invalid state");

	return this->handler->GetSenderStats(producer->GetTrack());
}

/* RecvTransport instance methods. */

RecvTransport::RecvTransport(
  Transport::Listener* listener,
  const json& transportRemoteParameters,
  PeerConnection::Options* peerConnectionOptions,
  const json& extendedRtpCapabilities,
  json appData)
  : Transport(listener, transportRemoteParameters, extendedRtpCapabilities, std::move(appData))
{
	MSC_TRACE();

	this->handler.reset(new RecvHandler(this, transportRemoteParameters, peerConnectionOptions));

	Transport::SetHandler(this->handler.get());
}

/*
 * Consume a remote Producer.
 */
Consumer* RecvTransport::Consume(
  Consumer::PublicListener* consumerPublicListener, const json& consumerRemoteParameters, json appData)
{
	MSC_TRACE();

	// Check if the track is a null pointer.
	if (this->closed)
		throw Exception("Invalid state");
	if (!consumerRemoteParameters.is_object())
		throw Exception("Missing consumerRemoteParameters");
	if (consumerRemoteParameters.find("id") == consumerRemoteParameters.end())
		throw Exception("Missing consumerRemoteParameters[\"id\"]");
	if (consumerRemoteParameters.find("producerId") == consumerRemoteParameters.end())
		throw Exception("Missing consumerRemoteParameters[\"producerId\"]");

	json consumerParameters = consumerRemoteParameters;

	// Ensure the device can consume it.
	auto canConsume =
	  ortc::canReceive(consumerParameters["rtpParameters"], this->extendedRtpCapabilities);

	if (!canConsume)
		throw Exception("cannot consume this Producer");

	webrtc::MediaStreamTrackInterface* track;

	// May throw.
	track = this->handler->Receive(
	  consumerParameters["id"].get<std::string>(),
	  consumerParameters["kind"].get<std::string>(),
	  consumerParameters["rtpParameters"]);

	auto* consumer = new Consumer(
	  this,
	  consumerPublicListener,
	  consumerParameters["id"].get<std::string>(),
	  consumerParameters["producerId"].get<std::string>(),
	  track,
	  consumerParameters["rtpParameters"],
	  std::move(appData));

	this->consumers[consumer->GetId()] = consumer;

	return consumer;
}

/* Consumer::Listener methods */
void RecvTransport::OnClose(Consumer* consumer)
{
	MSC_TRACE();

	this->consumers.erase(consumer->GetId());

	if (this->closed)
		return;

	// May throw.
	this->handler->StopReceiving(consumer->GetId());
}

json RecvTransport::OnGetStats(const Consumer* consumer)
{
	MSC_TRACE();

	if (this->closed)
		throw Exception("Invalid state");

	return this->handler->GetReceiverStats(consumer->GetId());
}
} // namespace mediasoupclient