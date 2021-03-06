// ================================================================================================
// 
// Copyright (C) 2011-2016, Christoph Hennersperger - all rights reserved
// Copyright (C) 2011-2016, Rüdiger Göbl - all rights reserved
// Copyright (c) 2019, NVIDIA CORPORATION. All rights reserved.
//
//          Christoph Hennersperger 
//          EmaiL christoph.hennersperger@tum.de
//          Chair for Computer Aided Medical Procedures
//          Technische Universität München
//          Boltzmannstr. 3, 85748 Garching b. München, Germany
//    and
//          Rüdiger Göbl
//          Email r.goebl@tum.de
// 
// This library is free software; you can redistribute it and/or
// modify it under the terms of the GNU Lesser General Public
// License, version 2.1, as published by the Free Software Foundation.
//
// This library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public
// License along with this program.  If not, see
// <http://www.gnu.org/licenses/>.
//
// ================================================================================================

#include <iostream>
#include <thread>
#include <tbb/flow_graph.h>
#include <cmath>

#include "OpenIGTLinkOutputDevice.h"

#include "igtlImageMessage.h"
#include <utilities/utility.h>
#include <utilities/Logging.h>

using namespace std;
namespace supra
{
	using namespace logging;

	OpenIGTLinkOutputDevice::OpenIGTLinkOutputDevice(tbb::flow::graph& graph, const std::string & nodeID, bool queueing)
		: AbstractOutput(graph, nodeID, queueing)
		, m_server(igtl::ServerSocket::New())
		, m_port(18944)
		, m_pConnectionThread(nullptr)
	{
		m_callFrequency.setName("IGTL");

		m_valueRangeDictionary.set<uint32_t>("port", 1, 65535, 18944, "Port");
		m_valueRangeDictionary.set<std::string>("streamName", "IGTL", "Stream name");
		m_isReady = false;
		m_isConnected = false;
	}

	OpenIGTLinkOutputDevice::~OpenIGTLinkOutputDevice()
	{
		m_isConnected = false;
		m_isReady = false;
		if (m_clientConnection)
		{
			m_clientConnection->CloseSocket();
		}
		m_server->CloseSocket();

		if (m_pConnectionThread && m_pConnectionThread->joinable())
		{
			m_pConnectionThread->join();
		}
	}

	void OpenIGTLinkOutputDevice::initializeOutput()
	{
		log_info("IGTL: server port: ", m_server->GetServerPort());
		if (m_server->CreateServer(m_port) != 0) {
			m_isReady = false;
		}
		else {
			m_isReady = true;
			//Wait asynchronously for the connection
			waitAsyncForConnection();
		}
	}

	bool OpenIGTLinkOutputDevice::ready()
	{
		return m_isReady;
	}

	void OpenIGTLinkOutputDevice::startOutput()
	{
	}

	void OpenIGTLinkOutputDevice::stopOutput()
	{
	}

	void OpenIGTLinkOutputDevice::configurationDone()
	{
		m_port = m_configurationDictionary.get<uint32_t>("port");
		m_streamName = m_configurationDictionary.get<std::string>("streamName");
	}

	void OpenIGTLinkOutputDevice::writeData(std::shared_ptr<RecordObject> data)
	{
		if (m_isReady && getRunning() && m_isConnected)
		{
			m_callFrequency.measure();
			sendMessage(data);
			m_callFrequency.measureEnd();
		}
	}

	void OpenIGTLinkOutputDevice::sendMessage(shared_ptr<const RecordObject> data)
	{
		switch (data->getType())
		{
		case TypeSyncRecordObject:
			sendSyncRecordMessage(data);
			break;
		case TypeTrackerDataSet:
			sendTrackingMessage(data);
			break;
		case TypeUSImage:
			sendImageMessage(data);
			break;
		case TypeRecordUnknown:
		default:
			break;
		}
	}

	void OpenIGTLinkOutputDevice::sendSyncRecordMessage(shared_ptr<const RecordObject> _syncMessage)
	{
		auto syncMessage = dynamic_pointer_cast<const SyncRecordObject>(_syncMessage);
		if (syncMessage)
		{
			for (shared_ptr<const RecordObject> syncedO : syncMessage->getSyncedRecords())
			{
				sendMessage(syncedO);
			}
			sendMessage(syncMessage->getMainRecord());
		}
	}

	template <typename T>
	void OpenIGTLinkOutputDevice::sendImageMessageTemplated(shared_ptr<const USImage> imageData)
	{
		static_assert(
			std::is_same<T, uint8_t>::value ||
			std::is_same<T, int16_t>::value ||
			std::is_same<T, float>::value,
			"Image only implemented for uchar, short and float at the moment");

		auto properties = imageData->getImageProperties();
		if (
			properties->getImageType() == USImageProperties::BMode ||
			properties->getImageType() == USImageProperties::Doppler)
		{
			double resolution = properties->getImageResolution();
			vec3s imageSize = imageData->getSize();

			igtl::ImageMessage::Pointer pImageMsg = igtl::ImageMessage::New();
			pImageMsg->SetDimensions((int)imageSize.x, (int)imageSize.y, (int)imageSize.z);
			pImageMsg->SetSpacing(resolution, resolution, resolution);
			if (is_same<T, uint8_t>::value)
			{
				pImageMsg->SetScalarTypeToUint8();
			}
			if (is_same<T, int16_t>::value)
			{
				pImageMsg->SetScalarTypeToInt16();
			}
			if (is_same<T, float>::value)
			{
				pImageMsg->SetScalarType(igtl::ImageMessage::TYPE_FLOAT32);
			}

			pImageMsg->SetEndian(igtl::ImageMessage::ENDIAN_LITTLE);
			igtl::Matrix4x4 m;
			igtl::IdentityMatrix(m);
			m[0][0] = -1;
			m[1][1] = -1;
			
			pImageMsg->SetMatrix(m);
			pImageMsg->SetNumComponents(1);
			pImageMsg->SetDeviceName(m_streamName.c_str());
			pImageMsg->AllocateScalars();
			igtl::TimeStamp::Pointer pTimestamp = igtl::TimeStamp::New();
			double timestampSeconds;
			double timestampFrac = modf(imageData->getSyncTimestamp(), &timestampSeconds);
			pTimestamp->SetTime((uint32_t)timestampSeconds, (uint32_t)(timestampFrac*1e9));
			pImageMsg->SetTimeStamp(pTimestamp);

			auto imageContainer = imageData->getData<T>();
			if (!imageContainer->isHost())
			{
				imageContainer = make_shared<Container<T> >(LocationHost, *imageContainer);
			}

			size_t numElements = imageSize.x * imageSize.y * imageSize.z;
			memcpy(pImageMsg->GetScalarPointer(), imageContainer->get(), numElements * sizeof(T));

			pImageMsg->Pack();

			int sendResult = m_clientConnection->Send(pImageMsg->GetPackPointer(), pImageMsg->GetPackSize());
			if (sendResult == 0) //when it could not be sent
			{
				m_isConnected = false;
				log_info("IGTL: Lost connection. Waiting for next connection.");
				waitAsyncForConnection();
			}
		}
	}

	void OpenIGTLinkOutputDevice::sendImageMessage(shared_ptr<const RecordObject> _imageData)
	{
		auto imageData = dynamic_pointer_cast<const USImage>(_imageData);
		switch (imageData->getDataType())
		{
		case TypeFloat:
			sendImageMessageTemplated<float>(imageData);
			break;
		case TypeInt16:
			sendImageMessageTemplated<int16_t>(imageData);
			break;
		case TypeUint8:
			sendImageMessageTemplated<uint8_t>(imageData);
			break;
		default:
			logging::log_error("OpenIGTLinkOutputDevice: Input image data type not supported");
			break;
		}
	}

	
	void OpenIGTLinkOutputDevice::sendTrackingMessage(shared_ptr<const RecordObject> _trackData)
	{
		auto trackData = dynamic_pointer_cast<const TrackerDataSet>(_trackData);
		if (trackData)
		{
			auto msg = igtl::TrackingDataMessage::New();
			msg->AllocatePack();

			for (size_t i = 0; i < trackData->getSensorData().size(); ++i)
				addTrackingData(msg, trackData->getSensorData()[i], static_cast<int>(i));
			msg->SetDeviceName(m_streamName.c_str());
			igtl::TimeStamp::Pointer pTimestamp = igtl::TimeStamp::New();
			double timestampSeconds;
			double timestampFrac = modf(trackData->getSyncTimestamp(), &timestampSeconds);
			pTimestamp->SetTime((uint32_t)timestampSeconds, (uint32_t)(timestampFrac*1e9));
			msg->SetTimeStamp(pTimestamp);
			msg->Pack();
			m_clientConnection->Send(msg->GetPackPointer(), msg->GetPackSize());
		}
	}
	
	void OpenIGTLinkOutputDevice::addTrackingData(igtl::TrackingDataMessage::Pointer msg,
		const TrackerData& trackerData,
		int targetSensor)
	{
		auto matrix = trackerData.getMatrix();

		// calculate rotation matrix and store it
		igtl::Matrix4x4 igtlMatrix;

		igtlMatrix[0][0] = matrix[0 + 0];
		igtlMatrix[0][1] = matrix[0 + 1];
		igtlMatrix[0][2] = matrix[0 + 2];
		igtlMatrix[0][3] = matrix[0 + 3];

		igtlMatrix[1][0] = matrix[4 + 0];
		igtlMatrix[1][1] = matrix[4 + 1];
		igtlMatrix[1][2] = matrix[4 + 2];
		igtlMatrix[1][3] = matrix[4 + 3];

		igtlMatrix[2][0] = matrix[8 + 0];
		igtlMatrix[2][1] = matrix[8 + 1];
		igtlMatrix[2][2] = matrix[8 + 2];
		igtlMatrix[2][3] = matrix[8 + 3];

		igtlMatrix[3][0] = matrix[12 + 0];
		igtlMatrix[3][1] = matrix[12 + 1];
		igtlMatrix[3][2] = matrix[12 + 2];
		igtlMatrix[3][3] = matrix[12 + 3];

		auto trackElem = igtl::TrackingDataElement::New();
		trackElem->SetMatrix(igtlMatrix);
		std::stringstream ss;
		ss << trackerData.getInstrumentName();
		ss << targetSensor;
		trackElem->SetName(ss.str().c_str());
		msg->AddTrackingDataElement(trackElem);
	}
	void OpenIGTLinkOutputDevice::waitAsyncForConnection()
	{
		if (m_pConnectionThread && m_pConnectionThread->joinable())
		{
			m_pConnectionThread->join();
		}

		m_pConnectionThread = unique_ptr<thread>(
			new thread([this]() {
			log_info("IGTL: waiting for connection");
			m_clientConnection = m_server->WaitForConnection();
			m_isConnected = true;
			log_info("IGTL: got connection!");
		}));
	}
}
