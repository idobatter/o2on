/* Copyright (C) 2006 o2on project. All rights reserved.
 * http://o2on.net/
 */

/*
 * project		: o2on
 * filename		: O2Job_SearchFriends.h
 * description	: 
 *
 */

#pragma once
#include "O2Job.h"
#include "O2Logger.h"
#include "O2Profile.h"
#include "O2NodeDB.h"
#include "O2FriendDB.h"
#include "O2Client.h"
#include "O2Protocol_Kademlia.h"
#include "dataconv.h"

#define MAX_SESSION_FOR_REFINE	3
#define REFINE_BORDER			5
#define TRACE_REFINE			0




class O2Job_SearchFriends
	: public O2Job
	, public O2Protocol_Kademlia
{
protected:
	O2Logger		*Logger;
	O2Profile		*Profile;
	O2NodeDB		*NodeDB;
	O2FriendDB		*FriendDB;
	O2Client		*Client;

public:
	O2Job_SearchFriends(const wchar_t	*name
			   , time_t			interval
			   , bool			startup
			   , O2Logger		*lgr
			   , O2Profile		*prof
			   , O2NodeDB		*ndb
			   , O2FriendDB		*fdb
			   , O2Client		*client)
		: O2Job(name, interval, startup)
		, Logger(lgr)
		, Profile(prof)
		, NodeDB(ndb)
		, FriendDB(fdb)
		, Client(client)
	{
	}

	~O2Job_SearchFriends()
	{
	}

	void JobThreadFunc(void)
	{
		O2NodeDB::NodeListT friends;
		if (FriendDB->GetFriends(friends) == 0)
			return;

		hashT myID;
		Profile->GetID(myID);

		// friendでループ
		O2NodeDB::NodeListT::iterator fit;
		for (fit = friends.begin(); fit != friends.end() && IsActive(); fit++) {
			if (time(NULL) - fit->lastlink < O2_FRIEND_LIVING_TT)
				continue;

			O2NodeDB::NodeListT neighbors;
			if (NodeDB->neighbors(fit->id, neighbors, false) == 0)
				break;

			O2NodeDB::NodeListT::iterator nit;
			for (nit = neighbors.begin(); nit != neighbors.end(); nit++) {
				nit->lastlink = 0;
				nit->reset();
			}

			bool found = false;

			// Refine
			O2SocketSessionPList SessionList;
			while (!found && IsActive()) {
				Sleep(SEARCH_REFINE_INTERVAL_MS);

				// α個を選びFIND_VALUEを送る
				size_t i = 0;
				for (nit = neighbors.begin(); nit != neighbors.end() && i < REFINE_BORDER && IsActive(); nit++, i++) {
					if (SessionList.size() >= MAX_SESSION_FOR_REFINE)
						break;
					if (nit->lastlink != 0)
						continue;

					// FIND_NODE発行
					O2SocketSession *ss = new O2SocketSession();
					ss->ip = nit->ip;
					ss->port = nit->port;
					ss->node = *nit;
					O2Protocol_Kademlia pk;
					MakeRequest_Kademlia_FINDNODE(ss, Profile, fit->id, ss->sbuff);

					//先にlastlinkセットしておく
					//そうしないと次回RefineでSessionListに追加されてしまう
					nit->lastlink = time(NULL);

					SessionList.push_back(ss);
					Client->AddRequest(ss);
				}

				if (SessionList.empty())
					break;

				// 返答を待つ
				std::vector<HANDLE> handles;
				O2SocketSessionPListIt sit;
				for (sit = SessionList.begin(); sit != SessionList.end(); sit++) {
					handles.push_back((*sit)->GetHandle());
				}

				DWORD ret = WaitForMultipleObjects(
					handles.size(), &handles[0], FALSE, INFINITE);

				uint index = ret - WAIT_OBJECT_0;

				for (sit = SessionList.begin(); sit != SessionList.end(); sit++) {
					if ((*sit)->GetHandle() == handles[index])
						break;
				}

				O2SocketSession *ss = *sit;
				SessionList.erase(sit);

				nit = std::find(
					neighbors.begin(), neighbors.end(), ss->node);

				HTTPHeader *header = (HTTPHeader*)ss->data;
				if (nit == neighbors.end()) {
					TRACEW(L"############\n");
					TRACEW(L"### UNKNOWN \n");
					TRACEW(L"############\n");
				}
				else if (!CheckResponse(ss, header, NodeDB, *nit)) {
#if TRACE_REFINE
					TRACEW(L"###################\n");
					TRACEW(L"### RESPONSE ERROR \n");
					TRACEW(L"###################\n");
					//NodeDB->remove(*nit);
#endif
					neighbors.erase(nit);
				}
				else {
					// 返答を取り込む
					char *body = &ss->rbuff[header->length];
					uint len = header->contentlength;

					O2NodeDB::NodeListT ReceiveNodes;
					NodeDB->ImportFromXML(NULL, body, len, &ReceiveNodes);

					// neighborsへ追加
					for (nit = ReceiveNodes.begin(); nit != ReceiveNodes.end(); nit++) {
						if (nit->id == myID)
							continue;

						O2NodeDB::NodeListT::iterator nnit = std::find(
							neighbors.begin(), neighbors.end(), *nit);

						if (nnit == neighbors.end()) {
							neighbors.push_back(*nit);
						}
					}
					// targetに近い順にソート
					O2NodeDB::SortByDistancePred comparetor(fit->id);
					neighbors.sort(comparetor);

#if TRACE_REFINE
					TRACEW(L"###----------\n");
					TRACEW(L"### REFINE\n");
					TRACEW(L"###----------\n");
					for (nit = neighbors.begin(); nit != neighbors.end(); nit++)
						printnode(fit->id, *nit);
#endif
					// 発見？
					nit = std::find(neighbors.begin(), neighbors.end(), *fit);
					if (nit != neighbors.end() && nit->lastlink) {
						FriendDB->Update(*nit);
						found = true;
					}
				}

				if (header) delete header;
				delete ss;
			}

			// 停止した場合はssが残ってる可能性があるのでdelete
			O2SocketSessionPListIt sit;
			for (sit = SessionList.begin(); sit != SessionList.end(); sit++) {
				O2SocketSession *ss = *sit;
				ss->Wait();
				HTTPHeader *header = (HTTPHeader*)ss->data;
				if (header) delete header;
				delete (ss);
			}

			// 成功したノードをtouch
			for (nit = neighbors.begin(); nit != neighbors.end() && IsActive(); nit++) {
				if (nit->lastlink) {
					NodeDB->touch(*nit);
				}
			}
		}
	}


	void printnode(const hashT &target, const O2Node &node)
	{
#if defined (_DEBUG)
		wstring hex;
		node.id.to_string(hex);
		//hashBitsetT d = target.bits ^ node.id.bits;

		wchar_t tmp[1024];
		swprintf_s(tmp, 1024, L"[%c][%d] %s\n",
			node.lastlink ? L'*' : L' ', hash_xor_bitlength(target, node.id), hex.c_str());

		TRACEW(tmp);
#endif
	}
};
