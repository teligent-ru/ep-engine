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

#define MAX_PACKET_SIZE 65000


ExpiryChannel::ExpiryChannel(): mSocket(-1) {
}

ExpiryChannel::~ExpiryChannel() {
	close();
}

bool ExpiryChannel::open(const std::string& dstAddr, const int dstPort) {
	LOG(EXTENSION_LOG_INFO, "%s: open(%s:%d)", __PRETTY_FUNCTION__, dstAddr.c_str(), dstPort);
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
			LOG(EXTENSION_LOG_ERROR, "%s: conn(%d,%d)=%d: %d=%s", __PRETTY_FUNCTION__, addr.sin_addr.s_addr, addr.sin_port, rc, errno, strerror(errno));
			close();
			return false;
		}
	}
	
	int mtu = IP_PMTUDISC_DONT;
	socklen_t socklen = sizeof(mtu);
	setsockopt(mSocket, IPPROTO_IP, IP_MTU_DISCOVER, &mtu, socklen);
	
	return true;
}

void ExpiryChannel::sendNotification(const StoredValue* v) {
	if(!v || !isConnected())
		return;
	// @TODO
	// get total size of packed data
	tptf->invalidateDataSize();
	const size_t buf_size = tptf->getSize(true);
	char *buf = new char[buf_size];
	
	ssize_t sz = tptf->pack(buf, buf_size);
	if (sz < 0) {
		LOG(EXTENSION_LOG_ERROR, "%s: Failed to pack transaction into buffer! buf_size = %d", __PRETTY_FUNCTION__, buf_size);
		delete[] buf;
		return;
	}
	if (sz > MAX_PACKET_SIZE)
		sz = MAX_PACKET_SIZE;

	LOG(EXTENSION_LOG_DEBUG, "%s: write_size=%d, buf_size=%d", __PRETTY_FUNCTION__, sz, buf_size);
	
	ssize_t writed = send(mSocket, buf, sz, 0);
	
	LOG(EXTENSION_LOG_DEBUG, "%s: size=%d writed=%d %s", __PRETTY_FUNCTION__, sz, writed, strerror(errno));
	
	if(writed < 0) {
		LOG(EXTENSION_LOG_ERROR, "%s: size=%d writed=%d %s", __PRETTY_FUNCTION__, sz, writed, strerror(errno));
	}
	
	delete[] buf;
}

void ExpiryChannel::close() {
	if(mSocket >= 0) {
		close();
		mSocket = -1;
	}
}

const bool ExpiryChannel::isConnected() const
{
	return (mSocket >= 0);
}
