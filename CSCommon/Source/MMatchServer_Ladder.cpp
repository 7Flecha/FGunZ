#include "stdafx.h"
#include "MMatchServer.h"
#include "MSharedCommandTable.h"
#include "MErrorTable.h"
#include "MBlobArray.h"
#include "MObject.h"
#include "MMatchObject.h"
#include "MMatchItem.h"
#include "MAgentObject.h"
#include "MMatchNotify.h"
#include "Msg.h"
#include "MMatchObjCache.h"
#include "MMatchStage.h"
#include "MMatchTransDataType.h"
#include "MMatchFormula.h"
#include "MMatchConfig.h"
#include "MCommandCommunicator.h"
#include "MMatchShop.h"
#include "MMatchTransDataType.h"
#include "MDebug.h"
#include "MMatchAuth.h"
#include "MMatchStatus.h"
#include "MAsyncDBJob.h"
#include "MLadderMgr.h"
#include "MTeamGameStrategy.h"




/////////////////////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////////////////////
int MMatchServer::ValidateChallengeLadderGame(MMatchObject** ppMemberObject, int nMemberCount)
{
	MBaseTeamGameStrategy* pTeamGameStrategy = MBaseTeamGameStrategy::GetInstance(MGetServerConfig()->GetServerMode());
	if (pTeamGameStrategy)
	{
		int nRet = pTeamGameStrategy->ValidateChallenge(ppMemberObject, nMemberCount);
		return nRet;
	};

	return MOK;
}

///////////////////////////////////////////////////////////////////////////
// LadderStage
bool MMatchServer::LadderJoin(const MUID& uidPlayer, const MUID& uidStage, MMatchTeam nTeam)
{
	MMatchObject* pObj = GetObject(uidPlayer);
	if (pObj == NULL) return false;

	if (pObj->GetStageUID() != MUID(0,0))
		StageLeave(pObj->GetUID());//, pObj->GetStageUID());

	MMatchStage* pStage = FindStage(uidStage);
	if (pStage == NULL) return false;

	// Custom: Clan war glitch fix
	if (!pObj->GetCharInfo() || (pObj->GetCharInfo() && pObj->GetCharInfo()->m_bIsLadderMatching == false))
		return false;

	pObj->GetCharInfo()->m_bIsLadderMatching = false;
	pObj->OnStageJoin();

	// Join
	pStage->AddObject(uidPlayer, pObj);
	pObj->SetStageUID(uidStage);
	pObj->SetStageState(MOSS_READY);
	pObj->SetLadderChallenging(false);

	// Custom: Ladder Rejoin
	if (pStage->CheckLadderList(pObj->GetCharInfo()->m_nCID))
	{
		MMatchObjectCacheBuilder CacheBuilder;
		CacheBuilder.AddObject(pObj);
		MCommand* pCmdCacheAdd = CacheBuilder.GetResultCmd(MATCHCACHEMODE_ADD, this);
		RouteToStage(pStage->GetUID(), pCmdCacheAdd);
	}
	else
		pStage->AddLadderList(pObj->GetCharInfo()->m_nCID);

	pObj->SetLadderStageUID(uidStage);
	pObj->SetLadderTeam(nTeam);

	pStage->PlayerTeam(uidPlayer, nTeam);
	pStage->PlayerState(uidPlayer, MOSS_READY);
	

	MCommand* pCmd = CreateCommand(MC_MATCH_LADDER_PREPARE, uidPlayer);
	pCmd->AddParameter(new MCmdParamUID(uidStage));
	pCmd->AddParameter(new MCmdParamInt(nTeam));
	Post(pCmd);

	return true;
}

void MMatchServer::LadderGameLaunch(MLadderGroup* pGroupA, MLadderGroup* pGroupB)
{
	if ((MGetServerConfig()->GetServerMode() != MSM_LADDER) && 
		(MGetServerConfig()->GetServerMode() != MSM_CLAN)) return;

	MUID uidStage = MUID(0,0);
	if (StageAdd(NULL, "LADDER_GAME", true, "", &uidStage) == false) {
		// Group 해체
		GetLadderMgr()->CancelChallenge(pGroupA->GetID(), "");
		GetLadderMgr()->CancelChallenge(pGroupB->GetID(), "");
		return;
	}
	MMatchStage* pStage = FindStage(uidStage);
	if (pStage == NULL) {
		// Group 해체
		GetLadderMgr()->CancelChallenge(pGroupA->GetID(), "");
		GetLadderMgr()->CancelChallenge(pGroupB->GetID(), "");
		return;
	}

	// A 그룹 입장
	for (list<MUID>::iterator i=pGroupA->GetPlayerListBegin(); i!= pGroupA->GetPlayerListEnd(); i++)
	{
		MUID uidPlayer = (*i);
		LadderJoin(uidPlayer, uidStage, MMT_RED);
	}
	// B 그룹 입장
	for (list<MUID>::iterator i=pGroupB->GetPlayerListBegin(); i!= pGroupB->GetPlayerListEnd(); i++)
	{
		MUID uidPlayer = (*i);
		LadderJoin(uidPlayer, uidStage, MMT_BLUE);
	}

	// Agent 준비
//	ReserveAgent(pStage);

	//////////////////////////////////////////////////////////////////////////////
	int nRandomMap = 0;
	// 클랜전은 Stage의 팀정보에 CLID까지 설정해야한다.
	MBaseTeamGameStrategy* pTeamGameStrategy = MBaseTeamGameStrategy::GetInstance(MGetServerConfig()->GetServerMode());
	if (pTeamGameStrategy)
	{
		nRandomMap = pTeamGameStrategy->GetRandomMap((int)pGroupA->GetPlayerCount());
	};


	MMATCH_GAMETYPE nGameType = MMATCH_GAMETYPE_DEATHMATCH_TEAM;

	// Game 설정
	pStage->SetStageType(MST_LADDER);
	pStage->ChangeRule(nGameType);

	// 클랜전은 Stage의 팀정보에 CLID까지 설정해야한다.
	if (pTeamGameStrategy)
	{
		MMatchLadderTeamInfo a_RedLadderTeamInfo, a_BlueLadderTeamInfo;
		pTeamGameStrategy->SetStageLadderInfo(&a_RedLadderTeamInfo, &a_BlueLadderTeamInfo, pGroupA, pGroupB);

		pStage->SetLadderTeam(&a_RedLadderTeamInfo, &a_BlueLadderTeamInfo);
	};

	MMatchStageSetting* pSetting = pStage->GetStageSetting();
	pSetting->SetMasterUID(MUID(0,0));
	pSetting->SetMapIndex(nRandomMap);
	pSetting->SetGameType(nGameType);
	// Custom: 5vs5 clan wars (Fix)
	pSetting->SetMaxPlayers(10);

	pSetting->SetLimitTime(3);	
	pSetting->SetRoundMax(99);		// 최대 99라운드까지 진행할 수 있다.
	

	MCommand* pCmd = CreateCmdResponseStageSetting(uidStage);
	RouteToStage(uidStage, pCmd);	// Stage Setting 전송


	// 디비에 로그를 남긴다.
	// test 맵등은 로그 남기지 않는다.
	if ( (MGetMapDescMgr()->MIsCorrectMap(nRandomMap)) && (MGetGameTypeMgr()->IsCorrectGameType(nGameType)) )
	{
		if (pStage->StartGame(MGetServerConfig()->IsUseResourceCRC32CacheCheck()) == true) {		// 게임시작
			// Send Launch Command
			ReserveAgent(pStage);

			/////////////////////////////////////////////////////////////////////////////////////////////
			// 클랜전은 ObjectCache를 따로 전송하지 않기 때문에 Stage가 완성되면 그때 전송해 준다.
			// 이 정보는 클라이언트들끼리 Peer의 정보를 받을 수 있지만 서버가 용청할 시점과
			//  클라이언트의 리스트 구성시점이 다를 수 있기 때문에 이때 전송을 해준다.
			// - by SungE.
			MMatchObjectCacheBuilder CacheBuilder;
			CacheBuilder.Reset();
			for (MUIDRefCache::iterator i=pStage->GetObjBegin(); i!=pStage->GetObjEnd(); i++) {
				MUID uidObj = (MUID)(*i).first;
				MMatchObject* pScanObj = (MMatchObject*)GetObject(uidObj);
				if (pScanObj) {
					CacheBuilder.AddObject(pScanObj);
				}
			}
			MCommand* pCmdCacheAdd = CacheBuilder.GetResultCmd(MATCHCACHEMODE_UPDATE, this);
			RouteToStage(pStage->GetUID(), pCmdCacheAdd);
			/////////////////////////////////////////////////////////////////////////////////////////////

			MCommand* pCmd = CreateCommand(MC_MATCH_LADDER_LAUNCH, MUID(0,0));
			pCmd->AddParameter(new MCmdParamUID(uidStage));
			pCmd->AddParameter(new MCmdParamStr( const_cast<char*>(pStage->GetMapName()) ));
			RouteToStage(uidStage, pCmd);

			// Ladder Log 남긴다.
		} else {
			// Group 해체
			GetLadderMgr()->CancelChallenge(pGroupA->GetID(), "");
			GetLadderMgr()->CancelChallenge(pGroupB->GetID(), "");
		}
	}
}


bool MMatchServer::IsLadderRequestUserInRequestClanMember( const MUID& uidRequestMember
														  , const MTD_LadderTeamMemberNode* pRequestMemberNode )
{
	// - by SungE 2007-10-11 
	// 해커가 MemberNamesBlob를 조작할 수 있기 때문에 요청자 자신의 이름을 저장하는 0번째 인덱스 노드를 
	//  커맨드 요청자와 같은지 검사해 줘야 한다. 클라이언트와 서버의 구조를 변경하지 않기 위해 추가 검사코드만 작성.
	// 이 코드는 클랜전요청과 관련된 클라이언트 코드가 변경되지 않는다는 전제조건에서 작성된 것임.
	// 0번 인덱스의 노드가 요청자이다.

	if( NULL == pRequestMemberNode )
		return false;

	MMatchObject* pRequestMemberObj = GetPlayerByName( pRequestMemberNode->szName );
	if( NULL == pRequestMemberObj )
		return false;

	// 요청자의 캐릭터와 MemberNameNode의 0번째 인덱스 유저의 UID가 같은지 검사한다.

	if( uidRequestMember != pRequestMemberObj->GetUID() )
		return false; ///< 같지 않으면 비정상 유저의 요청으로 판단.s

	return true;
}

void MMatchServer::OnLadderRequestChallenge(const MUID& uidRequestMember, void* pMemberNamesBlob, unsigned long int nOptions)
{
	if ((MGetServerConfig()->GetServerMode() != MSM_LADDER) && 
		(MGetServerConfig()->GetServerMode() != MSM_CLAN)) return;

	MMatchObject* pLeaderObject = GetPlayerByCommUID(uidRequestMember);
	if (! IsEnabledObject(pLeaderObject)) return;

	if (!MGetServerConfig()->IsEnabledCreateLadderGame())
	{
		RouteResponseToListener(pLeaderObject, MC_MATCH_LADDER_RESPONSE_CHALLENGE, MERR_LADDER_NOT_SERVICE_TIME);
		return;
	}

	// Custom: CW Option
	MMatchClan* pClan = FindClan( pLeaderObject->GetCharInfo()->m_ClanInfo.m_nClanID );

	if( pClan )
	{
		if ( pClan->GetCWOption() == 255 )
		{
			// uninitialized
			pClan->InitClanInfoFromDB();

			// log if its still uninitialized...
			if ( pClan->GetCWOption() == 255 )
			{
				LOG(LOG_FILE, "CWOption query fail - still couldn't initialize! (ClanID: %d)", pClan->GetCLID());
			}
		}

		// disabled
		if( (pClan->GetCWOption() == 1 && pLeaderObject->GetCharInfo()->m_ClanInfo.m_nGrade != MCG_MASTER) )
		{
			RouteResponseToListener(pLeaderObject, MC_MATCH_LADDER_RESPONSE_CHALLENGE, MERR_LADDER_CANNOT_CHALLENGE);
			Announce( pLeaderObject, "Clan War Invitations are currently disabled by the clan master." );
			return;
		}
		else if( (pClan->GetCWOption() == 2 && pLeaderObject->GetCharInfo()->m_ClanInfo.m_nGrade == MCG_MEMBER) )
		{
			RouteResponseToListener(pLeaderObject, MC_MATCH_LADDER_RESPONSE_CHALLENGE, MERR_LADDER_CANNOT_CHALLENGE);
			Announce( pLeaderObject, "Clan War Invitations are currently disabled for non-admins by the clan master." );
			return;
		} 
	}

	int nBlobCount = MGetBlobArrayCount(pMemberNamesBlob);
	int nMemberCount = nBlobCount;
	if (nMemberCount <= 0) return;

	if( !IsLadderRequestUserInRequestClanMember(uidRequestMember
		, (MTD_LadderTeamMemberNode*)MGetBlobArrayElement(pMemberNamesBlob, 0)) )
		return;
	
	MMatchObject* pMemberObjects[MAX_CLANBATTLE_TEAM_MEMBER];
	for (int i = 0; i < nMemberCount; i++)
	{
		MTD_LadderTeamMemberNode* pNode = (MTD_LadderTeamMemberNode*)MGetBlobArrayElement(pMemberNamesBlob, i);
		if (pNode == NULL) break;
		if ((strlen(pNode->szName) <= 0) || (strlen(pNode->szName) >= MATCHOBJECT_NAME_LENGTH)) return;

		pMemberObjects[i] = GetPlayerByName(pNode->szName);

		// 한명이라도 존재하지 않으면 안된다
		if (! IsEnabledObject(pMemberObjects[i]))
		{
			// 메세지 보내주고 끝.
			RouteResponseToListener(pLeaderObject, MC_MATCH_LADDER_RESPONSE_CHALLENGE, MERR_LADDER_CANNOT_CHALLENGE);
			return;
		}
	}


	int nRet = ValidateChallengeLadderGame(pMemberObjects, nMemberCount);
	if (nRet != MOK)
	{
		RouteResponseToListener(pLeaderObject, MC_MATCH_LADDER_RESPONSE_CHALLENGE, nRet);
		return;
	}

	// Custom: Disallow snipers from clan war
	for (int i = 0; i < nMemberCount; ++i)
	{
		MMatchItem* pPrimary = pMemberObjects[i]->GetCharInfo()->m_EquipedItem.GetItem(MMCIP_PRIMARY);
		MMatchItem* pSecondary = pMemberObjects[i]->GetCharInfo()->m_EquipedItem.GetItem(MMCIP_SECONDARY);
		MMatchItem* pCustom1 = pMemberObjects[i]->GetCharInfo()->m_EquipedItem.GetItem(MMCIP_CUSTOM1);
		MMatchItem* pCustom2 = pMemberObjects[i]->GetCharInfo()->m_EquipedItem.GetItem(MMCIP_CUSTOM2);

		if (pPrimary && pPrimary->GetDesc())
		{
			if (pPrimary->GetDesc()->m_nWeaponType.Ref() == MWT_SNIFER)
			{
				RouteResponseToListener(pLeaderObject, MC_MATCH_LADDER_RESPONSE_CHALLENGE, MERR_LADDER_CANNOT_CHALLENGE);
				Announce( pLeaderObject, "One or more members invited to clan war has Sniper(s) equipped." );
				return;
			}
		}

		if (pSecondary && pSecondary->GetDesc())
		{
			if (pSecondary->GetDesc()->m_nWeaponType.Ref() == MWT_SNIFER)
			{
				RouteResponseToListener(pLeaderObject, MC_MATCH_LADDER_RESPONSE_CHALLENGE, MERR_LADDER_CANNOT_CHALLENGE);
				Announce( pLeaderObject, "One or more members invited to clan war has Sniper(s) equipped." );
				return;
			}
		}

		if (pCustom1 && pCustom1->GetDesc())
		{
			// Ban landmines from cw
			if (pCustom1->GetDesc()->m_nID >= 1647 && pCustom1->GetDesc()->m_nID <= 1651)
			{
				RouteResponseToListener(pLeaderObject, MC_MATCH_LADDER_RESPONSE_CHALLENGE, MERR_LADDER_CANNOT_CHALLENGE);
				Announce( pLeaderObject, "One or more members invited to clan war has Landmines equipped." );
				return;
			}
		}

		if (pCustom2 && pCustom2->GetDesc())
		{
			// Ban landmines from cw
			if (pCustom2->GetDesc()->m_nID >= 1647 && pCustom2->GetDesc()->m_nID <= 1651)
			{
				RouteResponseToListener(pLeaderObject, MC_MATCH_LADDER_RESPONSE_CHALLENGE, MERR_LADDER_CANNOT_CHALLENGE);
				Announce( pLeaderObject, "One or more members invited to clan war has Landmines equipped." );
				return;
			}
		}
	}

	int nTeamID = 0;

	MBaseTeamGameStrategy* pTeamGameStrategy = NULL;

	pTeamGameStrategy = MBaseTeamGameStrategy::GetInstance(MGetServerConfig()->GetServerMode());
	if (pTeamGameStrategy)
	{
        nTeamID = pTeamGameStrategy->GetNewGroupID(pLeaderObject, pMemberObjects, nMemberCount);
	}
	if (nTeamID == 0) return;

	// 실제로 Challenge한다.
	// Ensure All Player Not in LadderGroup
	MLadderGroup* pGroup = GetLadderMgr()->CreateLadderGroup();
	pGroup->SetID(nTeamID);

	// balancedMatching 설정
	if (nOptions == 1)
	{
		pGroup->SetBalancedMatching(true);
	}
	else
	{
		pGroup->SetBalancedMatching(false);	
	}

	if (pTeamGameStrategy)
	{
		pTeamGameStrategy->SetLadderGroup(pGroup, pMemberObjects, nMemberCount);
	}

	for (int i=0; i<nMemberCount; i++) {
		pGroup->AddPlayer( pMemberObjects[i] );

		if( pMemberObjects[i]->GetCharInfo() )
			pMemberObjects[i]->GetCharInfo()->m_bIsLadderMatching = true;
	}

	GetLadderMgr()->Challenge(pGroup);
}

void MMatchServer::OnLadderRequestCancelChallenge(const MUID& uidPlayer)
{
	MMatchObject* pObj = GetObject(uidPlayer);
	if (!IsEnabledObject(pObj)) return;
	if (pObj->GetLadderGroupID() == 0) return;

	GetLadderMgr()->CancelChallenge(pObj->GetLadderGroupID(), pObj->GetCharInfo()->m_szName);
}

void MMatchServer::OnRequestProposal(const MUID& uidProposer, const int nProposalMode, const int nRequestID, 
		                const int nReplierCount, void* pReplierNamesBlob)
{
	MMatchObject* pProposerObject = GetObject(uidProposer);
	if (! IsEnabledObject(pProposerObject)) return;


	if ((nReplierCount > MAX_REPLIER) || (nReplierCount < 0))
	{
		_ASSERT(0);	// 16명이상 동의할 수 없음
		return;
	}


	if (!MGetServerConfig()->IsEnabledCreateLadderGame())
	{
		// 메세지 보내주고 끝.
		MCommand* pNewCmd = CreateCommand(MC_MATCH_RESPONSE_PROPOSAL, MUID(0,0));
		pNewCmd->AddParameter(new MCommandParameterInt(MERR_LADDER_NOT_SERVICE_TIME));
		pNewCmd->AddParameter(new MCommandParameterInt(nProposalMode));
		pNewCmd->AddParameter(new MCommandParameterInt(nRequestID));
		RouteToListener(pProposerObject, pNewCmd);
		return;
	}


	int nBlobCount = MGetBlobArrayCount(pReplierNamesBlob);
	if (nBlobCount != nReplierCount) return;

	MMatchObject* ppReplierObjects[MAX_REPLIER];

	for (int i = 0; i < nReplierCount; i++)
	{
		MTD_ReplierNode* pNode = (MTD_ReplierNode*)MGetBlobArrayElement(pReplierNamesBlob, i);
		if (pNode == NULL) return;
		if ((strlen(pNode->szName) <= 0) || (strlen(pNode->szName) >= MATCHOBJECT_NAME_LENGTH)) return;

		ppReplierObjects[i] = GetPlayerByName(pNode->szName);

		// 답변자가 한명이라도 존재하지 않으면 안된다
		if (!IsEnabledObject(ppReplierObjects[i]))
		{
			// 메세지 보내주고 끝.
			MCommand* pNewCmd = CreateCommand(MC_MATCH_RESPONSE_PROPOSAL, MUID(0,0));
			pNewCmd->AddParameter(new MCommandParameterInt(MERR_NO_TARGET));
			pNewCmd->AddParameter(new MCommandParameterInt(nProposalMode));
			pNewCmd->AddParameter(new MCommandParameterInt(nRequestID));
			RouteToListener(pProposerObject, pNewCmd);

			return;
		}
	}

	int nRet = MERR_UNKNOWN;
	// 상황에 맞게 validate 한다.

	switch (nProposalMode)
	{
	case MPROPOSAL_LADDER_INVITE:
		{
			MLadderGameStrategy* pLadderGameStrategy = MLadderGameStrategy::GetInstance();
			nRet = pLadderGameStrategy->ValidateRequestInviteProposal(pProposerObject, ppReplierObjects, nReplierCount);
		}
		break;
	case MPROPOSAL_CLAN_INVITE:
		{
			MClanGameStrategy* pClanGameStrategy = MClanGameStrategy::GetInstance();
			nRet = pClanGameStrategy->ValidateRequestInviteProposal(pProposerObject, ppReplierObjects, nReplierCount);
		}
		break;
	};

	if (nRet != MOK)
	{
		MCommand* pNewCmd = CreateCommand(MC_MATCH_RESPONSE_PROPOSAL, MUID(0,0));
		pNewCmd->AddParameter(new MCommandParameterInt(nRet));
		pNewCmd->AddParameter(new MCommandParameterInt(nProposalMode));
		pNewCmd->AddParameter(new MCommandParameterInt(nRequestID));
		RouteToListener(pProposerObject, pNewCmd);
		return;
	}


	int nMemberCount = nReplierCount+1;		// 제안자까지 
	void* pBlobMembersNameArray = MMakeBlobArray(sizeof(MTD_ReplierNode), nMemberCount);

	MTD_ReplierNode* pProposerNode = (MTD_ReplierNode*)MGetBlobArrayElement(pBlobMembersNameArray, 0);
	strcpy(pProposerNode->szName, pProposerObject->GetCharInfo()->m_szName);

	for (int k = 0; k < nReplierCount; k++)
	{
		MTD_ReplierNode* pMemberNode = (MTD_ReplierNode*)MGetBlobArrayElement(pBlobMembersNameArray, k+1);
		strcpy(pMemberNode->szName, ppReplierObjects[k]->GetCharInfo()->m_szName);
	}

	// 답변자에게 동의를 물어본다.
	for (int i = 0; i < nReplierCount; i++)
	{
		MCommand* pNewCmd = CreateCommand(MC_MATCH_ASK_AGREEMENT, MUID(0,0));
		pNewCmd->AddParameter(new MCommandParameterUID(uidProposer));
//		pNewCmd->AddParameter(new MCommandParameterString(pProposerObject->GetCharInfo()->m_szName));
		pNewCmd->AddParameter(new MCommandParameterBlob(pBlobMembersNameArray, MGetBlobArraySize(pBlobMembersNameArray)));

		pNewCmd->AddParameter(new MCommandParameterInt(nProposalMode));
		pNewCmd->AddParameter(new MCommandParameterInt(nRequestID));
		RouteToListener(ppReplierObjects[i], pNewCmd);


	}
	MEraseBlobArray(pBlobMembersNameArray);


	// 제안자에게 응답 보내줌
	MCommand* pNewCmd = CreateCommand(MC_MATCH_RESPONSE_PROPOSAL, MUID(0,0));
	pNewCmd->AddParameter(new MCommandParameterInt(nRet));
	pNewCmd->AddParameter(new MCommandParameterInt(nProposalMode));
	pNewCmd->AddParameter(new MCommandParameterInt(nRequestID));
	RouteToListener(pProposerObject, pNewCmd);

}

void MMatchServer::OnReplyAgreement(MUID& uidProposer, MUID& uidReplier, const char* szReplierName, 
		                const int nProposalMode, const int nRequestID, const bool bAgreement)
{
	MMatchObject* pProposerObject = GetObject(uidProposer);
	if (! IsEnabledObject(pProposerObject)) return;

	
	MCommand* pNewCmd = CreateCommand(MC_MATCH_REPLY_AGREEMENT, MUID(0,0));
	pNewCmd->AddParameter(new MCommandParameterUID(uidProposer));
	pNewCmd->AddParameter(new MCommandParameterUID(uidReplier));
	pNewCmd->AddParameter(new MCommandParameterString(szReplierName));
	pNewCmd->AddParameter(new MCommandParameterInt(nProposalMode));
	pNewCmd->AddParameter(new MCommandParameterInt(nRequestID));
	pNewCmd->AddParameter(new MCommandParameterBool(bAgreement));

	RouteToListener(pProposerObject, pNewCmd);	
}

void MMatchServer::OnLadderRejoin(const MUID& uidComm, bool bAcknowledged)
{
	MMatchObject* pObj = GetObject(uidComm);
	if (!IsEnabledObject(pObj))
		return;

	if (!pObj->GetCharInfo()->m_ClanInfo.IsJoined())
		return;

	if (pObj->GetStageUID().IsValid())
		return;

	MMatchChannel* pChannel = FindChannel(pObj->GetChannelUID());
	if (!pChannel || pChannel && pChannel->GetChannelType() != MCHANNEL_TYPE_CLAN)
		return;

	bool bJustFound = false;
	if (pObj->GetLadderStageUID() == MUID(0,0) || pObj->GetLadderTeam() == MMT_ALL)
	{
		// did the user relogin?
		bool bFound = false;

		MMatchClan* pClan = FindClan(pObj->GetCharInfo()->m_ClanInfo.m_nClanID);

		if (!pClan)
			return;

		// Don't bother checking
		if (pClan->GetMemberCount() <= 1)
			return;

		for (MUIDRefCache::iterator i=pClan->GetMemberBegin(); i!=pClan->GetMemberEnd(); i++)
		{
			MMatchObject* pClanObj = (MMatchObject*)(*i).second;
			if (!IsEnabledObject(pClanObj))
				continue;

			if (pClanObj->GetUID() == pObj->GetUID() || pClanObj->GetStageUID() == MUID(0,0))
				continue;

			MMatchStage* pClanStage = FindStage(pClanObj->GetStageUID());
			if (!pClanStage)
				continue;

			// check if it's ladder
			if (pClanStage->GetStageType() == MST_LADDER)
			{
				// is this the stage we left?
				if (!pClanStage->CheckLadderList(pObj->GetCharInfo()->m_nCID))
					continue;

				if (pClanStage->GetRule() && pClanStage->GetRule()->GetRoundState() == MMATCH_ROUNDSTATE_EXIT
					|| pClanStage->GetState() == STAGE_STATE_CLOSE)
					continue;

				// check if the game is balanced before reconnecting
				int red = 0;
				int blue = 0;
				pClanStage->GetTeamMemberCount(&red, &blue, NULL, true);

				if (pClanObj->GetTeam() == MMT_RED && red < blue || pClanObj->GetTeam() == MMT_BLUE && blue < red)
				{
					bFound = true;

					if (pClanStage->GetRule()->GetRoundState() == MMATCH_ROUNDSTATE_FINISH)
					{
						if (bAcknowledged)
							Announce(pObj, "Please try again.");
						return;
					}

					pObj->SetLadderStageUID(pClanObj->GetStageUID());
					pObj->SetLadderTeam(pClanObj->GetTeam());
					bJustFound = true;
					break;
				}
			}
		}

		if (!bFound)
		{
			if (bAcknowledged)
				Announce(pObj, "Cannot rejoin clan war match. No existing match found.");
			return;
		}
	}

	MMatchStage* pStage = FindStage(pObj->GetLadderStageUID());
	if (!pStage)
	{
		pObj->SetLadderStageUID(MUID(0,0));
		pObj->SetLadderTeam(MMT_ALL);

		if (bAcknowledged)
			Announce(pObj, "Unable to locate existing clan war match.");

		return;
	}

	if (!bJustFound)
	{
		if (pStage->GetState() == STAGE_STATE_CLOSE)
		{
			if (bAcknowledged)
				Announce(pObj, "Cannot rejoin clan war match. The match has ended.");

			return;
		}

		if (pStage->GetRule() && pStage->GetRule()->GetRoundState() == MMATCH_ROUNDSTATE_FINISH)
		{
			if (bAcknowledged)
				Announce(pObj, "Please try again.");

			return;
		}

		if (pStage->GetRule() && pStage->GetRule()->GetRoundState() == MMATCH_ROUNDSTATE_EXIT)
		{
			if (bAcknowledged)
				Announce(pObj, "Cannot rejoin clan war match. The match is ending.");

			return;
		}

		// check if the game is balanced before reconnecting
		int red = 0;
		int blue = 0;
		pStage->GetTeamMemberCount(&red, &blue, NULL, true);

		if (pObj->GetTeam() == MMT_RED && red >= blue || pObj->GetTeam() == MMT_BLUE && blue >= red)
		{
			if (bAcknowledged)
				Announce(pObj, "Cannot rejoin clan war match. The game is already balanced.");

			return;
		}
	}

	if (bAcknowledged)
	{
		MMatchItem* pPrimary = pObj->GetCharInfo()->m_EquipedItem.GetItem(MMCIP_PRIMARY);
		MMatchItem* pSecondary = pObj->GetCharInfo()->m_EquipedItem.GetItem(MMCIP_SECONDARY);
		MMatchItem* pCustom1 = pObj->GetCharInfo()->m_EquipedItem.GetItem(MMCIP_CUSTOM1);
		MMatchItem* pCustom2 = pObj->GetCharInfo()->m_EquipedItem.GetItem(MMCIP_CUSTOM2);

		if (pPrimary && pPrimary->GetDesc())
		{
			if (pPrimary->GetDesc()->m_nWeaponType.Ref() == MWT_SNIFER)
			{
				Announce( pObj, "Cannot clan war with Sniper(s) equipped." );
				return;
			}
		}

		if (pSecondary && pSecondary->GetDesc())
		{
			if (pSecondary->GetDesc()->m_nWeaponType.Ref() == MWT_SNIFER)
			{
				Announce( pObj, "Cannot clan war with Sniper(s) equipped." );
				return;
			}
		}

		if (pCustom1 && pCustom1->GetDesc())
		{
			// Ban landmines from cw
			if (pCustom1->GetDesc()->m_nID >= 1647 && pCustom1->GetDesc()->m_nID <= 1651)
			{
				Announce( pObj, "Cannot clan war with Landmines equipped." );
				return;
			}
		}

		if (pCustom2 && pCustom2->GetDesc())
		{
			// Ban landmines from cw
			if (pCustom2->GetDesc()->m_nID >= 1647 && pCustom2->GetDesc()->m_nID <= 1651)
			{
				Announce( pObj, "Cannot clan war with Landmines equipped." );
				return;
			}
		}

		pObj->GetCharInfo()->m_bIsLadderMatching = true;
		LadderJoin(uidComm, pObj->GetLadderStageUID(), pObj->GetLadderTeam());

		MMatchObjectCacheBuilder CacheBuilder;
		CacheBuilder.Reset();
		for (MUIDRefCache::iterator i=pStage->GetObjBegin(); i!=pStage->GetObjEnd(); i++) {
			MUID uidObj = (MUID)(*i).first;
			MMatchObject* pScanObj = (MMatchObject*)GetObject(uidObj);
			if (pScanObj) {
				CacheBuilder.AddObject(pScanObj);
			}
		}
		MCommand* pCmdCacheAdd = CacheBuilder.GetResultCmd(MATCHCACHEMODE_UPDATE, this);
		RouteToStage(pStage->GetUID(), pCmdCacheAdd);

		MCommand* pCmd = CreateCmdResponseStageSetting(pObj->GetLadderStageUID());
		RouteToListener(pObj, pCmd);	// Stage Setting

		pObj->SetForcedEntry(true);
	
		pCmd = CreateCommand(MC_MATCH_LADDER_LAUNCH, uidComm);
		pCmd->AddParameter(new MCmdParamUID(pObj->GetLadderStageUID()));
		pCmd->AddParameter(new MCmdParamStr( const_cast<char*>(pStage->GetMapName()) ));
		RouteToListener(pObj, pCmd);
	}
	else
	{
		// notify the user
		MCommand* pNew = CreateCommand(MC_MATCH_NOTIFY_LADDER_REJOIN, uidComm);
		RouteToListener(pObj, pNew);
	}
}
