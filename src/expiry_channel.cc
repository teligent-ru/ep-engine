/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2016 Teligent; Author: Alexander Petrossian (PAF)
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */

#include "expiry_channel.h"

#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/ip.h>
#include <arpa/inet.h>
#include <errno.h>
#include <sstream>

#include <cJSON.h>

static const size_t MAX_PACKET_SIZE=65000;


ExpiryChannel::ExpiryChannel(): mSocket(-1) {
}

ExpiryChannel::~ExpiryChannel() {
	close();
}

bool ExpiryChannel::open(const std::string& dstAddr, const int dstPort) {
	LOG(EXTENSION_LOG_INFO, "%s: open(%s:%d)", __func__, dstAddr.c_str(), dstPort);
	if(mSocket >= 0)
		close();
	mSocket = socket(PF_INET, SOCK_DGRAM, 0);
	if(mSocket < 0 || dstAddr.empty() || !dstPort) {
		return false;
	}
	
	{
		sockaddr_in addr;
		int rc;
		
		memset(&addr, 0, sizeof(addr));
		addr.sin_family = AF_INET;
		inet_pton(AF_INET, dstAddr.c_str(), &addr.sin_addr);
		addr.sin_port = htons(dstPort);
		
		if((rc=connect(mSocket, (sockaddr*)&addr, sizeof(addr))) < 0) {
			LOG(EXTENSION_LOG_WARNING, "%s: conn(%d,%d)=%d: %d=%s", __func__, addr.sin_addr.s_addr, addr.sin_port, rc, errno, strerror(errno));
			close();
			return false;
		}
	}
	
	int mtu = IP_PMTUDISC_DONT;
	socklen_t socklen = sizeof(mtu);
	setsockopt(mSocket, IPPROTO_IP, IP_MTU_DISCOVER, &mtu, socklen);
	
	return true;
}

void ExpiryChannel::sendNotification(const std::string& name, const StoredValue* v) {
	if(!v) {
		LOG(EXTENSION_LOG_WARNING, "%s[%s]: called without StoredValue, bailing out...", __func__, name.c_str());
        return;
    }
    if(!isConnected()) {
		LOG(EXTENSION_LOG_WARNING, "%s[%s.%s], but there is no connection (not configured? failed to open?), bailing out...", __func__, name.c_str(), v->getKey().c_str());
		return;
    }
/*
{
  "bucket": "<STYPE>",
  "id": "<SID>",
  "expiry": "<EXPIRES>",
  "cas": "<CAS value (version)>",
  "flags": <flags>,
  "body": <PAYLOAD in BOX format>  // datatype==0? "string", datatype==1(JSON) {...}
}
*/
	cJSON* root = cJSON_CreateObject();
	cJSON_AddStringToObject(root, "bucket", name.c_str());
	cJSON_AddStringToObject(root, "id", v->getKey().c_str());
	cJSON_AddNumberToObject(root, "expiry", v->getExptime());
	cJSON_AddNumberToObject(root, "cas", v->getCas());
	cJSON_AddNumberToObject(root, "flags", v->getFlags());

	const value_t& d = v->getValue();
	uint8_t t = d->getDataType();
	switch(t) {
		case PROTOCOL_BINARY_DATATYPE_JSON: {
			size_t vlength = d->vlength();
			char* bodyz = new char[vlength+1/*terminator for limited cJSON*/];
			memcpy(bodyz, d->getData(), vlength);
			bodyz[vlength] = 0; // terminator

			cJSON* jbody = cJSON_Parse(bodyz);
			if (!jbody) {
			LOG(EXTENSION_LOG_WARNING, "%s[%.%s]: reported its type as JSON but can not parse it, bailing out...", __func__, name.c_str(), v->getKey().c_str());
			delete[] bodyz;
			cJSON_Delete(root);
			return;
			}
			cJSON_AddItemToObject(root, "body", jbody); // assumes responsibility
			delete[] bodyz;
			break;
		}
		case PROTOCOL_BINARY_RAW_BYTES: {
			size_t vlength = d->vlength();
			char* bodyz = new char[vlength+1/*terminator for limited cJSON*/];
			memcpy(bodyz, d->getData(), vlength);
			bodyz[vlength] = 0; // terminator
			cJSON_AddStringToObject(root, "body", bodyz);
			delete[] bodyz;
			break;
		}
		default:
			LOG(EXTENSION_LOG_WARNING, "%s[%.%s]: can not handle its type[%d] (it's neither RAW=0 nor JSON=1), bailing out...", __func__, name.c_str(), v->getKey().c_str(), t);
			cJSON_Delete(root);
			return;
	}

	char* json_cstr = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);

	if (!json_cstr) {
		LOG(EXTENSION_LOG_WARNING, "%s[%s.%s]: failed to serialize to json. Had good type[%d] (RAW=0, JSON=1), bailing out...", __func__, name.c_str(), v->getKey().c_str(), t);
		return;
	}
	size_t json_length = strlen(json_cstr);
	if (json_length > MAX_PACKET_SIZE) {
		LOG(EXTENSION_LOG_WARNING, "%s[%s.%s]: serialized to json_length[%zu], which is more than MAX_PACKET_SIZE[%zu], bailing out...", __func__, name.c_str(), v->getKey().c_str(), json_length, MAX_PACKET_SIZE);
		cJSON_Free(json_cstr);
		return;
	}

	ssize_t written = send(mSocket, json_cstr, json_length, 0);
	if(json_length != static_cast<size_t>(written)) {
		LOG(EXTENSION_LOG_WARNING, "%s[%s.%s]: json_length[%zu] != written[%zu] errno[%s (%d)]", __func__, name.c_str(), v->getKey().c_str(), json_length, written, strerror(errno), errno);
	}

	cJSON_Free(json_cstr);
}

void ExpiryChannel::close() {
	if(isConnected()) {
		::close(mSocket);
		mSocket = -1;
	}
}

const bool ExpiryChannel::isConnected() const {
	return mSocket >= 0;
}
