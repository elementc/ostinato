#include "myservice.h"
#include "qdebug.h"

#include <qglobal.h>
#include <qendian.h>

#ifdef Q_OS_WIN32
#include <pcap-int.h>
#include <Ntddndis.h>
#endif

#define LOG(...)	{sprintf(logStr, __VA_ARGS__); host->Log(logStr);}
#define MB			(1024*1024)

quint32 StreamInfo::pseudoHdrCksumPartial(quint32 srcIp, quint32 dstIp, 
		quint8 protocol, quint16 len)
{
	quint32 sum;

	sum = srcIp >> 16;
	sum += srcIp & 0xFFFF;
	sum += dstIp >> 16;
	sum += dstIp & 0xFFFF;
	sum += (quint16) (protocol);
	sum += len;

	// Above calculation done assuming 'big endian' - so convert to host order
	return qFromBigEndian(sum);
}

quint32 StreamInfo::ipv4CksumPartial(uchar *buf, int len)
{
	quint32 sum = 0;
	quint16 *ip = (quint16*) buf;

	if (len & 0x0001)
	{
		qFatal("Cannot calculate partial checksum on non multiple of 2 length");
		return 0;
	}

	while(len)
	{
		sum += *ip;
		if(sum & 0x80000000)
			sum = (sum & 0xFFFF) + (sum >> 16);
		ip++;
		len -= 2;
	}

	return sum;
}

quint16 StreamInfo::ipv4Cksum(uchar *buf, int len, quint32 partialSum)
{
	quint32 sum = partialSum;
	quint16 *ip = (quint16*) buf;

	while(len > 1)
	{
		sum += *ip;
		if(sum & 0x80000000)
			sum = (sum & 0xFFFF) + (sum >> 16);
		ip++;
		len -= 2;
	}

	if (len)
		sum += (unsigned short) *(unsigned char *)ip;

	while(sum>>16)
		sum = (sum & 0xFFFF) + (sum >> 16);

	return (quint16) ~sum;
}

int StreamInfo::makePacket(uchar *buf, int bufMaxSize, int n)
{
	int		u, pktLen, dataLen, len = 0;
	quint32 srcIp, dstIp;	// need it later for TCP/UDP cksum calculation
	quint32 cumCksum = 0;	// cumulative cksum used to combine partial cksums
	int tcpOfs, udpOfs;		// needed to fill in cksum later
	uchar	scratch[8];

	// Decide a frame length based on length mode
	switch(d.core().len_mode())
	{
		case OstProto::StreamCore::e_fl_fixed:
			pktLen = d.core().frame_len();
			break;
		case OstProto::StreamCore::e_fl_inc:
			pktLen = d.core().frame_len_min() + (n %
				(d.core().frame_len_max() - d.core().frame_len_min() + 1));
			break;
		case OstProto::StreamCore::e_fl_dec:
			pktLen = d.core().frame_len_max() - (n %
				(d.core().frame_len_max() - d.core().frame_len_min() + 1));
			break;
		case OstProto::StreamCore::e_fl_random:
			pktLen = d.core().frame_len_min() + (qrand() %
				(d.core().frame_len_max() - d.core().frame_len_min() + 1));
			break;
		default:
			qWarning("Unhandled len mode %d. Using default 64", 
					d.core().len_mode());
			pktLen = 64;
			break;
	}

	// pktLen is adjusted for CRC/FCS which will be added by the NIC
	pktLen -=  4;

	if ((pktLen < 0) || (pktLen > bufMaxSize))
		return 0;

	// We always have a Mac Header!
	switch (d.mac().dst_mac_mode())
	{
		case OstProto::Mac::e_mm_fixed:
			qToBigEndian((quint64) d.mac().dst_mac(), scratch);
			break;
		case OstProto::Mac::e_mm_inc:
			u = (n % d.mac().dst_mac_count()) * d.mac().dst_mac_step(); 
			qToBigEndian((quint64) d.mac().dst_mac() + u, scratch);
			break;
		case OstProto::Mac::e_mm_dec:
			u = (n % d.mac().dst_mac_count()) * d.mac().dst_mac_step(); 
			qToBigEndian((quint64) d.mac().dst_mac() - u, scratch);
			break;
		default:
			qWarning("Unhandled dstMac_mode %d", d.mac().dst_mac_mode());
	}
	memcpy((buf + len), scratch + 2, 6);
	len += 6;

	switch (d.mac().src_mac_mode())
	{
		case OstProto::Mac::e_mm_fixed:
			qToBigEndian((quint64) d.mac().src_mac(), scratch);
			break;
		case OstProto::Mac::e_mm_inc:
			u = (n % d.mac().src_mac_count()) * d.mac().src_mac_step(); 
			qToBigEndian((quint64) d.mac().src_mac() + u, scratch);
			break;
		case OstProto::Mac::e_mm_dec:
			u = (n % d.mac().src_mac_count()) * d.mac().src_mac_step(); 
			qToBigEndian((quint64) d.mac().src_mac() - u, scratch);
			break;
		default:
			qWarning("Unhandled srcMac_mode %d", d.mac().src_mac_mode());
	}
	memcpy((buf + len), scratch + 2, 6);
	len += 6;


	// Frame Type - Part 1 (pre VLAN info)
	switch(d.core().ft())
	{
	case OstProto::StreamCore::e_ft_none:
	case OstProto::StreamCore::e_ft_eth_2:
		break;
	case OstProto::StreamCore::e_ft_802_3_raw:
		qToBigEndian((quint16) pktLen, buf+len);
		len += 2;
		break;
	case OstProto::StreamCore::e_ft_802_3_llc:
		qToBigEndian((quint16) pktLen, buf+len);
		len += 2;
		buf[len+0] = (quint8) d.llc().dsap();
		buf[len+1] = (quint8) d.llc().ssap();
		buf[len+2] = (quint8) d.llc().ctl();
		len +=3;
		break;
	case OstProto::StreamCore::e_ft_snap:
		qToBigEndian((quint16) pktLen, buf+len);
		len += 2;
		buf[len+0] = (quint8) d.llc().dsap();
		buf[len+1] = (quint8) d.llc().ssap();
		buf[len+2] = (quint8) d.llc().ctl();
		len +=3;
		qToBigEndian((quint32) d.snap().oui(), scratch);
		memcpy((buf + len), scratch + 2, 3);
		len += 3;
		break;
	default:
		qWarning("Unhandled frame type %d\n", d.core().ft());
	}

	// VLAN
	if (d.vlan().is_svlan_tagged())
	{
		if (d.vlan().is_stpid_override())
			qToBigEndian((quint16) d.vlan().stpid(), buf+len);
		else
			qToBigEndian((quint16) 0x88a8, buf+len);
		len += 2 ;

		qToBigEndian((quint16) d.vlan().svlan_tag(), buf+len);
		len += 2 ;
	}

	if (d.vlan().is_cvlan_tagged())
	{
		if (d.vlan().is_ctpid_override())
			qToBigEndian((quint16) d.vlan().ctpid(), buf+len);
		else
			qToBigEndian((quint16) 0x8100, buf+len);
		len += 2 ;

		qToBigEndian((quint16) d.vlan().cvlan_tag(), buf+len);
		len += 2 ;
	}

	// Frame Type - Part 2 (post VLAN info)
	switch(d.core().ft())
	{
	case OstProto::StreamCore::e_ft_none:
		break;
	case OstProto::StreamCore::e_ft_eth_2:
		qToBigEndian((quint16) d.eth2().type(), buf+len);
		len += 2;
		break;
	case OstProto::StreamCore::e_ft_802_3_raw:
	case OstProto::StreamCore::e_ft_802_3_llc:
		break;
	case OstProto::StreamCore::e_ft_snap:
		qToBigEndian((quint16) d.eth2().type(), buf+len);
		len += 2;
		break;
	default:
		qWarning("Unhandled frame type %d\n", d.core().ft());
	}

	// L3
	switch (d.core().l3_proto())
	{
	case OstProto::StreamCore::e_l3_none:
		break;
	case OstProto::StreamCore::e_l3_ip:
	{
		quint32 subnet, host;
		int ipOfs = len;

		buf[len+0] = (quint8) (d.ip().ver_hdrlen());
		buf[len+1] = (quint8) (d.ip().tos());
		len += 2;

		if (d.ip().is_override_totlen())
			qToBigEndian((quint16) d.ip().tot_len(), buf+len);
		else
			qToBigEndian((quint16) (pktLen - ipOfs), buf+len);
		len += 2;

		qToBigEndian((quint16) d.ip().id(), buf+len);
		len += 2;

		qToBigEndian((quint16) (( (d.ip().flags() & 0x3) << 13) | 
					(d.ip().frag_ofs() & 0x1FFF)), buf+len);
		len += 2;

		buf[len+0] = (quint8) (d.ip().ttl());
		buf[len+1] = (quint8) (d.ip().proto());
		len += 2;

		// cksum calculated after filling in the rest
		qToBigEndian((quint16) 0, buf+len);
		len += 2;

		// Get Src/Dst IP for this packet using respective IpMode
		switch(d.ip().src_ip_mode())
		{
			case OstProto::Ip::e_im_fixed:
				srcIp = (quint32) d.ip().src_ip();
				qToBigEndian(srcIp, buf+len);
				break;
			case OstProto::Ip::e_im_inc_host:
				u = n % d.ip().src_ip_count();
				subnet = d.ip().src_ip() & d.ip().src_ip_mask();
				host = (((d.ip().src_ip() & ~d.ip().src_ip_mask()) + u) &
					~d.ip().src_ip_mask());
				srcIp = (quint32) (subnet | host);
				qToBigEndian(srcIp, buf+len);
				break;
			case OstProto::Ip::e_im_dec_host:
				u = n % d.ip().src_ip_count();
				subnet = d.ip().src_ip() & d.ip().src_ip_mask();
				host = (((d.ip().src_ip() & ~d.ip().src_ip_mask()) - u) &
					~d.ip().src_ip_mask());
				srcIp = (quint32) (subnet | host);
				qToBigEndian(srcIp, buf+len);
				break;
			case OstProto::Ip::e_im_random_host:
				subnet = d.ip().src_ip() & d.ip().src_ip_mask();
				host = (qrand() & ~d.ip().src_ip_mask());
				srcIp = (quint32) (subnet | host);
				qToBigEndian(srcIp, buf+len);
				break;
			default:
				qWarning("Unhandled src_ip_mode = %d", d.ip().src_ip_mode());
		}
		len +=4;

		switch(d.ip().dst_ip_mode())
		{
			case OstProto::Ip::e_im_fixed:
				dstIp = (quint32) d.ip().dst_ip();
				qToBigEndian(dstIp, buf+len);
				break;
			case OstProto::Ip::e_im_inc_host:
				u = n % d.ip().dst_ip_count();
				subnet = d.ip().dst_ip() & d.ip().dst_ip_mask();
				host = (((d.ip().dst_ip() & ~d.ip().dst_ip_mask()) + u) &
					~d.ip().dst_ip_mask());
				dstIp = (quint32) (subnet | host);
				qToBigEndian(dstIp, buf+len);
				break;
			case OstProto::Ip::e_im_dec_host:
				u = n % d.ip().dst_ip_count();
				subnet = d.ip().dst_ip() & d.ip().dst_ip_mask();
				host = (((d.ip().dst_ip() & ~d.ip().dst_ip_mask()) - u) &
					~d.ip().dst_ip_mask());
				dstIp = (quint32) (subnet | host);
				qToBigEndian(dstIp, buf+len);
				break;
			case OstProto::Ip::e_im_random_host:
				subnet = d.ip().dst_ip() & d.ip().dst_ip_mask();
				host = (qrand() & ~d.ip().dst_ip_mask());
				dstIp = (quint32) (subnet | host);
				qToBigEndian(dstIp, buf+len);
				break;
			default:
				qWarning("Unhandled dst_ip_mode = %d", d.ip().dst_ip_mode());
		}
		len +=4;

		// Calculate and fill in cksum (unless overridden)
		if (d.ip().is_override_cksum())
			qToBigEndian((quint16) d.ip().cksum(), buf+ipOfs+10);
		else
			*((quint16*)(buf + ipOfs + 10)) = ipv4Cksum(buf + ipOfs, len-ipOfs);
		break;

	}
	case OstProto::StreamCore::e_l3_arp:
		// TODO(LOW)
		break;
	default:
		qWarning("Unhandled l3 proto %d\n", d.core().l3_proto());
	}

	switch (d.core().l4_proto())
	{
	case OstProto::StreamCore::e_l4_none:
		break;
	case OstProto::StreamCore::e_l4_tcp:
	{
		tcpOfs = len;

		cumCksum = pseudoHdrCksumPartial(srcIp, dstIp, 6, pktLen - len);

		qToBigEndian((quint16) d.tcp().src_port(), buf+len);
		len += 2;
		qToBigEndian((quint16) d.tcp().dst_port(), buf+len);
		len += 2;

		qToBigEndian((quint32) d.tcp().seq_num(), buf+len);
		len += 4;
		qToBigEndian((quint32) d.tcp().ack_num(), buf+len);
		len += 4;

		if (d.tcp().is_override_hdrlen())
			buf[len+0] = (quint8) d.tcp().hdrlen_rsvd();
		else
			buf[len+0] = (quint8) 0x50; // FIXME(LOW): Hardcoding
		buf[len+1] = (quint8) d.tcp().flags();
		len += 2;

		qToBigEndian((quint16) d.tcp().window(), buf+len);
		len +=2;

		// Fill in cksum as 0 for cksum calculation, actual cksum filled later
		qToBigEndian((quint16) 0, buf+len);
		len +=2;

		qToBigEndian((quint16) d.tcp().urg_ptr(), buf+len);
		len +=2;

		// Accumulate cumulative cksum
		cumCksum += ipv4CksumPartial(buf + tcpOfs, len - tcpOfs);

		break;
	}
	case OstProto::StreamCore::e_l4_udp:
	{
		udpOfs = len;

		cumCksum = pseudoHdrCksumPartial(srcIp, dstIp, 17, pktLen - len);

		qToBigEndian((quint16) d.udp().src_port(), buf+len);
		len += 2;
		qToBigEndian((quint16) d.udp().dst_port(), buf+len);
		len += 2;

		if (d.udp().is_override_totlen())
			qToBigEndian((quint16) d.udp().totlen(), buf+len);
		else
			qToBigEndian((quint16) (pktLen - udpOfs), buf+len);
		len +=2;

		// Fill in cksum as 0 for cksum calculation, actual cksum filled later
		qToBigEndian((quint16) 0, buf+len);
		len +=2;

		// Accumulate cumulative cksum
		cumCksum += ipv4CksumPartial(buf + udpOfs, len - udpOfs);

		break;
	}
	case OstProto::StreamCore::e_l4_icmp:
		// TODO(LOW)
		break;
	case OstProto::StreamCore::e_l4_igmp:
		// TODO(LOW)
		break;
	default:
		qWarning("Unhandled l4 proto %d\n", d.core().l4_proto());
	}

	// Fill-in the data pattern
	dataLen = pktLen - len;
	switch(d.core().pattern_mode())
	{
		case OstProto::StreamCore::e_dp_fixed_word:
			for (int i = 0; i < (dataLen/4)+1; i++)
				qToBigEndian((quint32) d.core().pattern(), buf+len+(i*4));
			break;
		case OstProto::StreamCore::e_dp_inc_byte:
			for (int i = 0; i < dataLen; i++)
				buf[len + i] = i % (0xFF + 1);
			break;
		case OstProto::StreamCore::e_dp_dec_byte:
			for (int i = 0; i < dataLen; i++)
				buf[len + i] = 0xFF - (i % (0xFF + 1));
			break;
		case OstProto::StreamCore::e_dp_random:
			for (int i = 0; i < dataLen; i++)
				buf[len + i] =  qrand() % (0xFF + 1);
			break;
		default:
			qWarning("Unhandled data pattern %d", d.core().pattern_mode());
	}

	// Calculate TCP/UDP checksum over the data pattern/payload and fill in
	switch (d.core().l4_proto())
	{
	case OstProto::StreamCore::e_l4_tcp:
		if (d.tcp().is_override_cksum())
			qToBigEndian((quint16) d.tcp().cksum(), buf + tcpOfs + 16);
		else
			*((quint16*)(buf + tcpOfs + 16)) = 
				ipv4Cksum(buf + len, dataLen, cumCksum);
		break;
	case OstProto::StreamCore::e_l4_udp:
		if (d.udp().is_override_cksum())
			qToBigEndian((quint16) d.udp().cksum(), buf + udpOfs + 6);
		else
			*((quint16*)(buf + udpOfs + 6)) = 
				ipv4Cksum(buf + len, dataLen, cumCksum);
		break;
	case OstProto::StreamCore::e_l4_none:
	case OstProto::StreamCore::e_l4_icmp:
	case OstProto::StreamCore::e_l4_igmp:
		// No cksum processing required
		break;
	}

	return pktLen;
}


//
// ------------------ PortInfo --------------------
//
PortInfo::PortInfo(uint id, pcap_if_t *dev)
	: monitorRx(this), monitorTx(this)
{
    char errbuf[PCAP_ERRBUF_SIZE];

	this->dev = dev;

	/*
	 * Get 2 device handles - one for rx and one for tx. If we use only
	 * one handle for both rx and tx anythin that we tx using the single
	 * handle is not received back to us
	 */
	devHandleRx = pcap_open_live(dev->name, 0, PCAP_OPENFLAG_PROMISCUOUS ,
		1000 /*ms*/, errbuf);
	if (devHandleRx == NULL)
	{
		qDebug("Error opening port %s: %s\n", 
				dev->name, pcap_geterr(devHandleRx));
	}

	if (pcap_setdirection(devHandleRx, PCAP_D_IN)<0)
	{
		qDebug("[%s] Error setting direction inbound only\n", dev->name);
	}

	/* By default, put the interface in statistics mode */
	if (pcap_setmode(devHandleRx, MODE_STAT)<0)
	{
		qDebug("Error setting statistics mode.\n");
	}

	devHandleTx = pcap_open_live(dev->name, 0, PCAP_OPENFLAG_PROMISCUOUS ,
		1000 /*ms*/, errbuf);
	if (devHandleTx == NULL)
	{
		qDebug("Error opening port %s: %s\n", 
				dev->name, pcap_geterr(devHandleTx));
	}

	if (pcap_setdirection(devHandleTx, PCAP_D_OUT)<0)
	{
		qDebug("[%s] Error setting direction outbound only\n", dev->name);
	}

	/* By default, put the interface in statistics mode */
	if (pcap_setmode(devHandleTx, MODE_STAT)<0)
	{
		qDebug("Error setting statistics mode.\n");
	}

	d.mutable_port_id()->set_id(id);

#ifdef Q_OS_WIN32
	d.set_name(QString("if%1").arg(id).toAscii().constData());
#else
	if (dev->name)
		d.set_name(dev->name);
	else
		d.set_name(QString("if%1").arg(id).toAscii().constData());
#endif
	d.set_name(d.name()+pcap_datalink_val_to_name(pcap_datalink(devHandleRx)));

	if (dev->description)
		d.set_description(dev->description);
	d.set_is_enabled(true);	// FIXME(MED):check
	d.set_is_oper_up(true); // FIXME(MED):check
	d.set_is_exclusive_control(false); // FIXME(MED): check

	memset((void*) &stats, 0, sizeof(stats));
	resetStats();

	// We'll create sendqueue later when required
	sendQueue = NULL;
	pcapExtra.sendQueueCumLen.clear();
	pcapExtra.txPkts = 0;
	pcapExtra.txBytes = 0;
	isSendQueueDirty=true;

	// Start the monitor thread
	monitorRx.start();
	monitorTx.start();
}

void PortInfo::update()
{
	uchar		pktBuf[2000];
	pcap_pkthdr	pktHdr;

	qDebug("In %s", __FUNCTION__);

	if (sendQueue)
		pcap_sendqueue_destroy(sendQueue);

	// TODO(LOW): calculate sendqueue size
	sendQueue = pcap_sendqueue_alloc(1*MB);
	pcapExtra.sendQueueCumLen.clear();
	
	// First sort the streams by ordinalValue
	qSort(streamList);

	for (int i = 0; i < streamList.size(); i++)
	{
		if (streamList[i].d.core().is_enabled())
		{
			int	numPackets, numBursts;

			switch (streamList[i].d.control().unit())
			{
			case OstProto::StreamControl::e_su_bursts:
				numBursts = streamList[i].d.control().num_bursts();
				numPackets = streamList[i].d.control().packets_per_burst();
				break;
			case OstProto::StreamControl::e_su_packets:
				numBursts = 1;
				numPackets = streamList[i].d.control().num_packets();
				break;
			default:
				qWarning("Unhandled stream control unit %d",
					streamList[i].d.control().unit());
				continue;
			}


			for (int j = 0; j < numBursts; j++)
			{
				for (int k = 0; k < numPackets; k++)
				{
					int len;

					len = streamList[i].makePacket(pktBuf, sizeof(pktBuf), 
							j * numPackets + k);
					if (len > 0)
					{
						pktHdr.caplen = pktHdr.len = len;
						pktHdr.ts.tv_sec = pktHdr.ts.tv_usec = 0; // FIXME(HI)

						if (-1 == pcap_sendqueue_queue(sendQueue, &pktHdr, 
									(u_char*) pktBuf))
						{
							qDebug("[port %d] sendqueue_queue() failed for "
									"streamidx %d\n", id(), i);
						}
						else
							pcapExtra.sendQueueCumLen.append(sendQueue->len);
					}
				}
			}
		}
	}

	isSendQueueDirty = false;
}

void PortInfo::startTransmit()
{
	uint bytes, pkts;

	// TODO(HI): Stream Mode - one pass/continuous
	// NOTE: Transmit on the Rx Handle so that we can receive it back
	// on the Tx Handle to do stats
	bytes = pcap_sendqueue_transmit(devHandleRx, sendQueue, false);
	if (bytes < sendQueue->len)
	{	
		qDebug("port %d: sent (%d/%d) error %s. TxStats may be inconsistent", 
				id(), bytes, sendQueue->len, pcap_geterr(devHandleTx));

		// parse sendqueue using 'bytes' to get actual pkts sent
#if 0
		// FIXME(LOW): Get this working
		pkts = qUpperBound(pcapExtra.sendQueueCumLen, bytes);
#else
		for (int i = 0; i < pcapExtra.sendQueueCumLen.size(); i++)
		{
			if (pcapExtra.sendQueueCumLen.at(i) > bytes)
			{
				pkts = i;
				break;
			}
		}
#endif
	}
	else
	{
		qDebug("port %d: sent (%d/%d) bytes\n", id(), bytes, sendQueue->len);
		pkts = pcapExtra.sendQueueCumLen.size();
	}

	// pcap_sendqueue_transmit() returned 'bytes' includes size of pcap_pkthdr
	// - adjust for it
	if (bytes)
		bytes -= pkts * sizeof(pcap_pkthdr);
#ifdef Q_OS_WIN32
	// Update pcapExtra counters - port TxStats will be updated in the
	// 'stats callback' function so that both Rx and Tx stats are updated
	// together
	pcapExtra.txPkts += pkts;
	pcapExtra.txBytes += bytes;
#endif
}

void PortInfo::stopTransmit()
{
}

void PortInfo::resetStats()
{
	memcpy((void*) &epochStats, (void*) &stats, sizeof(stats));
}

//
// ------------------ PortMonitor -------------------
//

PortInfo::PortMonitorRx::PortMonitorRx(PortInfo *port)
{
	this->port = port;
#ifdef Q_OS_WIN32
	{
		int sz = sizeof(PACKET_OID_DATA) + sizeof(quint64) + 4;
		//oidData = GlobalAllocPtr(GMEM_MOVEABLE | GMEM_ZEROINIT,
			//sizeof(PACKET_OID_DATA) + sizeof(quint64) - 1);
		oidData = (PPACKET_OID_DATA) malloc(sz);
		if (oidData)
		{
			memset(oidData, 0, sz);
			oidData->Length=sizeof(quint64);
		}
		else
			qFatal("failed to alloc oidData");
	}
#endif
}

PortInfo::PortMonitorTx::PortMonitorTx(PortInfo *port)
{
	this->port = port;
#ifdef Q_OS_WIN32
	{
		int sz = sizeof(PACKET_OID_DATA) + sizeof(quint64) + 4;
		//oidData = GlobalAllocPtr(GMEM_MOVEABLE | GMEM_ZEROINIT,
			//sizeof(PACKET_OID_DATA) + sizeof(quint64) - 1);
		oidData = (PPACKET_OID_DATA) malloc(sz);
		if (oidData)
		{
			memset(oidData, 0, sz);
			oidData->Length=sizeof(quint64);
		}
		else
			qFatal("failed to alloc oidData");
	}
#endif
}

#ifdef Q_OS_WIN32
void PortInfo::PortMonitorRx::callbackRx(u_char *state, 
		const struct pcap_pkthdr *header, const u_char *pkt_data)
{
	// This is the WinPcap Callback - which is a 'stats mode' callback

	uint		usec;
	PortInfo	*port = (PortInfo*) state;

	quint64 pkts;
	quint64 bytes;

	// Update RxStats and RxRates using PCAP data
	pkts  = *((quint64*)(pkt_data + 0));
	bytes = *((quint64*)(pkt_data + 8));

	// Note: PCAP reported bytes includes ETH_FRAME_HDR_SIZE - adjust for it
	bytes -= pkts * ETH_FRAME_HDR_SIZE;

	usec = (header->ts.tv_sec - port->lastTsRx.tv_sec) * 1000000 + 
		(header->ts.tv_usec - port->lastTsRx.tv_usec);
	port->stats.rxPps = (pkts * 1000000) / usec;
	port->stats.rxBps = (bytes * 1000000) / usec;

	port->stats.rxPkts += pkts;
	port->stats.rxBytes += bytes;

	// Store curr timestamp as last timestamp
	port->lastTsRx.tv_sec = header->ts.tv_sec;
	port->lastTsRx.tv_usec = header->ts.tv_usec;

#if 0
	for (int i=0; i < 16; i++)
	{
		qDebug("%02x ", pkt_data[i]);
	}
	qDebug("{%d: %llu, %llu}\n", port->id(),
			pkts, bytes);
	qDebug("[%d: pkts : %llu]\n", port->id(), port->stats.rxPkts);
	qDebug("[%d: bytes: %llu]\n", port->id(), port->stats.rxBytes);
#endif

	// Retreive NIC stats
#ifdef Q_OS_WIN32
	port->monitorRx.oidData->Oid = OID_GEN_RCV_OK;
	if (PacketRequest(port->devHandleRx->adapter, 0, port->monitorRx.oidData))
	{
		if (port->monitorRx.oidData->Length <= sizeof(port->stats.rxPktsNic))
			memcpy((void*)&port->stats.rxPktsNic,
					(void*)port->monitorRx.oidData->Data, 
					port->monitorRx.oidData->Length);
	}
#endif
}
void PortInfo::PortMonitorTx::callbackTx(u_char *state, 
		const struct pcap_pkthdr *header, const u_char *pkt_data)
{
	// This is the WinPcap Callback - which is a 'stats mode' callback

	uint		usec;
	PortInfo	*port = (PortInfo*) state;

	quint64 pkts;
	quint64 bytes;

	// Update RxStats and RxRates using PCAP data
	pkts  = *((quint64*)(pkt_data + 0));
	bytes = *((quint64*)(pkt_data + 8));

	// Note: PCAP reported bytes includes ETH_FRAME_HDR_SIZE - adjust for it
	bytes -= pkts * ETH_FRAME_HDR_SIZE;

	usec = (header->ts.tv_sec - port->lastTs.tv_sec) * 1000000 + 
		(header->ts.tv_usec - port->lastTs.tv_usec);
	port->stats.rxPps = (pkts * 1000000) / usec;
	port->stats.rxBps = (bytes * 1000000) / usec;

	port->stats.rxPkts += pkts;
	port->stats.rxBytes += bytes;

	// Since WinPCAP (due to NDIS limitation) cannot distinguish between
	// rx/tx packets, pcap stats are not of much use - for the tx stats
	// update from PcapExtra

	pkts  = port->pcapExtra.txPkts - port->stats.txPkts;
	bytes  = port->pcapExtra.txBytes - port->stats.txBytes;

	// Use the pcap timestamp for rate calculation though
	usec = (header->ts.tv_sec - port->lastTs.tv_sec) * 1000000 + 
		(header->ts.tv_usec - port->lastTs.tv_usec);
	port->stats.txPps = (pkts * 1000000) / usec;
	port->stats.txBps = (bytes * 1000000) / usec;

	port->stats.txPkts = port->pcapExtra.txPkts;
	port->stats.txBytes = port->pcapExtra.txBytes;

	// Store curr timestamp as last timestamp
	port->lastTsTx.tv_sec = header->ts.tv_sec;
	port->lastTsTx.tv_usec = header->ts.tv_usec;

#if 0
	for (int i=0; i < 16; i++)
	{
		qDebug("%02x ", pkt_data[i]);
	}
	qDebug("{%d: %llu, %llu}\n", port->id(),
			pkts, bytes);
	qDebug("[%d: pkts : %llu]\n", port->id(), port->stats.rxPkts);
	qDebug("[%d: bytes: %llu]\n", port->id(), port->stats.rxBytes);
#endif

	// Retreive NIC stats
#ifdef Q_OS_WIN32
	port->monitorTx.oidData->Oid = OID_GEN_XMIT_OK;
	if (PacketRequest(port->devHandleTx->adapter, 0, port->monitorTx.oidData))
	{
		if (port->monitorTx.oidData->Length <= sizeof(port->stats.txPktsNic))
			memcpy((void*)&port->stats.txPktsNic,
					(void*)port->monitorTx.oidData->Data, 
					port->monitorTx.oidData->Length);
	}
#endif
}
#else
void PortInfo::PortMonitorRx::callbackRx(u_char *state, 
		const struct pcap_pkthdr *header, const u_char *pkt_data)
{
	// This is the LibPcap Callback - which is a 'capture mode' callback
	// This callback is called once for EVERY packet

	uint		usec;
	PortInfo	*port = (PortInfo*) state;

	quint64 pkts;
	quint64 bytes;

	// Update RxStats and RxRates using PCAP data
	usec = (header->ts.tv_sec - port->lastTsRx.tv_sec) * 1000000 + 
		(header->ts.tv_usec - port->lastTsRx.tv_usec);
	// TODO(rate)
#if 0
	port->stats.rxPps = (pkts * 1000000) / usec;
	port->stats.rxBps = (bytes * 1000000) / usec;
#endif

	// Note: For a 'capture callback' PCAP reported bytes DOES NOT include
	// ETH_FRAME_HDR_SIZE - so don't adjust for it
	port->stats.rxPkts++;
	port->stats.rxBytes += header->len;

	// Store curr timestamp as last timestamp
	port->lastTsRx.tv_sec = header->ts.tv_sec;
	port->lastTsRx.tv_usec = header->ts.tv_usec;
}

void PortInfo::PortMonitorTx::callbackTx(u_char *state, 
		const struct pcap_pkthdr *header, const u_char *pkt_data)
{
	// This is the LibPcap Callback - which is a 'capture mode' callback
	// This callback is called once for EVERY packet

	uint		usec;
	PortInfo	*port = (PortInfo*) state;

	quint64 pkts;
	quint64 bytes;

	// Update TxStats and TxRates using PCAP data
	usec = (header->ts.tv_sec - port->lastTsTx.tv_sec) * 1000000 + 
		(header->ts.tv_usec - port->lastTsTx.tv_usec);
	// TODO(rate)
#if 0
	port->stats.txPps = (pkts * 1000000) / usec;
	port->stats.txBps = (bytes * 1000000) / usec;
#endif

	// Note: For a 'capture callback' PCAP reported bytes DOES NOT include
	// ETH_FRAME_HDR_SIZE - so don't adjust for it

	port->stats.txPkts++;
	port->stats.txBytes += header->len;

	// Store curr timestamp as last timestamp
	port->lastTsTx.tv_sec = header->ts.tv_sec;
	port->lastTsTx.tv_usec = header->ts.tv_usec;
}
#endif
void PortInfo::PortMonitorRx::run()
{
	int		ret;

	qDebug("before pcap_loop rx \n");

	/* Start the main loop */
	ret = pcap_loop(port->devHandleRx, -1,
			&PortInfo::PortMonitorRx::callbackRx, (u_char*) port);

	switch(ret)
	{
		case 0:
			qDebug("Unexpected return from pcap_loop()\n");
			break;
		case -1:
			qDebug("Unsolicited (error) return from pcap_loop()\n");
			break;
		case -2:
			qDebug("Solicited return from pcap_loop()\n");
			break;
		default:
			qDebug("Unknown return value from pcap_loop()\n");
	}
}

void PortInfo::PortMonitorTx::run()
{
	int		ret;

	qDebug("before pcap_loopTx\n");

	/* Start the main loop */
	ret = pcap_loop(port->devHandleTx, -1,
			&PortInfo::PortMonitorTx::callbackTx, (u_char*) port);

	switch(ret)
	{
		case 0:
			qDebug("Unexpected return from pcap_loop()\n");
			break;
		case -1:
			qDebug("Unsolicited (error) return from pcap_loop()\n");
			break;
		case -2:
			qDebug("Solicited return from pcap_loop()\n");
			break;
		default:
			qDebug("Unknown return value from pcap_loop()\n");
	}
}

/*--------------- MyService ---------------*/

int MyService::getStreamIndex(unsigned int portIdx,
	unsigned int streamId)
{
	int i;

	// note: index and id are interchageable for port but not for stream

	Q_ASSERT(portIdx < numPorts);

	for (i = 0; i < portInfo[portIdx]->streamList.size(); i++)
	{
		if (streamId == portInfo[portIdx]->streamList.at(i).d.stream_id().id())
			goto _found;
	}

	qDebug("%s: stream id %d not found", __PRETTY_FUNCTION__, streamId);
	return -1;

_found:
	return i;
}

MyService::MyService(AbstractHost *host)
{
    pcap_if_t *dev;
    int i=0;
    char errbuf[PCAP_ERRBUF_SIZE];

	// Init Data
	this->host = host;
	numPorts = 0;
	alldevs = NULL;

    LOG("Retrieving the device list from the local machine\n"); 
    if (pcap_findalldevs(&alldevs, errbuf) == -1)
    {
        LOG("Error in pcap_findalldevs_ex: %s\n", errbuf);
        goto _fail;
    }

	portInfo.clear();
    /* Count, Populate and Print the list */
    for(i=0, dev=alldevs; dev!=NULL; i++, dev=dev->next)
    {
		portInfo.append(new PortInfo(i, dev));
		numPorts++;

#if 1
        LOG("%d. %s", i, dev->name);
        if (dev->description)
		{
            LOG(" (%s)\n", dev->description);
		}
#endif
    }
    
    if (i == 0)
    {
        LOG("\nNo interfaces found! Make sure WinPcap is installed.\n");
        goto _fail;
    }

_fail:
	return;
}

MyService::~MyService()
{
    pcap_freealldevs(alldevs);
}

void MyService::getPortIdList(
	::google::protobuf::RpcController* controller,
	const ::OstProto::Void* request,
	::OstProto::PortIdList* response,
	::google::protobuf::Closure* done)
{
	qDebug("In %s", __PRETTY_FUNCTION__);

	for (uint i = 0; i < numPorts; i++)
	{
		::OstProto::PortId	*p;

		p = response->add_port_id();
		p->set_id(portInfo[i]->d.port_id().id());
	}

	done->Run();
}

void MyService::getPortConfig(::google::protobuf::RpcController* controller,
const ::OstProto::PortIdList* request,
::OstProto::PortConfigList* response,
::google::protobuf::Closure* done)
{
	qDebug("In %s", __PRETTY_FUNCTION__);

	for (int i=0; i < request->port_id_size(); i++)
	{
		unsigned int idx;

		idx = request->port_id(i).id();
		if (idx < numPorts)
		{
			OstProto::Port	*p;

			p = response->add_port();
			p->CopyFrom(portInfo[idx]->d);
		}
	}

	done->Run();
}

void MyService::getStreamIdList(::google::protobuf::RpcController* controller,
const ::OstProto::PortId* request,
::OstProto::StreamIdList* response,
::google::protobuf::Closure* done)
{
	unsigned int portIdx;

	qDebug("In %s", __PRETTY_FUNCTION__);

	portIdx = request->id();
	if (portIdx >= numPorts)
	{
		qDebug("%s: Invalid port id %d", __PRETTY_FUNCTION__, portIdx);
		controller->SetFailed("Invalid Port Id");
		goto _exit;		// TODO(LOW): Partial status of RPC
	}

	response->mutable_port_id()->set_id(portIdx);
	for (int j = 0; j < portInfo[portIdx]->streamList.size(); j++)
	{
		OstProto::StreamId	*s, *q;

		q = portInfo[portIdx]->streamList[j].d.mutable_stream_id();

		s = response->add_stream_id();
		s->CopyFrom(*q);
	}

_exit:
	done->Run();
}

void MyService::getStreamConfig(::google::protobuf::RpcController* controller,
const ::OstProto::StreamIdList* request,
::OstProto::StreamConfigList* response,
::google::protobuf::Closure* done)
{
	unsigned int portIdx;

	qDebug("In %s", __PRETTY_FUNCTION__);

	portIdx = request->port_id().id();
	if (portIdx >= numPorts)
	{
		controller->SetFailed("invalid portid");
		goto _exit;
	}

	response->mutable_port_id()->set_id(portIdx);
	for (int i = 0; i < request->stream_id_size(); i++)
	{
		int streamIndex;
		OstProto::Stream	*s;

		streamIndex = getStreamIndex(portIdx, request->stream_id(i).id());
		if (streamIndex < 0)
			continue;		// TODO(LOW): Partial status of RPC

		s = response->add_stream();
		s->CopyFrom(portInfo[portIdx]->streamList[streamIndex].d);
	}

_exit:
	done->Run();
}

void MyService::addStream(::google::protobuf::RpcController* controller,
const ::OstProto::StreamIdList* request,
::OstProto::Ack* response,
::google::protobuf::Closure* done)
{
	unsigned int portIdx;

	qDebug("In %s", __PRETTY_FUNCTION__);

	portIdx = request->port_id().id();
	if (portIdx >= numPorts)
	{
		controller->SetFailed("invalid portid");
		goto _exit;
	}

	for (int i = 0; i < request->stream_id_size(); i++)
	{
		int	streamIndex;
		StreamInfo		s;

		// If stream with same id as in request exists already ==> error!!
		streamIndex = getStreamIndex(portIdx, request->stream_id(i).id());
		if (streamIndex >= 0)
			continue;		// TODO(LOW): Partial status of RPC

		// Append a new "default" stream - actual contents of the new stream is
		// expected in a subsequent "modifyStream" request - set the stream id
		// now itself however!!!
		s.d.mutable_stream_id()->CopyFrom(request->stream_id(i));
		portInfo[portIdx]->streamList.append(s);

		// TODO(LOW): fill-in response "Ack"????
	}
	portInfo[portIdx]->setDirty(true);
_exit:
	done->Run();
}

void MyService::deleteStream(::google::protobuf::RpcController* controller,
const ::OstProto::StreamIdList* request,
::OstProto::Ack* response,
::google::protobuf::Closure* done)
{
	unsigned int portIdx;

	qDebug("In %s", __PRETTY_FUNCTION__);

	portIdx = request->port_id().id();
	if (portIdx >= numPorts)
	{
		controller->SetFailed("invalid portid");
		goto _exit;
	}

	for (int i = 0; i < request->stream_id_size(); i++)
	{
		int	streamIndex;
		StreamInfo		s;

		streamIndex = getStreamIndex(portIdx, request->stream_id(i).id());
		if (streamIndex < 0)
			continue;		// TODO(LOW): Partial status of RPC

		portInfo[portIdx]->streamList.removeAt(streamIndex);

		// TODO(LOW): fill-in response "Ack"????
	}
	portInfo[portIdx]->setDirty(true);
_exit:
	done->Run();
}

void MyService::modifyStream(::google::protobuf::RpcController* controller,
const ::OstProto::StreamConfigList* request,
::OstProto::Ack* response,
::google::protobuf::Closure* done)
{
	unsigned int	portIdx;

	qDebug("In %s", __PRETTY_FUNCTION__);

	portIdx = request->port_id().id();
	if (portIdx >= numPorts)
	{
		controller->SetFailed("invalid portid");
		goto _exit;
	}

	for (int i = 0; i < request->stream_size(); i++)
	{
		int streamIndex;

		streamIndex = getStreamIndex(portIdx, 
			request->stream(i).stream_id().id());
		if (streamIndex < 0)
			continue;		// TODO(LOW): Partial status of RPC

		portInfo[portIdx]->streamList[streamIndex].d.MergeFrom(
			request->stream(i));

		// TODO(LOW): fill-in response "Ack"????
	}
	portInfo[portIdx]->setDirty(true);
_exit:
	done->Run();
}

void MyService::startTx(::google::protobuf::RpcController* controller,
const ::OstProto::PortIdList* request,
::OstProto::Ack* response,
::google::protobuf::Closure* done)
{
	qDebug("In %s", __PRETTY_FUNCTION__);

	// If any of the ports in the request are dirty, first update them
	for (int i=0; i < request->port_id_size(); i++)
	{
		uint portIdx;

		portIdx = request->port_id(i).id();
		if (portIdx >= numPorts)
			continue; 	// TODO(LOW): partial RPC?

		if (portInfo[portIdx]->isDirty())
			portInfo[portIdx]->update();
	}

	for (int i=0; i < request->port_id_size(); i++)
	{
		uint portIdx;

		portIdx = request->port_id(i).id();
		if (portIdx >= numPorts)
			continue; 	// TODO(LOW): partial RPC?

		portInfo[portIdx]->startTransmit();
	}

	// TODO(LOW): fill-in response "Ack"????

	done->Run();
}

void MyService::stopTx(::google::protobuf::RpcController* controller,
const ::OstProto::PortIdList* request,
::OstProto::Ack* response,
::google::protobuf::Closure* done)
{
	qDebug("In %s", __PRETTY_FUNCTION__);

	for (int i=0; i < request->port_id_size(); i++)
	{
		uint portIdx;

		portIdx = request->port_id(i).id();
		if (portIdx >= numPorts)
			continue; 	// TODO(LOW): partial RPC?

		portInfo[portIdx]->stopTransmit();
	}
	// TODO(LOW): fill-in response "Ack"????
	done->Run();
}

void MyService::startCapture(::google::protobuf::RpcController* controller,
const ::OstProto::PortIdList* request,
::OstProto::Ack* response,
::google::protobuf::Closure* done)
{
	qDebug("In %s", __PRETTY_FUNCTION__);
	controller->SetFailed("Not Implemented");
	done->Run();
}

void MyService::stopCapture(::google::protobuf::RpcController* controller,
const ::OstProto::PortIdList* request,
::OstProto::Ack* response,
::google::protobuf::Closure* done)
{
	qDebug("In %s", __PRETTY_FUNCTION__);
	controller->SetFailed("Not Implemented");
	done->Run();
}

void MyService::getCaptureBuffer(::google::protobuf::RpcController* controller,
const ::OstProto::PortIdList* request,
::OstProto::CaptureBufferList* response,
::google::protobuf::Closure* done)
{
	qDebug("In %s", __PRETTY_FUNCTION__);
	controller->SetFailed("Not Implemented");
	done->Run();
}

void MyService::getStats(::google::protobuf::RpcController* controller,
const ::OstProto::PortIdList* request,
::OstProto::PortStatsList* response,
::google::protobuf::Closure* done)
{
	//qDebug("In %s", __PRETTY_FUNCTION__);

	for (int i=0; i < request->port_id_size(); i++)
	{
		uint 	portidx;
		::OstProto::PortStats	*s;

		portidx = request->port_id(i).id();
		if (portidx >= numPorts)
			continue; 	// TODO(LOW): partial rpc?

		s = response->add_port_stats();
		s->mutable_port_id()->set_id(request->port_id(i).id());

		s->set_rx_pkts(portInfo[portidx]->stats.rxPkts -
				portInfo[portidx]->epochStats.rxPkts);
		s->set_rx_bytes(portInfo[portidx]->stats.rxBytes -
				portInfo[portidx]->epochStats.rxBytes);
		s->set_rx_pkts_nic(portInfo[portidx]->stats.rxPktsNic -
				portInfo[portidx]->epochStats.rxPktsNic);
		s->set_rx_bytes_nic(portInfo[portidx]->stats.rxBytesNic -
				portInfo[portidx]->epochStats.rxBytesNic);
		s->set_rx_pps(portInfo[portidx]->stats.rxPps);
		s->set_rx_bps(portInfo[portidx]->stats.rxBps);

		s->set_tx_pkts(portInfo[portidx]->stats.txPkts -
				portInfo[portidx]->epochStats.txPkts);
		s->set_tx_bytes(portInfo[portidx]->stats.txBytes -
				portInfo[portidx]->epochStats.txBytes);
		s->set_tx_pkts_nic(portInfo[portidx]->stats.txPktsNic -
				portInfo[portidx]->epochStats.txPktsNic);
		s->set_tx_bytes_nic(portInfo[portidx]->stats.txBytesNic -
				portInfo[portidx]->epochStats.txBytesNic);
		s->set_tx_pps(portInfo[portidx]->stats.txPps);
		s->set_tx_bps(portInfo[portidx]->stats.txBps);
	}

	done->Run();
}

void MyService::clearStats(::google::protobuf::RpcController* controller,
const ::OstProto::PortIdList* request,
::OstProto::Ack* response,
::google::protobuf::Closure* done)
{
	qDebug("In %s", __PRETTY_FUNCTION__);

	for (int i=0; i < request->port_id_size(); i++)
	{
		uint portIdx;

		portIdx = request->port_id(i).id();
		if (portIdx >= numPorts)
			continue; 	// TODO(LOW): partial RPC?

		portInfo[portIdx]->resetStats();
	}
	// TODO(LOW): fill-in response "Ack"????

	done->Run();
}
