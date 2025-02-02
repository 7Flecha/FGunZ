#ifndef _ZMYITEM_H
#define _ZMYITEM_H


#include <map>
using std::map;

class ZMyItemNode : public MBaseItem
{
protected:
	unsigned long int		m_nItemID;
	MUID					m_UID;
	DWORD					m_dwWhenReceivedClock;
	int						m_nRarity;
	int						m_niLevel;
	
public:
	
	ZMyItemNode() : MBaseItem(), m_nItemID(0), m_UID(MUID(0,0)) { }
	virtual	~ZMyItemNode() { }

	void Create(MUID& uidItem, unsigned long int nItemID, int nCount=1, 
				bool bIsRentItem=false, int nRentMinutePeriodRemainder=RENT_MINUTE_PERIOD_UNLIMITED, int iMaxUseHour = 0, int nRarity = 0, int niLevel = 0)	// Update sgk 0614 [int iMaxUseHour = 0] �߰�
	{
		m_UID = uidItem;
		m_nItemID = nItemID;
		m_bIsRentItem = bIsRentItem;
		m_nRentMinutePeriodRemainder = nRentMinutePeriodRemainder;
		m_wRentHourPeriod = iMaxUseHour;
		m_dwWhenReceivedClock = timeGetTime();
		m_nRarity = nRarity;
		m_niLevel = niLevel;
		m_nCount = nCount;
	}
	void Create(unsigned long int nItemID, int nCount=1, 
				bool bIsRentItem=false, int nRentMinutePeriodRemainder=RENT_MINUTE_PERIOD_UNLIMITED, int iMaxUseHour = 0, int nRarity = 0, int niLevel = 0)	// Update sgk 0614 [int iMaxUseHour = 0] �߰�
	{
		m_nItemID = nItemID;
		m_bIsRentItem = bIsRentItem;
		m_nRentMinutePeriodRemainder = nRentMinutePeriodRemainder;
		m_wRentHourPeriod = iMaxUseHour;
		m_dwWhenReceivedClock = timeGetTime();
		m_nRarity = nRarity;
		m_niLevel = niLevel;
		m_nCount = nCount;
	}
	DWORD GetWhenReceivedClock()
	{
		return m_dwWhenReceivedClock;
	}

	unsigned long int GetItemID()	{ return m_nItemID; }
	unsigned long int GetItemRarity()	{ return m_nRarity; }
	unsigned long int GetItemLevel()	{ return m_niLevel; }
	MUID& GetUID()					{ return m_UID; }
};

typedef map<MUID, ZMyItemNode*> MITEMNODEMAP;
typedef map<int, ZMyItemNode*> MACCOUNT_ITEMNODEMAP;




#endif