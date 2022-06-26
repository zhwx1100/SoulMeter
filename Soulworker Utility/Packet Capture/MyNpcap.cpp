#include "pch.h"
#include ".\Packet Capture\MyNpcap.h"
#include ".\Packet Capture\PacketParser.h"
#include ".\Packet Capture\PacketCapture.h"
#include ".\UI\Option.h"
#include ".\Third Party\Npcap\Include\pcap.h"

DWORD MyNpcap::LoadNpcapDlls() {

	DWORD error = ERROR_NOT_FOUND;

	do {
		TCHAR npcap_dir[MAX_PATH + 3];
		UINT len;

		len = GetSystemDirectory(npcap_dir, MAX_PATH);

		if (!len) {
			Log::WriteLog(const_cast<LPTSTR>(_T("Error in GetSystemDirectory: %x")), GetLastError());
			error = ERROR_FUNCTION_FAILED;
			break;
		}

		_tcscat_s(npcap_dir, MAX_PATH, TEXT("\\Npcap"));

		if (SetDllDirectory(npcap_dir) == 0) {
			Log::WriteLog(const_cast<LPTSTR>(_T("Error in SetDllDirectory: %x")), GetLastError());
			error = ERROR_FUNCTION_FAILED;
			break;
		}

		_tcscat_s(npcap_dir, MAX_PATH + 3, L"\\*");

		WIN32_FIND_DATA ffd;
		HANDLE hFind = FindFirstFile(npcap_dir, &ffd);

		if (INVALID_HANDLE_VALUE == hFind)
		{
			error = ERROR_NOT_FOUND;
			break;
		}

		do
		{
			if (wcscmp(ffd.cFileName, L"wpcap.dll") == 0) {
				error = ERROR_SUCCESS;
				break;
			}

		} while (FindNextFile(hFind, &ffd) != 0);

	} while (false);

	return error;
}

DWORD MyNpcap::Filter(pcap_t* device) {

	DWORD error = ERROR_SUCCESS;

	do {

		if (device == nullptr) {
			error = ERROR_INVALID_PARAMETER;
			break;
		}

		bpf_program fcode;

		if (pcap_compile(device, &fcode, NPCAP_FILTER_RULE, 1, NULL) < 0) {
			error = ERROR_API_UNAVAILABLE;
			Log::WriteLog(const_cast<LPTSTR>(_T("Error in pcap_compile")));
			break;
		}

		if (pcap_setfilter(device, &fcode) < 0) {
			error = ERROR_API_UNAVAILABLE;
			Log::WriteLog(const_cast<LPTSTR>(_T("Error in pcap_setfilter")));
			break;
		}

	} while (false);

	return error;
}

DWORD MyNpcap::Init() {

	DWORD error = ERROR_SUCCESS;

	pcap_if_t* alldevs = nullptr;

	char temp[MAX_BUFFER_LENGTH] = { 0 };

	do {
		if (LoadNpcapDlls()) {
			error = ERROR_DLL_NOT_FOUND;
			break;
		}

		if (pcap_findalldevs_ex(const_cast<char*>(PCAP_SRC_IF_STRING), NULL, &alldevs, temp) == -1) {
			Log::WriteLogA(temp);
			error = ERROR_API_UNAVAILABLE;
			break;
		}

		for (pcap_if_t* d = alldevs; d != nullptr; d = d->next) {

			pcap_t* device = nullptr;

			if ((device = pcap_open(d->name, 65536, PCAP_OPENFLAG_PROMISCUOUS | PCAP_OPENFLAG_NOCAPTURE_RPCAP, 10, NULL, temp)) == NULL)
				continue;

			if (Filter(device))
				continue;

			CreateThread(NULL, 0, CreatePcapLoop, device, 0, NULL);
		}

		pcap_freealldevs(alldevs);

	} while (false);

	return error;
}

DWORD MyNpcap::CreatePcapLoop(LPVOID pAlldevs)
{
	pcap_t* device = (pcap_t*)pAlldevs;

	//int res;
	//pcap_pkthdr* header = nullptr;
	//const unsigned char* pkt_data = nullptr;
	pcap_loop(device, 0, ReceiveCallback, nullptr);
	//while ((res = pcap_next_ex(device, &header, &pkt_data)) >= 0) 
	//{
	//	ReceiveCallback(nullptr, header, pkt_data);
	//}

	return 0;
}

VOID MyNpcap::ReceiveCallback(u_char* prc, const struct pcap_pkthdr* header, const u_char* pkt_data) {

	if (prc != nullptr || pkt_data == nullptr || header == nullptr || PACKETCAPTURE.isStopCaptue()) {
		return;
	}

	BYTE* new_pkt_data = new BYTE[header->caplen];
	memcpy(new_pkt_data, pkt_data, header->caplen);

	CHAR tmp[128] = { 0 };
	sprintf_s(tmp, "%d%d", header->ts.tv_sec, header->ts.tv_usec);

	IPv4Packet* packet = new IPv4Packet;
	PACKETCAPTURE.ParseNpcapStruct(packet, (BYTE*)new_pkt_data, header->caplen);

	//Log::WriteLogA("[MyNpcap::ReceiveCallback] CapLen: %d, Length: %d", header->caplen, header->len);

	mutex* pMutex = nullptr;
	mutex* pQueueMutex = nullptr;
	ULONG* SEQ = nullptr;
	if (packet->_isRecv) {
		pMutex = PACKETCAPTURE.GetRecvMutex();
		pQueueMutex = PACKETCAPTURE.GetRecvQueueMutex();
	}
	else {
		pMutex = PACKETCAPTURE.GetSendMutex();
		pQueueMutex = PACKETCAPTURE.GetSendQueueMutex();
	}

	if (packet->_tcpHeader->syn) {
		PACKETCAPTURE.SetPause(TRUE);
		pMutex->lock();

		PACKETCAPTURE.ClearQueue(packet->_isRecv);
		PACKETCAPTURE.SetSEQ(packet->_tcpHeader->seq_number + 1, packet->_isRecv);

		pMutex->unlock();
		PACKETCAPTURE.SetPause(FALSE);
	}
	else if (!PACKETCAPTURE.isInitRecv() || !PACKETCAPTURE.isInitSend()) {
		PACKETCAPTURE.SetPause(TRUE);
		pMutex->lock();

		PACKETCAPTURE.SetSEQ(packet->_tcpHeader->seq_number, packet->_isRecv);

		pMutex->unlock();
		PACKETCAPTURE.SetPause(FALSE);
	}

	PacketInfo* pi = new PacketInfo;
	pi->_packet = packet;
	

	if (packet->_datalength > 0 && header->caplen >= 64)
	{
		pQueueMutex->lock();
		if (packet->_isRecv)
			PACKETCAPTURE.GetRecvQueue()->emplace(packet->_tcpHeader->seq_number, pi);
		else
			PACKETCAPTURE.GetSendQueue()->emplace(packet->_tcpHeader->seq_number, pi);
		pQueueMutex->unlock();
	}
	else
		PACKETCAPTURE.ClearPacketInfo(pi);
}