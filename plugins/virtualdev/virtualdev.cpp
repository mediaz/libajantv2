/**
	@file		VirtualDev.cpp
	@brief		A configurable ntv2 virtual device, originally created to be configured by Control Panel 2
	@copyright	(C) 2022-2023 AJA Video Systems, Inc.	Proprietary and confidential information.
**/
#include "ntv2card.h"
#include "ntv2devicescanner.h"
#include "ntv2nubaccess.h"
#include "ntv2publicinterface.h"
#include "ntv2utils.h"
#include "ntv2version.h"
#include "ntv2registerexpert.h"
#include "ajabase/system/debug.h"
#include "ajabase/common/common.h"
#include "ajabase/common/json.hpp"
#include <fstream>
#include <iomanip>
#if defined(AJAMac)
	#include <CoreFoundation/CoreFoundation.h>
	#include <dlfcn.h>
#elif defined(AJALinux)
	#include <dlfcn.h>
#endif

using json = nlohmann::json;


using namespace std;

#define INSTP(_p_)			xHEX0N(uint64_t(_p_),16)
#define	NBFAIL(__x__)		AJA_sERROR  (AJA_DebugUnit_RPCClient, INSTP(this) << "::" << AJAFUNC << ": " << __x__)
#define	NBWARN(__x__)		AJA_sWARNING(AJA_DebugUnit_RPCClient, INSTP(this) << "::" << AJAFUNC << ": " << __x__)
#define	NBNOTE(__x__)		AJA_sNOTICE (AJA_DebugUnit_RPCClient, INSTP(this) << "::" << AJAFUNC << ": " << __x__)
#define	NBINFO(__x__)		AJA_sINFO   (AJA_DebugUnit_RPCClient, INSTP(this) << "::" << AJAFUNC << ": " << __x__)
#define	NBDBG(__x__)		AJA_sDEBUG  (AJA_DebugUnit_RPCClient, INSTP(this) << "::" << AJAFUNC << ": " << __x__)

#define	AsNTV2GetRegisters(_p_)			(reinterpret_cast<NTV2GetRegisters*>(_p_))
#define	AsNTV2SetRegisters(_p_)			(reinterpret_cast<NTV2SetRegisters*>(_p_))

#if defined(MSWindows)
	#define EXPORT __declspec(dllexport)	
#else
	#define EXPORT	
#endif

typedef map<NTV2WidgetID,NTV2WidgetID>			WgtMap;
typedef pair<NTV2WidgetID,NTV2WidgetID>			WgtPair;
typedef WgtMap::const_iterator					WgtMapCIter;

typedef map<NTV2InputXptID,NTV2InputXptID>		InXptMap;
typedef pair<NTV2InputXptID,NTV2InputXptID>		InXptPair;
typedef InXptMap::const_iterator				InXptMapCIter;

typedef map<NTV2OutputXptID,NTV2OutputXptID>	OutXptMap;
typedef pair<NTV2OutputXptID,NTV2OutputXptID>	OutXptPair;
typedef OutXptMap::const_iterator				OutXptMapCIter;

class RegInfo
{
	public:
		RegInfo (const ULWord regNum = 0, const ULWord ndx = 0, const NTV2InputXptID iXpt = NTV2_INPUT_CROSSPOINT_INVALID)
			:	mRegNum(regNum), mNdx(ndx), mIXpt(iXpt)					{}
		inline ULWord			regNum (void) const						{return mRegNum;}
		inline ULWord			mask (void) const						{return MaskForNdx(mNdx);}
		inline ULWord			invMask (void) const					{return InvertedMaskForNdx(mNdx);}
		inline UByte			shift (void) const						{return ShiftForNdx(mNdx);}
		inline NTV2InputXptID	inputXpt (void) const					{return mIXpt;}
		inline bool				isValid (void) const					{return regNum() != 0;}
		inline RegInfo &		makeInvalid (void)						{mRegNum = 0; mNdx = 0; mIXpt = NTV2_INPUT_CROSSPOINT_INVALID; return *this;}
		inline RegInfo &		setRegNum (const ULWord regNum)			{mRegNum = regNum; return *this;}
		inline RegInfo &		setMaskShiftIndex (const ULWord ndx)	{mNdx = ndx; return *this;}
		inline RegInfo &		setInputXpt (const NTV2InputXptID xpt)	{mIXpt = xpt; return *this;}
		inline bool operator == (const RegInfo & rhs) const			{return serialnum() == rhs.serialnum();}
		inline bool operator < (const RegInfo & rhs) const			{return serialnum() < rhs.serialnum();}
		inline ostream &	print (ostream & oss) const
		{	oss << CNTV2RegisterExpert::GetDisplayName(regNum()) << " (" << DEC(regNum()) << ")"
				<< " & " << xHEX0N(mask(),8) << " >> " << DEC(UWord(shift()))
				<< " " << ::NTV2InputCrosspointIDToString(inputXpt());
			return oss;
		}
	protected:
		static inline ULWord MaskForNdx (const UByte ndx)
		{	static const ULWord sMasks[4] = {0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000};
			return ndx < 4 ? sMasks[ndx] : 0;
		}
		static inline ULWord InvertedMaskForNdx (const UByte ndx)
		{	static const ULWord sInvMasks[4] = {0xFFFFFF00, 0xFFFF00FF, 0xFF00FFFF, 0x00FFFFFF};
			return ndx < 4 ? sInvMasks[ndx] : 0;
		}
		static inline ULWord ShiftForNdx (const UByte ndx)
		{	static const ULWord sShifts[4] = {0, 8, 16, 24};
			return ndx < 4 ? sShifts[ndx] : 0;
		}
		inline ULWord64	serialnum (void) const	{return ULWord64(regNum()) << 40 | ULWord64(mask());}
	private:
		ULWord			mRegNum;
		ULWord			mNdx;
		NTV2InputXptID	mIXpt;
};	//	RegInfo

inline ostream & operator << (ostream & oss, const RegInfo & obj)	{return obj.print(oss);}

typedef multimap<ULWord,RegInfo>				XptRegInfoMMap;
typedef pair<ULWord,RegInfo>					XptRegInfoPair;
typedef XptRegInfoMMap::iterator				XptRegMMapIter;
typedef XptRegInfoMMap::const_iterator			XptRegMMapCIter;
typedef map<NTV2Crosspoint, NTV2Crosspoint>		ACXptMap;
typedef ACXptMap::const_iterator				ACXptMapCIter;
typedef map<NTV2Channel, NTV2Channel>			ChannelMap;
typedef ChannelMap::const_iterator				ChannelMapCIter;
typedef map<NTV2AudioSystem, NTV2AudioSystem>	AudSysMap;
typedef AudSysMap::const_iterator				AudSysMapCIter;
typedef map<ULWord, ULWord>						DATMap;
typedef DATMap::const_iterator					DATMapCIter;

/*****************************************************************************************************************************************************
	NTV2Virtual Device

	An NTV2 virtual device that is configurable.
	Designed to be configured with a virtual device and widget map generated from Control Panel 2

	CONFIGURATION PARAMETERS
		Parameter Name			Required?	Description
		------------------		----------	---------------------------------------------------------------------------
		CP2ConfigPath			Yes			Device specifier that identifies the underlying device to connect to.
											Must be a device that has at least 2 FrameStores/Channels.
		DeviceSN				Yes			Used to find the relevant configuration in the cp2 config file.
											Also used as the "devspec" to open the physical device
		VDID					Yes			The UUID for the virtual device which is being configured
		help					No			Displays parameter help to stdout/stderr.
		verbose					No			Displays mapping information to stderr.

	EXAMPLE USAGE:
		To use this virtual device in the NTV2Player demo on MacOS as a proxy to channels 3 & 4 on the first Corvid88 board on the host:
			./bin/ntv2player  --device 'ntv2kona1://localhost/?devspec=corvid88&channel=2'
		...or in C++:
			CNTV2Card device;
			device.Open("ntv2kona1://localhost/?devspec=corvid88&channel=3");

		After URL-decoding, and viewed as key/value pairs, here's the software device config params:
			PARM				VALUE
			devspec				corvid88
			channel				3
*****************************************************************************************************************************************************/

class NTV2VirtualDev : public NTV2RPCAPI
{
	//	Instance Methods
	public:
									NTV2VirtualDev (void * pInDLLHandle, const NTV2ConnectParams & inParams, const uint32_t inCallingVersion);
		virtual						~NTV2VirtualDev ();
		virtual string				Name						(void) const;
		virtual string				Description					(void) const;
		virtual inline bool			IsConnected					(void) const	{return mCard.IsOpen();}
		virtual bool				NTV2Connect					(void);
		virtual	bool				NTV2Disconnect				(void);
		virtual inline bool			NTV2GetBoolParamRemote		(const ULWord inParamID,  ULWord & outValue)		{return false;}
		virtual inline bool			NTV2GetNumericParamRemote	(const ULWord inParamID,  ULWord & outValue)		{return false;}
		virtual inline bool			NTV2GetSupportedRemote		(const ULWord inEnumsID, ULWordSet & outSupported)	{return false;}
		virtual	bool				NTV2ReadRegisterRemote		(const ULWord regNum, ULWord & outRegValue, const ULWord regMask = 0xFFFFFFFF, const ULWord regShift = 0);
		virtual	bool				NTV2WriteRegisterRemote		(const ULWord regNum, const ULWord regValue, const ULWord regMask = 0xFFFFFFFF, const ULWord regShift = 0);
		virtual	bool				NTV2AutoCirculateRemote		(AUTOCIRCULATE_DATA & autoCircData);
		virtual	bool				NTV2WaitForInterruptRemote	(const INTERRUPT_ENUMS eInterrupt, const ULWord timeOutMs);
		virtual	bool				NTV2DMATransferRemote		(const NTV2DMAEngine inDMAEngine,	const bool inIsRead,
																const ULWord inFrameNumber,			NTV2Buffer & inOutBuffer,
																const ULWord inCardOffsetBytes,		const ULWord inNumSegments,
																const ULWord inSegmentHostPitch,	const ULWord inSegmentCardPitch,
																const bool inSynchronous);
		virtual	bool				NTV2MessageRemote			(NTV2_HEADER * pInMessage);
		virtual inline string		getParam					(const string & inKey)			{return mConnectParams.valueForKey(inKey);}
		virtual inline bool			hasParam					(const string & inKey) const	{return mConnectParams.hasKey(inKey);}

	//	Protected & Private Instance Methods
	protected:
		virtual	bool				NTV2OpenRemote				(void);
		virtual	inline bool			NTV2CloseRemote				(void)							{return true;}
		virtual bool 				ImportVDFromJSON			(void);
		virtual bool				SetupMapping				(void);
		virtual bool				SetupWidgetMapping			(void);
		virtual bool				SetupInputXptMapping		(void);
		virtual bool				SetupOutputXptMapping		(void);
		virtual bool				SetupXptSelectRegMapping	(void);
		virtual NTV2Channel			VDevToCardChannel (const NTV2Channel ch) const;
		virtual NTV2Channel			CardToVDevChannel (const NTV2Channel ch) const;
		virtual NTV2Channel			VDevToCardMixer (const NTV2Channel ch) const;
		virtual NTV2Channel			CardToVDevMixer (const NTV2Channel ch) const;
		virtual NTV2AudioSystem		KonaToCardAudSys (const NTV2AudioSystem ch) const;
		virtual NTV2AudioSystem		CardToKonaAudSys (const NTV2AudioSystem ch) const;
		virtual bool				HasCardAudSys (const NTV2AudioSystem ch) const;
		virtual bool				HasKonaAudSys (const NTV2AudioSystem ch) const;
		virtual NTV2InputXptID		VDevToCardInputXpt (const NTV2InputXptID inVDevInputXpt) const;
		virtual NTV2InputXptID		CardToVDevInputXpt (const NTV2InputXptID inCardInputXpt) const;
		virtual NTV2OutputXptID		VDevToCardOutputXpt (const NTV2OutputXptID inVDevOutputXpt) const;
		virtual NTV2OutputXptID		CardToVDevOutputXpt (const NTV2OutputXptID inCardOutputXpt) const;
		virtual NTV2Crosspoint		KonaToCardACXpt (const NTV2Crosspoint ch) const;
		virtual NTV2Crosspoint		CardToKonaACXpt (const NTV2Crosspoint ch) const;
		virtual INTERRUPT_ENUMS		VDevToCardInterrupt (const INTERRUPT_ENUMS i) const;
		virtual bool				DATKonaToCardFrmOffset (ULWord & inOutFrameNum, ULWord & inOutCardOffset) const;
		virtual bool				DATKonaToCard (ULWord & byteAddr, ULWord & byteCount) const;
		virtual bool				DATCardToKonaFrmOffset (ULWord & inOutFrameNum, ULWord & inOutCardOffset) const;
		virtual bool				isMyAncInsRegister	(const ULWord regNum) const;
		virtual bool				isMyAncExtRegister	(const ULWord regNum) const;
		virtual bool				isMyXptSelectRegister(const ULWord regNum) const
									{return mVDevXptRegInfos.find(regNum) != mVDevXptRegInfos.end();}
		virtual	bool				HandleReadXptSelectReg	(const ULWord regNum, ULWord & outRegValue, const ULWord regMask = 0xFFFFFFFF, const ULWord regShift = 0);
		virtual	bool				HandleWriteXptSelectReg	(const ULWord regNum, const ULWord regValue, const ULWord regMask = 0xFFFFFFFF, const ULWord regShift = 0);
		virtual	bool				HandleReadAncIns	(const ULWord regNum, ULWord & outRegValue, const ULWord regMask = 0xFFFFFFFF, const ULWord regShift = 0);
		virtual	bool				HandleReadAncExt	(const ULWord regNum, ULWord & outRegValue, const ULWord regMask = 0xFFFFFFFF, const ULWord regShift = 0);
		virtual	bool				HandleWriteAncIns	(const ULWord regNum, const ULWord inRegValue, const ULWord regMask = 0xFFFFFFFF, const ULWord regShift = 0);
		virtual	bool				HandleWriteAncExt	(const ULWord regNum, const ULWord inRegValue, const ULWord regMask = 0xFFFFFFFF, const ULWord regShift = 0);
		virtual bool				HandleReadGlobalControl (const ULWord regNum, ULWord & outValue, const ULWord mask = 0xFFFFFFFF, const ULWord shift = 0);
		virtual bool				HandleReadChannelControl (const ULWord regNum, ULWord & outValue, const ULWord mask = 0xFFFFFFFF, const ULWord shift = 0);
		virtual bool				HandleWriteSDITransmitControl (const ULWord regNum, const ULWord inRegValue, const ULWord regMask = 0xFFFFFFFF, const ULWord regShift = 0);
		virtual bool 				HandleReadSDITransmitControl(const ULWord RegNum, ULWord & OutValue, const ULWord RegMask = 0xFFFFFFFF, const ULWord RegShift = 0);
		virtual inline bool			ReadCardRegister (const RegInfo & regInfo, ULWord & outValue)
										{return mCard.ReadRegister(regInfo.regNum(), outValue, regInfo.mask(), regInfo.shift());}
		virtual inline bool			WriteCardRegister (const RegInfo & regInfo, const ULWord inValue)
										{return mCard.WriteRegister(regInfo.regNum(), inValue, regInfo.mask(), regInfo.shift());}
		virtual bool				GetVDevAudioMemoryOffset (const ULWord inOffsetBytes,  ULWord & outAbsByteOffset,
										const NTV2AudioSystem inAudSys) const;
		virtual bool 				GetCardIOChannelFromVirtual(NTV2Channel inVirtualChan, NTV2Channel & outCardChan);
		static bool					GetInputXptRegInfo (const NTV2InputXptID ixpt, RegInfo & outInfo);


	//	Instance Data
	private:
		string				mCP2config;			///< @brief	The full path to the Control Panel 2 configuration JSON
		string				mDevSN;				///< @brief The physical device serial number
		string				mVDID;				///< @brief The UUID of the virtual device created in CP2
		string 				mVDName;			///< @brief The name given to the virtual device by user in CP2
		json 				mVDjson;			///< @brief The json object representing the virutal device (from CP2 config json)
		uint64_t			mDLLHandle;			///< @brief	DLL handle
		const uint32_t		mHostSDKVersion;	///< @brief	Host/caller SDK version
		const uint32_t		mSDKVersion;		///< @brief	My SDK version
		NTV2DeviceID		mSimDeviceID;		///< @brief	Simulated device ID (DEVICE_ID_INVALID uses real device ID)
		mutable CNTV2Card	mCard;				///< @brief	My CNTV2Card object
		UWord				mChannel;			///< @brief	VDev Ch1 maps to this underlying device channel
		WgtMap				mCardToVDevWgts;	///< @brief	Widget mapping card-to-VDev
		WgtMap				mVDevToCardWgts;	///< @brief	Widget mapping VDev-to-card
		InXptMap			mCardToVDevIXpts;	///< @brief	Input crosspoint mapping card-to-VDev
		InXptMap			mVDevToCardIXpts;	///< @brief	Input crosspoint mapping VDev-to-card
		OutXptMap			mCardToVDevOXpts;	///< @brief	Output crosspoint mapping card-to-VDev
		OutXptMap			mVDevToCardOXpts;	///< @brief	Output crosspoint mapping VDev-to-card
		XptRegInfoMMap		mCardXptRegInfos;	///< @brief	Card inputXpt register-to-RegInfo multimap
		XptRegInfoMMap		mVDevXptRegInfos;	///< @brief	VDev inputXpt register-to-RegInfo multimap
		ChannelMap			mCardToVDevChls;	///< @brief	NTV2Channel card-to-VDev mapping
		ChannelMap			mVDevToCardChls;	///< @brief	NTV2Channel VDev-to-card mapping
		ChannelMap			mCardToVDevMxrs;	///< @brief	Mixer card-to-VDev mapping
		ChannelMap			mVDevToCardMxrs;	///< @brief	Mixer VDev-to-card mapping
		ACXptMap			mCardToKonaACXpts;	///< @brief	AutoCirc NTV2Crosspoint card-to-VDev mapping
		ACXptMap			mKonaToCardACXpts;	///< @brief	AutoCirc NTV2Crosspoint VDev-to-card mapping
		AudSysMap			mCardToKonaAudSys;	///< @brief	NTV2AudioSystem card-to-VDev mapping
		AudSysMap			mKonaToCardAudSys;	///< @brief	NTV2AudioSystem VDev-to-card mapping
		DATMap				mCardToKonaDAT;		///< @brief	DAT card-to-VDev mapping
		DATMap				mKonaToCardDAT;		///< @brief	DAT VDev-to-card mapping

};	//	NTV2VirtualDev

extern "C"
{
	EXPORT NTV2RPCClientAPI * CreateClient (void * pInDLLHandle, const NTV2ConnectParams & inParams, const uint32_t inCallerSDKVers);
}

NTV2RPCClientAPI * CreateClient (void * pInDLLHandle, const NTV2ConnectParams & inParams, const uint32_t inCallerSDKVers)
{
	AJADebug::Open();
    NTV2VirtualDev * pResult(new NTV2VirtualDev (pInDLLHandle, inParams, inCallerSDKVers));
	if (!pResult->NTV2Connect())
	{	AJA_sERROR(AJA_DebugUnit_RPCClient, AJAFUNC << ": NTV2Connect failed");
		delete pResult;
		return AJA_NULL;
	}	//	Failed
	AJA_sDEBUG(AJA_DebugUnit_RPCClient, AJAFUNC << ": returning " << xHEX0N(uint64_t(pResult),16));
	return pResult;
}

//	ALL THESE WERE COPIED FROM ntv2register.cpp:
//  uncomment as needed to avoid compiler warnings
// static const ULWord gChannelToGlobalControlRegNum []	= { kRegGlobalControl, kRegGlobalControlCh2, kRegGlobalControlCh3, kRegGlobalControlCh4,
// 															kRegGlobalControlCh5, kRegGlobalControlCh6, kRegGlobalControlCh7, kRegGlobalControlCh8, 0};

static const ULWord gChannelToSDIOutControlRegNum []	= { kRegSDIOut1Control, kRegSDIOut2Control, kRegSDIOut3Control, kRegSDIOut4Control,
															kRegSDIOut5Control, kRegSDIOut6Control, kRegSDIOut7Control, kRegSDIOut8Control, 0};

static const ULWord gChannelToControlRegNum []			= { kRegCh1Control, kRegCh2Control, kRegCh3Control, kRegCh4Control, kRegCh5Control, kRegCh6Control,
															kRegCh7Control, kRegCh8Control, 0};

static const ULWord gChannelToOutputFrameRegNum []		= { kRegCh1OutputFrame, kRegCh2OutputFrame, kRegCh3OutputFrame, kRegCh4OutputFrame,
															kRegCh5OutputFrame, kRegCh6OutputFrame, kRegCh7OutputFrame, kRegCh8OutputFrame, 0};

static const ULWord gChannelToInputFrameRegNum []		= { kRegCh1InputFrame, kRegCh2InputFrame, kRegCh3InputFrame, kRegCh4InputFrame,
															kRegCh5InputFrame, kRegCh6InputFrame, kRegCh7InputFrame, kRegCh8InputFrame, 0};
#if !defined(NTV2_DEPRECATE_16_2)
static const ULWord gChannelToPCIAccessFrameRegNum []	= { kRegCh1PCIAccessFrame, kRegCh2PCIAccessFrame, kRegCh3PCIAccessFrame, kRegCh4PCIAccessFrame,
															kRegCh5PCIAccessFrame, kRegCh6PCIAccessFrame, kRegCh7PCIAccessFrame, kRegCh8PCIAccessFrame, 0};
#endif	//	!defined(NTV2_DEPRECATE_16_2)

static const ULWord gAudioSystemToAudioControlRegNum [] = { kRegAud1Control,		kRegAud2Control,		kRegAud3Control,		kRegAud4Control,
															kRegAud5Control,		kRegAud6Control,		kRegAud7Control,		kRegAud8Control,		0};

static const ULWord gAudioSystemToSrcSelectRegNum []	= { kRegAud1SourceSelect,	kRegAud2SourceSelect,	kRegAud3SourceSelect,	kRegAud4SourceSelect,
															kRegAud5SourceSelect,	kRegAud6SourceSelect,	kRegAud7SourceSelect,	kRegAud8SourceSelect,	0};

static const ULWord gChannelToAudioInLastAddrRegNum []	= { kRegAud1InputLastAddr,	kRegAud2InputLastAddr,	kRegAud3InputLastAddr,	kRegAud4InputLastAddr,
															kRegAud5InputLastAddr,	kRegAud6InputLastAddr,	kRegAud7InputLastAddr,	kRegAud8InputLastAddr,	0};

static const ULWord gChannelToAudioOutLastAddrRegNum [] = { kRegAud1OutputLastAddr, kRegAud2OutputLastAddr, kRegAud3OutputLastAddr, kRegAud4OutputLastAddr,
															kRegAud5OutputLastAddr, kRegAud6OutputLastAddr, kRegAud7OutputLastAddr, kRegAud8OutputLastAddr, 0};

static const ULWord gAudioDelayRegisterNumbers []		= { kRegAud1Delay,	kRegAud2Delay,	kRegAud3Delay,	kRegAud4Delay,
															kRegAud5Delay,	kRegAud6Delay,	kRegAud7Delay,	kRegAud8Delay,	0};

static const ULWord gChannelToOutputTimingCtrlRegNum [] = { kRegOutputTimingControl, kRegOutputTimingControlch2, kRegOutputTimingControlch3, kRegOutputTimingControlch4,
															kRegOutputTimingControlch5, kRegOutputTimingControlch6, kRegOutputTimingControlch7, kRegOutputTimingControlch8, 0};

static const ULWord gChannelToSDIInput3GStatusRegNum [] = { kRegSDIInput3GStatus,		kRegSDIInput3GStatus,		kRegSDIInput3GStatus2,		kRegSDIInput3GStatus2,
															kRegSDI5678Input3GStatus,	kRegSDI5678Input3GStatus,	kRegSDI5678Input3GStatus,	kRegSDI5678Input3GStatus,	0};

static const ULWord gChannelToSDIIn3GbModeMask []		= { kRegMaskSDIIn3GbpsSMPTELevelBMode,	kRegMaskSDIIn23GbpsSMPTELevelBMode, kRegMaskSDIIn33GbpsSMPTELevelBMode, kRegMaskSDIIn43GbpsSMPTELevelBMode,
															kRegMaskSDIIn53GbpsSMPTELevelBMode, kRegMaskSDIIn63GbpsSMPTELevelBMode, kRegMaskSDIIn73GbpsSMPTELevelBMode, kRegMaskSDIIn83GbpsSMPTELevelBMode, 0};

static const ULWord gChannelToSDIIn3GbModeShift []		= { kRegShiftSDIIn3GbpsSMPTELevelBMode,		kRegShiftSDIIn23GbpsSMPTELevelBMode,	kRegShiftSDIIn33GbpsSMPTELevelBMode,	kRegShiftSDIIn43GbpsSMPTELevelBMode,
															kRegShiftSDIIn53GbpsSMPTELevelBMode,	kRegShiftSDIIn63GbpsSMPTELevelBMode,	kRegShiftSDIIn73GbpsSMPTELevelBMode,	kRegShiftSDIIn83GbpsSMPTELevelBMode,	0};

static const ULWord gIndexToVidProcControlRegNum []		= { kRegVidProc1Control,	kRegVidProc2Control,	kRegVidProc3Control,	kRegVidProc4Control,	0};

static const ULWord gIndexToVidProcMixCoeffRegNum []	= { kRegMixer1Coefficient,	kRegMixer2Coefficient,	kRegMixer3Coefficient,	kRegMixer4Coefficient,	0};
static const ULWord gIndexToVidProcFlatMatteRegNum []	= { kRegFlatMatteValue,		kRegFlatMatte2Value,	kRegFlatMatte3Value,	kRegFlatMatte4Value,	0};

static const ULWord gChannelToRP188ModeGCRegNum[]		= { kRegGlobalControl,			kRegGlobalControl,			kRegGlobalControl2,			kRegGlobalControl2,
																kRegGlobalControl2,			kRegGlobalControl2,			kRegGlobalControl2,			kRegGlobalControl2,			0};
static const ULWord gChannelToRP188ModeMasks[]				= { kRegMaskRP188ModeCh1,		kRegMaskRP188ModeCh2,		kRegMaskRP188ModeCh3,		kRegMaskRP188ModeCh4,
																kRegMaskRP188ModeCh5,		ULWord(kRegMaskRP188ModeCh6),	kRegMaskRP188ModeCh7,		kRegMaskRP188ModeCh8,		0};
static const ULWord gChannelToRP188ModeShifts[]			= { kRegShiftRP188ModeCh1,		kRegShiftRP188ModeCh2,		kRegShiftRP188ModeCh3,		kRegShiftRP188ModeCh4,
																kRegShiftRP188ModeCh5,		kRegShiftRP188ModeCh6,		kRegShiftRP188ModeCh7,		kRegShiftRP188ModeCh8,		0};
static const ULWord gChlToRP188DBBRegNum[]				= { kRegRP188InOut1DBB,			kRegRP188InOut2DBB,			kRegRP188InOut3DBB,			kRegRP188InOut4DBB,
																kRegRP188InOut5DBB,			kRegRP188InOut6DBB,			kRegRP188InOut7DBB,			kRegRP188InOut8DBB,			0};
static const ULWord gChlToRP188Bits031RegNum[]			= { kRegRP188InOut1Bits0_31,	kRegRP188InOut2Bits0_31,	kRegRP188InOut3Bits0_31,	kRegRP188InOut4Bits0_31,
																kRegRP188InOut5Bits0_31,	kRegRP188InOut6Bits0_31,	kRegRP188InOut7Bits0_31,	kRegRP188InOut8Bits0_31,	0};
static const ULWord gChlToRP188Bits3263RegNum[]			= { kRegRP188InOut1Bits32_63,	kRegRP188InOut2Bits32_63,	kRegRP188InOut3Bits32_63,	kRegRP188InOut4Bits32_63,
																kRegRP188InOut5Bits32_63,	kRegRP188InOut6Bits32_63,	kRegRP188InOut7Bits32_63,	kRegRP188InOut8Bits32_63,	0};

static const ULWord gChannelToRXSDIStatusRegs []			= { kRegRXSDI1Status,				kRegRXSDI2Status,				kRegRXSDI3Status,				kRegRXSDI4Status,				kRegRXSDI5Status,				kRegRXSDI6Status,				kRegRXSDI7Status,				kRegRXSDI8Status,				0};

static const ULWord gChannelToRXSDICRCErrorCountRegs[] = { kRegRXSDI1CRCErrorCount, kRegRXSDI2CRCErrorCount, kRegRXSDI3CRCErrorCount, kRegRXSDI4CRCErrorCount, kRegRXSDI5CRCErrorCount, kRegRXSDI6CRCErrorCount, kRegRXSDI7CRCErrorCount, kRegRXSDI8CRCErrorCount, 0 };

static const ULWord gChannelToSmpte372RegisterNum []		= { kRegGlobalControl,			kRegGlobalControl,			kRegGlobalControl2,			kRegGlobalControl2,
																kRegGlobalControl2,			kRegGlobalControl2,			kRegGlobalControl2,			kRegGlobalControl2,			0};
static const ULWord gChannelToSmpte372Masks []				= { kRegMaskSmpte372Enable,		kRegMaskSmpte372Enable,		kRegMaskSmpte372Enable4,	kRegMaskSmpte372Enable4,
																kRegMaskSmpte372Enable6,	kRegMaskSmpte372Enable6,	kRegMaskSmpte372Enable8,	kRegMaskSmpte372Enable8,	0};
static const ULWord gChannelToSmpte372Shifts []				= { kRegShiftSmpte372,			kRegShiftSmpte372,		kRegShiftSmpte372Enable4,	kRegShiftSmpte372Enable4,
																kRegShiftSmpte372Enable6,	kRegShiftSmpte372Enable6,	kRegShiftSmpte372Enable8,	kRegShiftSmpte372Enable8,	0};
static const ULWord gChannelToSDIIn3GModeMask []	= { kRegMaskSDIIn3GbpsMode,		kRegMaskSDIIn23GbpsMode,	kRegMaskSDIIn33GbpsMode,	kRegMaskSDIIn43GbpsMode,
														kRegMaskSDIIn53GbpsMode,	kRegMaskSDIIn63GbpsMode,	kRegMaskSDIIn73GbpsMode,	kRegMaskSDIIn83GbpsMode,	0};

static const ULWord gChannelToSDIIn3GModeShift []	= { kRegShiftSDIIn3GbpsMode,	kRegShiftSDIIn23GbpsMode,	kRegShiftSDIIn33GbpsMode,	kRegShiftSDIIn43GbpsMode,
														kRegShiftSDIIn53GbpsMode,	kRegShiftSDIIn63GbpsMode,	kRegShiftSDIIn73GbpsMode,	kRegShiftSDIIn83GbpsMode,	0};

static const ULWord	gChannelToSDIInVPIDLinkAValidMask[]	= {	kRegMaskSDIInVPIDLinkAValid,	kRegMaskSDIIn2VPIDLinkAValid,	kRegMaskSDIIn3VPIDLinkAValid,	kRegMaskSDIIn4VPIDLinkAValid,
															kRegMaskSDIIn5VPIDLinkAValid,	kRegMaskSDIIn6VPIDLinkAValid,	kRegMaskSDIIn7VPIDLinkAValid,	kRegMaskSDIIn8VPIDLinkAValid,	0};

static const ULWord	gChannelToSDIInVPIDARegNum []		= {	kRegSDIIn1VPIDA,			kRegSDIIn2VPIDA,			kRegSDIIn3VPIDA,			kRegSDIIn4VPIDA,
															kRegSDIIn5VPIDA,			kRegSDIIn6VPIDA,			kRegSDIIn7VPIDA,			kRegSDIIn8VPIDA,			0};

static const ULWord	gChannelToSDIInVPIDBRegNum []		= {	kRegSDIIn1VPIDB,			kRegSDIIn2VPIDB,			kRegSDIIn3VPIDB,			kRegSDIIn4VPIDB,
															kRegSDIIn5VPIDB,			kRegSDIIn6VPIDB,			kRegSDIIn7VPIDB,			kRegSDIIn8VPIDB,			0};

static const ULWord gChannelToSDIIn6GModeMask []	= { kRegMaskSDIIn16GbpsMode,		kRegMaskSDIIn26GbpsMode,	kRegMaskSDIIn36GbpsMode,	kRegMaskSDIIn46GbpsMode,
														kRegMaskSDIIn56GbpsMode,	kRegMaskSDIIn66GbpsMode,	kRegMaskSDIIn76GbpsMode,	kRegMaskSDIIn86GbpsMode,	0};

static const ULWord gChannelToSDIIn6GModeShift []	= { kRegShiftSDIIn16GbpsMode,	kRegShiftSDIIn26GbpsMode,	kRegShiftSDIIn36GbpsMode,	kRegShiftSDIIn46GbpsMode,
														kRegShiftSDIIn56GbpsMode,	kRegShiftSDIIn66GbpsMode,	kRegShiftSDIIn76GbpsMode,	kRegShiftSDIIn86GbpsMode,	0};

static const ULWord gChannelToSDIIn12GModeMask []	= { kRegMaskSDIIn112GbpsMode,		kRegMaskSDIIn212GbpsMode,	kRegMaskSDIIn312GbpsMode,	kRegMaskSDIIn412GbpsMode,
														kRegMaskSDIIn512GbpsMode,	kRegMaskSDIIn612GbpsMode,	kRegMaskSDIIn712GbpsMode,	ULWord(kRegMaskSDIIn812GbpsMode),	0};

static const ULWord gChannelToSDIIn12GModeShift []	= { kRegShiftSDIIn112GbpsMode,	kRegShiftSDIIn212GbpsMode,	kRegShiftSDIIn312GbpsMode,	kRegShiftSDIIn412GbpsMode,
														kRegShiftSDIIn512GbpsMode,	kRegShiftSDIIn612GbpsMode,	kRegShiftSDIIn712GbpsMode,	kRegShiftSDIIn812GbpsMode,	0};

static const ULWord gChannelToSDIInputStatusRegNum []		= { kRegInputStatus,		kRegInputStatus,		kRegInputStatus2,		kRegInputStatus2,
																kRegInput56Status,		kRegInput56Status,		kRegInput78Status,		kRegInput78Status,	0};

static const ULWord gChannelToSDIInputRateMask []			= { kRegMaskInput1FrameRate,			kRegMaskInput2FrameRate,			kRegMaskInput1FrameRate,			kRegMaskInput2FrameRate,
																kRegMaskInput1FrameRate,			kRegMaskInput2FrameRate,			kRegMaskInput1FrameRate,			kRegMaskInput2FrameRate,			0};
static const ULWord gChannelToSDIInputRateHighMask []		= { kRegMaskInput1FrameRateHigh,		kRegMaskInput2FrameRateHigh,		kRegMaskInput1FrameRateHigh,		kRegMaskInput2FrameRateHigh,
																kRegMaskInput1FrameRateHigh,		kRegMaskInput2FrameRateHigh,		kRegMaskInput1FrameRateHigh,		kRegMaskInput2FrameRateHigh,		0};
static const ULWord gChannelToSDIInputRateShift []			= { kRegShiftInput1FrameRate,			kRegShiftInput2FrameRate,			kRegShiftInput1FrameRate,			kRegShiftInput2FrameRate,
																kRegShiftInput1FrameRate,			kRegShiftInput2FrameRate,			kRegShiftInput1FrameRate,			kRegShiftInput2FrameRate,			0};
static const ULWord gChannelToSDIInputRateHighShift []		= { kRegShiftInput1FrameRateHigh,		kRegShiftInput2FrameRateHigh,		kRegShiftInput1FrameRateHigh,		kRegShiftInput2FrameRateHigh,
																kRegShiftInput1FrameRateHigh,		kRegShiftInput2FrameRateHigh,		kRegShiftInput1FrameRateHigh,		kRegShiftInput2FrameRateHigh,		0};

static const ULWord gChannelToSDIInputGeometryMask []		= { kRegMaskInput1Geometry,				kRegMaskInput2Geometry,				kRegMaskInput1Geometry,				kRegMaskInput2Geometry,
																kRegMaskInput1Geometry,				kRegMaskInput2Geometry,				kRegMaskInput1Geometry,				kRegMaskInput2Geometry,				0};
static const ULWord gChannelToSDIInputGeometryHighMask []	= { kRegMaskInput1GeometryHigh,			ULWord(kRegMaskInput2GeometryHigh), kRegMaskInput1GeometryHigh,			ULWord(kRegMaskInput2GeometryHigh),
																kRegMaskInput1GeometryHigh,			ULWord(kRegMaskInput2GeometryHigh), kRegMaskInput1GeometryHigh,			ULWord(kRegMaskInput2GeometryHigh), 0};
static const ULWord gChannelToSDIInputGeometryShift []		= { kRegShiftInput1Geometry,			kRegShiftInput2Geometry,			kRegShiftInput1Geometry,			kRegShiftInput2Geometry,
																kRegShiftInput1Geometry,			kRegShiftInput2Geometry,			kRegShiftInput1Geometry,			kRegShiftInput2Geometry,			0};
static const ULWord gChannelToSDIInputGeometryHighShift []	= { kRegShiftInput1GeometryHigh,		kRegShiftInput2GeometryHigh,		kRegShiftInput1GeometryHigh,		kRegShiftInput2GeometryHigh,
																kRegShiftInput1GeometryHigh,		kRegShiftInput2GeometryHigh,		kRegShiftInput1GeometryHigh,		kRegShiftInput2GeometryHigh,		0};

static const ULWord gChannelToSDIInputProgressiveMask []	= { kRegMaskInput1Progressive,			kRegMaskInput2Progressive,			kRegMaskInput1Progressive,			kRegMaskInput2Progressive,
																kRegMaskInput1Progressive,			kRegMaskInput2Progressive,			kRegMaskInput1Progressive,			kRegMaskInput2Progressive,			0};
static const ULWord gChannelToSDIInputProgressiveShift []	= { kRegShiftInput1Progressive,			kRegShiftInput2Progressive,			kRegShiftInput1Progressive,			kRegShiftInput2Progressive,
																kRegShiftInput1Progressive,			kRegShiftInput2Progressive,			kRegShiftInput1Progressive,			kRegShiftInput2Progressive,			0};

//													NTV2_AUDIOSYSTEM_1		NTV2_AUDIOSYSTEM_2		NTV2_AUDIOSYSTEM_3		NTV2_AUDIOSYSTEM_4
static const ULWord		sAudioDetectRegs []		= { kRegAud1Detect,			kRegAud1Detect,			kRegAudDetect2,			kRegAudDetect2,
//													NTV2_AUDIOSYSTEM_5		NTV2_AUDIOSYSTEM_6		NTV2_AUDIOSYSTEM_7		NTV2_AUDIOSYSTEM_8
													kRegAudioDetect5678,	kRegAudioDetect5678,	kRegAudioDetect5678,	kRegAudioDetect5678, 0 };

//static const ULWord sSignalRouterRegMasks[]		=	{	0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000	};
//static const ULWord sSignalRouterRegShifts[]	=	{			 0,			 8,			16,			24	};

static const ULWord sSDIXmitEnableMasks[] = {	kRegMaskSDI1Transmit, kRegMaskSDI2Transmit, kRegMaskSDI3Transmit, ULWord(kRegMaskSDI4Transmit),
												kRegMaskSDI5Transmit, kRegMaskSDI6Transmit, kRegMaskSDI7Transmit, kRegMaskSDI8Transmit};
static const ULWord sSDIXmitEnableShifts[] = {	kRegShiftSDI1Transmit, kRegShiftSDI2Transmit, kRegShiftSDI3Transmit, kRegShiftSDI4Transmit,
												kRegShiftSDI5Transmit, kRegShiftSDI6Transmit, kRegShiftSDI7Transmit, kRegShiftSDI8Transmit};

NTV2VirtualDev::NTV2VirtualDev (void * pInDLLHandle, const NTV2ConnectParams & inParams , const uint32_t inCallingVersion)
	:	NTV2RPCAPI		(inParams),
		mDLLHandle		(uint64_t(pInDLLHandle)),
		mHostSDKVersion	(inCallingVersion),
		mSDKVersion		(AJA_NTV2_SDK_VERSION),
		mSimDeviceID	(DEVICE_ID_INVALID),
		mChannel		(0)
{

	string queryStr(ConnectParam(kConnectParamQuery));
	if (!queryStr.empty())
		if (queryStr[0] == '?')
			queryStr.erase(0,1);	//	Remove leading '?'
	const NTV2StringList strs(aja::split(queryStr, "&"));
	
	
	for (NTV2StringListConstIter it(strs.begin());  it != strs.end();  ++it)
	{
		string str(*it), key, value;
		if (str.find("=") == string::npos)
		{
			key = aja::lower(str);
			mConnectParams.insert(key, value);
			NBDBG("'" << key << "'");
			continue;
		}
		NTV2StringList pieces(aja::split(str,"="));
		if (pieces.empty())
			continue;
		key = aja::lower(pieces.at(0));
		if (pieces.size() > 1)
			value = pieces.at(1);
		if (key.empty())
			{NBWARN("Empty key '" << key << "'");  continue;}
		if (HasConnectParam(key))
			NBDBG("Param '" << key << "' value '" << mConnectParams.valueForKey(key) << "' to be replaced with '" << value << "'");
		mConnectParams.insert(key, ::PercentDecode(value));
		NBDBG("'" << key << "' = '" << mConnectParams.valueForKey(key) << "'");
	}

	NBINFO("constructed, " << DEC(mConnectParams.size()) << " param(s): " << mConnectParams);

}

NTV2VirtualDev::~NTV2VirtualDev ()
{
	NTV2Disconnect();
	if (mDLLHandle)
	{
#if defined(MSWindows)
#else
		::dlclose(reinterpret_cast<void*>(mDLLHandle));  NBINFO("dlclose(" << xHEX0N(mDLLHandle,16) << ")");
#endif	//	AJAMac or AJALinux
	}
	else NBINFO("");
}

bool NTV2VirtualDev::NTV2Connect (void)
{
	//	Version check...
	if (mSDKVersion  &&  mHostSDKVersion  &&  mSDKVersion != mHostSDKVersion)
		NBWARN(" SDK version " << xHEX0N(mSDKVersion,8) << " doesn't match host SDK version " << xHEX0N(mHostSDKVersion,8));

	//	Check config params:
	const NTV2StringSet keys(mConnectParams.keys());
	NTV2StringList skippedParams;
	for (NTV2StringSetConstIter it(keys.begin());  it != keys.end();  ++it)
	{
		const string key(*it); string value(mConnectParams.valueForKey(key));
		if (key == "cp2configpath")
		{
			if (value.empty())
				{NBFAIL("'cp2configpath' parameter value missing or empty");  return false;}
			if (!mCP2config.empty())
				{NBFAIL("'cp2configpath' parameter specified more than once, was '" << mCP2config << "', now '" << value << "'"); return false;}
			mCP2config = value;
			NBINFO("'cp2configpath' parameter value '" << value << "' specified");
		}
		else if (key == "devicesn")
		{
			if (value.empty())
				{NBFAIL("'devicesn' parameter value missing or empty");  return false;}
			if (!mDevSN.empty())
				{NBFAIL("'devicesn' parameter specified more than once, was '" << mDevSN << "', now '" << value << "'"); return false;}
			mDevSN = value;
			NBINFO("'devicesn' parameter value '" << value << "' specified");
		}
		else if (key == "vdid")
		{
			if (value.empty())
				{NBFAIL("'vdid' parameter value missing or empty");  return false;}
			if (!mVDID.empty())
				{NBFAIL("'vdid' parameter specified more than once, was '" << mVDID << "', now '" << value << "'"); return false;}
			mVDID = value;
			NBINFO("'vdid' parameter value '" << value << "' specified");
		}
		else if (key == "help")
		{
			// VDTODO, update this
			ostringstream oss;
			oss << "NTV2VirtualDev:  This plugin is an NTV2 virtual device as configured in Control Panel 2" << endl
				<< "CONFIG PARAMS:" << endl
				<< "Name            Reqd    Default     Desc" << endl
				<< "devspec=spec    Yes     '0'         'spec' identifies the underlying device to connect to." << endl
				<< "channel=num     Yes     N/A         'num' specifies 1-based target channel on underlying device." << endl
				<< "verbose         No      N/A         Dumps widget & channel mapping info to stderr.";
			NBINFO(oss.str());
			cerr << oss.str() << endl;
			return false;
		}
		else if (key == "verbose")
		{
			if (!value.empty())
				{NBWARN("'verbose' parameter value not empty");  continue;}
		}
		else
			skippedParams.push_back(key);
	}	//	for each connectParams key
	if (!HasConnectParam("cp2configpath"))
		{NBFAIL("Required 'cp2configpath' parameter missing"); return false;}
	if (!HasConnectParam("devicesn"))
		{NBFAIL("Required 'devicesn' parameter missing"); return false;}
	if (!HasConnectParam("vdid"))
		{NBFAIL("Required 'vdid' parameter missing"); return false;}
	if (!skippedParams.empty())
		NBWARN("Skipped unrecognized parameter(s): " << skippedParams);

	//	Open the devSpec...
	if (!CNTV2DeviceScanner::GetFirstDeviceFromArgument(mDevSN, mCard))
		{NBFAIL("No underlying device '" << mDevSN << "'");  return false;}
	mSimDeviceID = mCard.GetDeviceID();   // VDTODO - Is this what we want to do?
	
	// Import the virtual device from the CP2 Json Config
	if(!ImportVDFromJSON())
		return false;

// CP2 should be managing all device compatibility questions, so commenting out the following...
//	if (mCard.GetDeviceID() == DEVICE_ID_VDev)
//		{NBFAIL("Underlying device '" << devSpec << "' cannot be DEVICE_ID_VDev");  return false;}
	//	Check other device capabilities...
//	if (!mCard.features().CanDoCapture())
//		{NBFAIL("'" << mCard.GetDisplayName() << "' cannot capture");  return false;}
//	if (!mCard.features().CanDoPlayback())
//		{NBFAIL("'" << mCard.GetDisplayName() << "' cannot output");  return false;}
//	if (mCard.features().GetNumFrameStores() < ::NTV2DeviceGetNumFrameStores(DEVICE_ID_VDev))
//		{NBFAIL("'" << mCard.GetDisplayName() << "' needs at least 2 FrameStores"); return false;}
//	if (mCard.features().GetNumAudioSystems() < ::NTV2DeviceGetNumAudioSystems(DEVICE_ID_VDev))
//		{NBFAIL("'" << mCard.GetDisplayName() << "' needs at least " << DEC(::NTV2DeviceGetNumAudioSystems(DEVICE_ID_VDev)) << " AudioSystems"); return false;}
//	if (mCard.features().GetNumCSCs() < ::NTV2DeviceGetNumCSCs(DEVICE_ID_VDev))
//		{NBFAIL("'" << mCard.GetDisplayName() << "' needs at least " << DEC(::NTV2DeviceGetNumCSCs(DEVICE_ID_VDev)) << " CSCs"); return false;}
//	if (mCard.features().GetNumLUTs() < ::NTV2DeviceGetNumLUTs(DEVICE_ID_VDev))
//		{NBFAIL("'" << mCard.GetDisplayName() << "' needs at least " << DEC(::NTV2DeviceGetNumLUTs(DEVICE_ID_VDev)) << " LUTs"); return false;}
//	if (mCard.features().GetNumMixers() < ::NTV2DeviceGetNumMixers(DEVICE_ID_VDev))
//		{NBFAIL("'" << mCard.GetDisplayName() << "' needs at least " << DEC(::NTV2DeviceGetNumMixers(DEVICE_ID_VDev)) << " Mixers"); return false;}
//	if (mCard.features().GetNumEmbeddedAudioInputChannels() < ::NTV2DeviceGetNumEmbeddedAudioInputChannels(DEVICE_ID_VDev))
//		{NBFAIL("'" << mCard.GetDisplayName() << "' needs at least " << DEC(::NTV2DeviceGetNumEmbeddedAudioInputChannels(DEVICE_ID_VDev)) << " embedded audio input channels"); return false;}
//	if (mCard.features().GetNumEmbeddedAudioOutputChannels() < ::NTV2DeviceGetNumEmbeddedAudioOutputChannels(DEVICE_ID_VDev))
//		{NBFAIL("'" << mCard.GetDisplayName() << "' needs at least " << DEC(::NTV2DeviceGetNumEmbeddedAudioOutputChannels(DEVICE_ID_VDev)) << " embedded audio output channels"); return false;}
//	if (mChannel >= mCard.features().GetNumFrameStores())
//		{NBFAIL(Name() << " specified map channel Ch" << DEC(mChannel+1) << " invalid on '" << mCard.GetDisplayName() << "'"); return false;}
//	if ((mChannel+1) >= mCard.features().GetNumFrameStores())
//		{NBFAIL(Name() << " specified map channel Ch" << DEC(mChannel+2) << " invalid on '" << mCard.GetDisplayName() << "'"); return false;}
//	if (mCard.features().GetActiveMemorySize() < ::NTV2DeviceGetActiveMemorySize(DEVICE_ID_VDev))
//		{NBFAIL("'" << mCard.GetDisplayName() << "' active SDRAM complement " << xHEX0N(mCard.features().GetActiveMemorySize(),8) << " < VDev " << xHEX0N(::NTV2DeviceGetActiveMemorySize(DEVICE_ID_VDev),8)); return false;}
//	if (mCard.features().HasBiDirectionalSDI())
//	{	bool inXmit(false), outXmit(false);
//		if (mCard.GetSDITransmitEnable (NTV2Channel(mChannel), inXmit)  &&  inXmit)
//			mCard.SetSDITransmitEnable (NTV2Channel(mChannel), false);	//	SDIIn1
//		if (mCard.GetSDITransmitEnable (NTV2Channel(mChannel+1), outXmit)  &&  !outXmit)
//			mCard.SetSDITransmitEnable (NTV2Channel(mChannel+1), true);	//	SDIOut1
//		if (inXmit) NBINFO("Bidirectional SDI" << DEC(mChannel+1) << " switched to input for VDev 'SDIIn1'");
//		if (!outXmit) NBINFO("Bidirectional SDI" << DEC(mChannel+2) << " switched to output for VDev 'SDIOut1'");
//	}
	
	if (mCard.IsOpen() && SetupMapping())
	{
		NBINFO(Description() << " is ready");
		return true;
	}
	else
		return false; 
}	//	NTV2Connect

bool NTV2VirtualDev::NTV2Disconnect (void)
{
	NBINFO("");
	return true;
}

bool NTV2VirtualDev::ImportVDFromJSON()
{
	std::ifstream cfgJsonfile(mCP2config);
	json cp2Json;
	if (cfgJsonfile.is_open())
		//VDTODO: Sent to debug sucessful open
		cp2Json = json::parse(cfgJsonfile);   //VDTODO  handle any parse_error exception, send to error
	else
		//VDTODO: Send to debug fail to open
		return false;
	cfgJsonfile.close();

	// locate the hardare device
	json devlist = cp2Json["v2"]["deviceConfigList"];
	json hwdev;
	for (const auto& dev : devlist) 
	{
		if (dev["serial"] == mDevSN)
		{
			hwdev = dev;
			break;
		}
	}
	//VDTODO error if no match HW Device

	// Locate the virtual device
	json vdevlist = hwdev["virtualDevices"];
	for (const auto& vdev : vdevlist) 
	{
		if (vdev["id"] == mVDID)
		{
			mVDjson = vdev; 
			mVDName = vdev["name"];
			break;
		}
	}   
	//VDTODO error if no match Virtual Device

	return true;
}

bool NTV2VirtualDev::GetVDevAudioMemoryOffset (const ULWord inOffsetBytes,  ULWord & outAbsByteOffset,
										const NTV2AudioSystem inAudSys) const
{
	//VDTODO: Is this appropriate for all devices?  Any special cases?
	outAbsByteOffset = 0; 
	const NTV2DeviceID	devID(mCard.GetDeviceID());   
	if (ULWord(inAudSys) >= ULWord(::NTV2DeviceGetNumAudioSystems(devID)) + 1)	//	kDeviceGetNumBufferedAudioSystems
		return false;	//	Invalid audio system

	//	VDev is a stacked audio device
	const ULWord	EIGHT_MEGABYTES (0x800000);
	const ULWord	memSize			(::NTV2DeviceGetActiveMemorySize(devID));
	const ULWord	engineOffset	(memSize  -	 EIGHT_MEGABYTES * ULWord(inAudSys+1));
	outAbsByteOffset = inOffsetBytes + engineOffset;
	return true;
}

bool NTV2VirtualDev::SetupMapping (void)
{

	if (!SetupWidgetMapping())
		return false;
	if (!SetupOutputXptMapping())
		return false;
	if (!SetupInputXptMapping())
		return false;
	if (!SetupXptSelectRegMapping())
		return false;
	//	AutoCirc NTV2Crosspoint & NTV2AudioSystem mappings...
	for (ChannelMapCIter it(mCardToVDevChls.begin());  it != mCardToVDevChls.end();  ++it)
	{
		mCardToKonaACXpts[::GetNTV2CrosspointInputForIndex(it->first)] = ::GetNTV2CrosspointInputForIndex(it->second);
		mCardToKonaACXpts[::GetNTV2CrosspointChannelForIndex(it->first)] = ::GetNTV2CrosspointChannelForIndex(it->second);
		mKonaToCardACXpts[::GetNTV2CrosspointInputForIndex(it->second)] = ::GetNTV2CrosspointInputForIndex(it->first);
		mKonaToCardACXpts[::GetNTV2CrosspointChannelForIndex(it->second)] = ::GetNTV2CrosspointChannelForIndex(it->first);
		const NTV2AudioSystem cardAudSys(::NTV2ChannelToAudioSystem(it->first));
		const NTV2AudioSystem konaAudSys(::NTV2ChannelToAudioSystem(it->second));
		mCardToKonaAudSys[cardAudSys] = konaAudSys;
		mKonaToCardAudSys[konaAudSys] = cardAudSys;
//		cerr << "Card AudSys" << DEC(cardAudSys+1) << " => VDev AudSys" << DEC(konaAudSys+1) << endl;

		ULWord cardByteOffset(0), konaByteOffset(0);
		if (!mCard.GetAudioMemoryOffset (0, cardByteOffset, cardAudSys)) continue;
		if (!GetVDevAudioMemoryOffset (0, konaByteOffset, konaAudSys)) continue;
		if (konaByteOffset == cardByteOffset) continue;
		mCardToKonaDAT[cardByteOffset] = konaByteOffset;
		mKonaToCardDAT[konaByteOffset] = cardByteOffset;
		if (HasConnectParam("verbose"))
			cerr << "Card AudSys" << DEC(cardAudSys+1) << " " << xHEX0N(cardByteOffset,8)
				<< " => VDev AudSys" << DEC(konaAudSys+1) << " " << xHEX0N(konaByteOffset,8) << endl;
	}
	return true;
}

bool NTV2VirtualDev::SetupWidgetMapping (void)
{
	// With CP2, the parent's widget list should be provided.
	// Currently assuming the VDEV channel is always NTV2_CHANNEL1

	//	Channel mapping...
	//  mChannel is used in various places in the plugin, and aligns
	//  with the framestore channel being used.  
	//  While processing widgets, set mChannel to the lowest channel device framestore.
	mChannel = 999;
	json mappedWidgets = mVDjson["mappedWidgets"];
	for (const auto& widMap : mappedWidgets) 
	{
		const NTV2WidgetID cardWgt(widMap["deviceWidgetId"]);
		const NTV2WidgetID VDevWgt(widMap["virtualWidgetId"]);
		const NTV2WidgetType wgtType (CNTV2SignalRouter::WidgetIDToType(cardWgt));
		if (wgtType == NTV2WidgetType_FrameStore)
		{
			NTV2Channel chan = CNTV2SignalRouter::WidgetIDToChannel(cardWgt);
			if (chan < mChannel)
				mChannel = chan;
		}
		mVDevToCardWgts [VDevWgt]	= cardWgt;
		mCardToVDevWgts [cardWgt]	= VDevWgt;
	}   
	//VDTODO:  Error tests & checking 

	if (mChannel == 999)
		mChannel = 0;

	mCardToVDevChls[NTV2Channel(mChannel)] = NTV2_CHANNEL1;
	mVDevToCardChls[NTV2_CHANNEL1] = NTV2Channel(mChannel);



// Below is from the Kona1 plugin which mapped all widgets from a Kona 1
/*  // Now assuming the widget mappings will be provided directly by CP2
	for (NTV2WidgetIDSetConstIter wit(mWgts.begin());  wit != mWgts.end();  ++wit)
	{	const NTV2WidgetID cardWgt(*wit);
		if (cardWgt == NTV2_WgtGenLock)
			continue;	//	Skip GenLock
		const NTV2WidgetType wgtType (CNTV2SignalRouter::WidgetIDToType(cardWgt));
		//const NTV2Channel VDevWgtNdx (CNTV2SignalRouter::WidgetIDToChannel(VDevWgt));
		const NTV2Channel VDevWgtNdx = NTV2_CHANNEL1;
		NTV2Channel cardWgtNdx = static_cast<NTV2Channel>(mChannel);

		// if (mCard.features().HasBiDirectionalSDI())
		// 	if (wgtType == NTV2WidgetType_SDIOut3G || wgtType == NTV2WidgetType_SDIOut12G || wgtType == NTV2WidgetType_DualLinkV2Out)
		// 		cardWgtNdx = NTV2Channel(cardWgtNdx + 1);
		if (wgtType == NTV2WidgetType_Mixer  ||  wgtType == NTV2WidgetType_SMPTE425Mux)
		{
			cardWgtNdx = NTV2Channel(mChannel / 2);
			mCardToVDevMxrs [cardWgtNdx] = VDevWgtNdx;
			mVDevToCardMxrs [VDevWgtNdx] = cardWgtNdx;
		}
		const NTV2WidgetID VDevWgt (CNTV2SignalRouter::WidgetIDFromTypeAndChannel(wgtType, VDevWgtNdx));
		mVDevToCardWgts [VDevWgt]	= cardWgt;
		mCardToVDevWgts [cardWgt]	= VDevWgt;
	}	//	for each VDev widget
*/

	if (mVDevToCardWgts.empty())
		{NBFAIL("No widgets");  return false;}

	//	Dump widget mapping...
	ostringstream oss;
	for (WgtMapCIter it(mVDevToCardWgts.begin());  it != mVDevToCardWgts.end();  ++it)
		oss << endl
				<< "\t" << ::NTV2WidgetIDToString(it->first, true) << "\t=>\t" << ::NTV2WidgetIDToString(it->second, true);
	if (HasConnectParam("verbose"))
	cerr << DEC(mVDevToCardWgts.size()) << " widget mappings for 'VDev' => '" << mCard.GetDisplayName() << "' Ch" << DEC(mChannel+1) << ":" << oss.str();
	return true;
}	//	SetupWidgetMapping

bool NTV2VirtualDev::SetupOutputXptMapping (void)
{
	//	Map output crosspoints...
	ostringstream oss;
	for (WgtMapCIter it(mVDevToCardWgts.begin());  it != mVDevToCardWgts.end();  ++it)
	{
		NTV2OutputXptIDSet vDevOxpts, cardOxpts;
		CNTV2SignalRouter::GetWidgetOutputs (it->first, vDevOxpts);
		CNTV2SignalRouter::GetWidgetOutputs (it->second, cardOxpts);
		if (vDevOxpts.find(NTV2_XptMixer1VidRGB) != vDevOxpts.end())
			vDevOxpts.erase(vDevOxpts.find(NTV2_XptMixer1VidRGB));	//	remove special case
		if (vDevOxpts.size() > cardOxpts.size())
		{	NBFAIL("VDev " << ::NTV2WidgetIDToString(it->first) << " " << DEC(vDevOxpts.size())
					<< " output(s) != " << DEC(cardOxpts.size()) << " output(s) from '" << mCard.GetDisplayName()
					<< "' " << ::NTV2WidgetIDToString(it->second));
			NBDBG("VDev " << vDevOxpts << " != '" << mCard.GetDisplayName() << "' " << cardOxpts);
			return false;
		}
		if (vDevOxpts.empty())
			continue;	//	Ignore widgets that have no output sockets

		NTV2OutputXptIDSetConstIter vDevIt(vDevOxpts.begin()), cardIt(cardOxpts.begin());
		while (true)
		{
			mCardToVDevOXpts[*cardIt] = *vDevIt;
			mVDevToCardOXpts[*vDevIt] = *cardIt;
			oss << endl
				<< DEC(mCardToVDevOXpts.size()) << ")\t" << ::NTV2OutputCrosspointIDToString(*vDevIt,true) << "\t=>\t" << ::NTV2OutputCrosspointIDToString(*cardIt,true);
			if (++vDevIt == vDevOxpts.end())
				break;
			if (++cardIt == cardOxpts.end())
				break;
		}
	}	//	for each widget

	//	Dump output xpt mapping
	if (HasConnectParam("verbose"))
	cerr << DEC(mCardToVDevOXpts.size()) << " output xpt mappings for 'VDev' => '" << mCard.GetDisplayName() << "' Ch" << DEC(mChannel+1) << ":" << oss.str() << endl;
	return !mCardToVDevOXpts.empty() && !mVDevToCardOXpts.empty();
}	//	SetupOutputXptMapping

bool NTV2VirtualDev::SetupInputXptMapping (void)
{
	//	Map input crosspoints...
	ostringstream oss;
	for (WgtMapCIter it(mVDevToCardWgts.begin());  it != mVDevToCardWgts.end();  ++it)
	{
		NTV2InputXptIDSet vDevIxpts, cardIxpts;
		CNTV2SignalRouter::GetWidgetInputs (it->first, vDevIxpts);
		CNTV2SignalRouter::GetWidgetInputs (it->second, cardIxpts);
		if (vDevIxpts.size() != cardIxpts.size())
		{	NBFAIL("VDev " << ::NTV2WidgetIDToString(it->first) << " " << DEC(vDevIxpts.size())
					<< " input(s) != " << DEC(cardIxpts.size()) << " input(s) from '" << mCard.GetDisplayName()
					<< "' " << ::NTV2WidgetIDToString(it->second));
			NBDBG("VDev " << vDevIxpts << " != '" << mCard.GetDisplayName() << "' " << cardIxpts);
			return false;
		}
		if (vDevIxpts.empty())
			continue;	//	Ignore widgets that have no input sockets

		NTV2InputXptIDSetConstIter vDevIt(vDevIxpts.begin()), cardIt(cardIxpts.begin());
		while (true)
		{
			mCardToVDevIXpts[*cardIt] = *vDevIt;
			mVDevToCardIXpts[*vDevIt] = *cardIt;
			oss << endl
				<< DEC(mCardToVDevIXpts.size()) << ")\t" << ::NTV2InputCrosspointIDToString(*vDevIt,true) << "\t=>\t" << ::NTV2InputCrosspointIDToString(*cardIt,true);
			if (++vDevIt == vDevIxpts.end())
				break;
			if (++cardIt == cardIxpts.end())
				break;
		}
	}	//	for each widget

	//	Dump input xpt mapping
	if (HasConnectParam("verbose"))
	cerr << DEC(mCardToVDevIXpts.size()) << " input xpt mappings for 'VDev' => '" << mCard.GetDisplayName() << "' Ch" << DEC(mChannel+1) << ":" << oss.str() << endl;
	return !mCardToVDevIXpts.empty() && !mVDevToCardIXpts.empty();
}	//	SetupInputXptMapping

bool NTV2VirtualDev::SetupXptSelectRegMapping (void)
{
	ostringstream ossK, ossC;
	RegInfo regInfo;
	//	VDev xpt select register mapping
	for (InXptMapCIter it(mVDevToCardIXpts.begin());  it != mVDevToCardIXpts.end();  ++it)
		if (GetInputXptRegInfo(it->first, regInfo))
		{
			mVDevXptRegInfos.insert(XptRegInfoPair(regInfo.regNum(), regInfo));
			ossK << endl << DEC(mVDevXptRegInfos.size()) << ")\t" << regInfo;
		}
	//	Card xpt select register mapping
	for (InXptMapCIter it(mCardToVDevIXpts.begin());  it != mCardToVDevIXpts.end();  ++it)
		if (GetInputXptRegInfo(it->first, regInfo))
		{
			mCardXptRegInfos.insert(XptRegInfoPair(regInfo.regNum(), regInfo));
			ossC << endl << DEC(mCardXptRegInfos.size()) << ")\t" << regInfo;
		}

	//	Dump VDev xpt select reg mapping
	if (HasConnectParam("verbose"))
	cerr << DEC(mVDevXptRegInfos.size()) << " VDev xptSelectReg(s):" << ossK.str() << endl
	<< DEC(mCardXptRegInfos.size()) << " '" << mCard.GetDisplayName() << "' xptSelectReg(s):" << ossC.str() << endl;
	return !mVDevXptRegInfos.empty()  &&  !mCardXptRegInfos.empty();
}	//	SetupXptSelectRegMapping

bool NTV2VirtualDev::GetInputXptRegInfo (const NTV2InputXptID ixpt, RegInfo & outInfo)
{
	uint32_t reg(0), ndx(0);
	bool result (CNTV2RegisterExpert::GetCrosspointSelectGroupRegisterInfo (ixpt, reg, ndx));
	outInfo.setRegNum(reg).setMaskShiftIndex(ndx).setInputXpt(ixpt);
	return result;
}

string NTV2VirtualDev::Name (void) const
{
	return mVDName;
	// ostringstream oss;
	// oss << "'VDev' proxy to '" << mCard.GetDisplayName() << "'";
	// return oss.str();
}

string NTV2VirtualDev::Description (void) const
{
	ostringstream oss;
	oss << Name() << ", Ch" << DEC(mChannel+1) <<" Virtual Device for: " << mCard.GetDisplayName();
	if (mCard.GetSerialNumber())
		if (!::SerialNum64ToString(mCard.GetSerialNumber()).empty())
			oss << " Serial '" << ::SerialNum64ToString(mCard.GetSerialNumber()) << "'";
	return oss.str();
}

bool NTV2VirtualDev::NTV2OpenRemote (void)
{
	return true;
}

NTV2Channel NTV2VirtualDev::VDevToCardChannel (const NTV2Channel ch) const
{
	ChannelMapCIter it(mVDevToCardChls.find(ch));
	return it != mVDevToCardChls.end() ? it->second : NTV2_CHANNEL_INVALID;
}
NTV2Channel NTV2VirtualDev::CardToVDevChannel (const NTV2Channel ch) const
{
	ChannelMapCIter it(mCardToVDevChls.find(ch));
	return it != mCardToVDevChls.end() ? it->second : NTV2_CHANNEL_INVALID;
}

NTV2Channel NTV2VirtualDev::VDevToCardMixer (const NTV2Channel ch) const
{
	ChannelMapCIter it(mVDevToCardMxrs.find(ch));
	return it != mVDevToCardMxrs.end() ? it->second : NTV2_CHANNEL1;
}
NTV2Channel NTV2VirtualDev::CardToVDevMixer (const NTV2Channel ch) const
{
	ChannelMapCIter it(mCardToVDevMxrs.find(ch));
	return it != mCardToVDevMxrs.end() ? it->second : NTV2_CHANNEL1;
}

NTV2AudioSystem NTV2VirtualDev::KonaToCardAudSys (const NTV2AudioSystem aud) const
{
	AudSysMapCIter it(mKonaToCardAudSys.find(aud));
	return it != mKonaToCardAudSys.end() ? it->second : NTV2_AUDIOSYSTEM_1;
}
NTV2AudioSystem NTV2VirtualDev::CardToKonaAudSys (const NTV2AudioSystem aud) const
{
	AudSysMapCIter it(mCardToKonaAudSys.find(aud));
	return it != mCardToKonaAudSys.end() ? it->second : NTV2_AUDIOSYSTEM_1;
}
bool NTV2VirtualDev::HasCardAudSys (const NTV2AudioSystem aud) const
{
	return mCardToKonaAudSys.find(aud) != mCardToKonaAudSys.end();
}
bool NTV2VirtualDev::HasKonaAudSys (const NTV2AudioSystem aud) const
{
	return mKonaToCardAudSys.find(aud) != mKonaToCardAudSys.end();
}


NTV2InputXptID NTV2VirtualDev::VDevToCardInputXpt (const NTV2InputXptID inVDevInputXpt) const
{	//	Map VDev input xpt to mCard's input xpt
	InXptMapCIter it(mVDevToCardIXpts.find(inVDevInputXpt));
	return it != mVDevToCardIXpts.end() ? it->second : NTV2_INPUT_CROSSPOINT_INVALID;
}

NTV2InputXptID NTV2VirtualDev::CardToVDevInputXpt (const NTV2InputXptID inCardInputXpt) const
{	//	Map mCard's input xpt to VDev input xpt
	InXptMapCIter it(mCardToVDevIXpts.find(inCardInputXpt));
	return it != mCardToVDevIXpts.end() ? it->second : NTV2_INPUT_CROSSPOINT_INVALID;
}

NTV2OutputXptID NTV2VirtualDev::VDevToCardOutputXpt (const NTV2OutputXptID xpt) const
{	//	Map VDev output xpt to mCard's output xpt
	OutXptMapCIter it(mVDevToCardOXpts.find(xpt));
	return it != mVDevToCardOXpts.end() ? it->second : NTV2_XptBlack;
}

NTV2OutputXptID NTV2VirtualDev::CardToVDevOutputXpt (const NTV2OutputXptID cardXpt) const
{	//	Unmap mCard's output xpt to VDev output xpt
	OutXptMapCIter it(mCardToVDevOXpts.find(cardXpt));
	return it != mCardToVDevOXpts.end() ? it->second : NTV2_XptBlack;
}

static const INTERRUPT_ENUMS Einputs[] = {eInput1, eInput2, eInput3, eInput4, eInput5, eInput6, eInput7, eInput8};

//	PER-SDI-SPIGOT REGISTER NUMBERS (Copied from ntv2anc.cpp)
//								SDI Spigot:		   1	   2	   3	   4	   5	   6	   7	   8
static const ULWord sAncInsBaseRegNum[] =	{	4608,	4672,	4736,	4800,	4864,	4928,	4992,	5056	};
static const ULWord sAncExtBaseRegNum[] =	{	4096,	4160,	4224,	4288,	4352,	4416,	4480,	4544	};
static const ULWord sAncInsNumRegs(19), sAncExtNumRegs (22);

NTV2Crosspoint NTV2VirtualDev::KonaToCardACXpt (const NTV2Crosspoint xpt) const
{
	ACXptMapCIter it(mKonaToCardACXpts.find(xpt));
	return it != mKonaToCardACXpts.end() ? it->second : NTV2CROSSPOINT_INVALID;
}

NTV2Crosspoint NTV2VirtualDev::CardToKonaACXpt (const NTV2Crosspoint xpt) const
{
	ACXptMapCIter it(mCardToKonaACXpts.find(xpt));
	return it != mCardToKonaACXpts.end() ? it->second : NTV2CROSSPOINT_INVALID;
}

INTERRUPT_ENUMS NTV2VirtualDev::VDevToCardInterrupt (const INTERRUPT_ENUMS i) const
{
	switch (i)
	{
		case eInput1:	return Einputs[mChannel];
		case eInput2:	return Einputs[mChannel+1];
		case eOutput1:
		case eOutput2:
		case eOutput3:
		case eOutput4:
		case eOutput5:
		case eOutput6:
		case eOutput7:
		case eOutput8:	return eVerticalInterrupt;
		default:	break;
	}
	return i;
}

//	Device Address Translation
//	This is necessary for audio streaming when the VDev audio system's buffer
//	address differs from the underlying device audio system's buffer address.
//	This implementation uses a simple std::map of 8MB-page-to-8MB-page.
bool NTV2VirtualDev::DATKonaToCardFrmOffset (ULWord & inOutFrameNum, ULWord & inOutCardOffsetBytes) const
{
	ULWord devAddr (0x800000 * inOutFrameNum + inOutCardOffsetBytes);	//	Assuming 8MB frames!!
	ULWord devAddrNearest8MB (devAddr & 0xFF800000);
	DATMapCIter it (mKonaToCardDAT.find(devAddrNearest8MB));
	if (it == mKonaToCardDAT.end())
		return true;	//	No mapping, return unchanged
	inOutCardOffsetBytes = devAddr - devAddrNearest8MB;
	devAddrNearest8MB = it->second;
	inOutFrameNum = devAddrNearest8MB >> 23;
	return true;
}

//	This function should return one or more address/length pairs
//	(particularly when byteCount spans 1 or more 8MB boundaries)
bool NTV2VirtualDev::DATKonaToCard (ULWord & byteAddress, ULWord & byteCount) const
{
	const ULWord devAddrNearest8MB (byteAddress & 0xFF800000);
	DATMapCIter it (mKonaToCardDAT.find(devAddrNearest8MB));
	if (it == mKonaToCardDAT.end())
		return true;	//	No mapping, return unchanged
	const ULWord bytesPast8MBBoundary (byteAddress - devAddrNearest8MB);
	byteAddress = it->second + bytesPast8MBBoundary;
	return true;
}

bool NTV2VirtualDev::DATCardToKonaFrmOffset (ULWord & inOutFrameNum, ULWord & inOutCardOffsetBytes) const
{
	ULWord devAddr (0x800000 * inOutFrameNum + inOutCardOffsetBytes);	//	Assuming 8MB frames!!
	ULWord devAddrNearest8MB (devAddr & 0xFF800000);
	DATMapCIter it (mCardToKonaDAT.find(devAddrNearest8MB));
	if (it == mCardToKonaDAT.end())
		return true;	//	No mapping, return unchanged
	inOutCardOffsetBytes = devAddr - devAddrNearest8MB;
	devAddrNearest8MB = it->second;
	inOutFrameNum = devAddrNearest8MB >> 23;
	return true;
}

bool NTV2VirtualDev::HandleReadXptSelectReg (const ULWord inRegNum, ULWord & outVal, const ULWord inMask, const ULWord inShift)
{
	XptRegMMapCIter pVDevXptRegInfo (mVDevXptRegInfos.find(inRegNum));
	if (pVDevXptRegInfo == mVDevXptRegInfos.end())
		return mCard.ReadRegister(inRegNum, outVal, inMask, inShift);
	while (pVDevXptRegInfo->first == inRegNum)
	{
		const RegInfo & konaXptRegInfo (pVDevXptRegInfo->second);
		if ((konaXptRegInfo.mask() & inMask) != konaXptRegInfo.mask())
			{++pVDevXptRegInfo;  continue;}	//	skip -- caller isn't interested in this input xpt
		const NTV2InputXptID cardInpXpt (VDevToCardInputXpt (konaXptRegInfo.inputXpt()));
		ULWord cardRegNum(0), nibbleNdx(0);
		if (!CNTV2RegisterExpert::GetCrosspointSelectGroupRegisterInfo (cardInpXpt, cardRegNum, nibbleNdx))
			return false;
		//	Read the card xpt select register, and get the output xpt value for the equivalent card input xpt...
		const RegInfo cardXptRegInfo (cardRegNum, nibbleNdx, cardInpXpt);
		ULWord cardOxpt(0);
		if (!ReadCardRegister (cardXptRegInfo, cardOxpt))
			return false;
		//	Translate the card outputXpt to the equivalent VDev outputXpt...
		const NTV2OutputXptID konaOutXpt (CardToVDevOutputXpt(NTV2OutputXptID(cardOxpt)));
		//	Update the VDev's xpt select register value...
		outVal = (outVal & konaXptRegInfo.invMask())	//	Keep everything intact except konaXptRegInfo.mask()
					| ULWord(konaOutXpt << konaXptRegInfo.shift());	//	and OR-in the konaOutXpt value
		++pVDevXptRegInfo;
	}	//	for each VDev input xpt represented in this register
	if (inShift  &&  inShift < 31)
		outVal >>= inShift;	//	Perform shift requested by caller
	return true;
}	//	HandleReadXptSelectReg

bool NTV2VirtualDev::HandleWriteXptSelectReg (const ULWord inRegNum, const ULWord inVal, const ULWord inMask, const ULWord inShift)
{
	XptRegMMapCIter pVDevXptRegInfo (mVDevXptRegInfos.find(inRegNum));
	if (pVDevXptRegInfo == mVDevXptRegInfos.end())
		return mCard.WriteRegister(inRegNum, inVal, inMask, inShift);	//	Not my register

	NBDBG(CNTV2RegisterExpert::GetDisplayName(inRegNum) << " (" << DEC(inRegNum) << ") val=" << xHEX0N(inVal,8) << "(" << DEC(inVal) << ") msk=" << xHEX0N(inMask,8) << " sh=" << DEC(inShift));
	while (pVDevXptRegInfo->first == inRegNum)
	{
		const RegInfo & konaXptRegInfo (pVDevXptRegInfo->second);
		NBDBG("VDev " << konaXptRegInfo);
		if ((konaXptRegInfo.mask() & inMask) != konaXptRegInfo.mask())
			{++pVDevXptRegInfo;  continue;}	//	skip -- caller isn't interested in this input xpt
		const NTV2OutputXptID konaOxpt (NTV2OutputXptID(inVal+0));
		//	Translate the VDev outputXpt to the equivalent card outputXpt...
		const NTV2OutputXptID cardOutXpt (VDevToCardOutputXpt(konaOxpt));
		const NTV2InputXptID cardInpXpt (VDevToCardInputXpt (konaXptRegInfo.inputXpt()));
		ULWord cardRegNum(0), nibbleNdx(0);
		if (!CNTV2RegisterExpert::GetCrosspointSelectGroupRegisterInfo (cardInpXpt, cardRegNum, nibbleNdx))
			return false;
		//	Write the the cardOutXpt value into the card xpt select register...
		const RegInfo cardXptRegInfo (cardRegNum, nibbleNdx, cardInpXpt);
		NBDBG("'" << mCard.GetDisplayName() << "' " << cardXptRegInfo);
		if (!WriteCardRegister (cardXptRegInfo, cardOutXpt))
			return false;
		++pVDevXptRegInfo;
	}	//	for each VDev input xpt represented in this register
	return true;
}	//	HandleWriteXptSelectReg

static const ULWord AncInsRegOffsetPerChannel (sAncInsBaseRegNum[1] - sAncInsBaseRegNum[0]);
static const ULWord AncExtRegOffsetPerChannel (sAncExtBaseRegNum[1] - sAncExtBaseRegNum[0]);

bool NTV2VirtualDev::HandleReadAncIns (const ULWord regNum, ULWord & outRegValue, const ULWord regMask, const ULWord regShift)
{
	ULWord newRegNum(0);
	if (regNum >= sAncInsBaseRegNum[0] && regNum < sAncInsBaseRegNum[0]+sAncInsNumRegs)
		newRegNum = regNum + ULWord(mChannel+1) * AncInsRegOffsetPerChannel;	//	Map to anc ins 0 reg on target
	if (newRegNum)
		return mCard.ReadRegister (newRegNum, outRegValue, regMask, regShift);
	outRegValue = 0;	//	Otherwise return zero value and ignore
	return true;
}

bool NTV2VirtualDev::HandleWriteAncIns (const ULWord regNum, const ULWord inRegValue, const ULWord regMask, const ULWord regShift)
{
	ULWord newRegNum(0);
	if (regNum >= sAncInsBaseRegNum[0] && regNum < sAncInsBaseRegNum[0]+sAncInsNumRegs)
		newRegNum = regNum + ULWord(mChannel+1) * AncInsRegOffsetPerChannel;	//	Map to anc ins 0 reg on target
	if (newRegNum)
		return mCard.WriteRegister (newRegNum, inRegValue, regMask, regShift);
	return true;	//	Otherwise ignore
}

bool NTV2VirtualDev::HandleReadAncExt (const ULWord regNum, ULWord & outRegValue, const ULWord regMask, const ULWord regShift)
{
	ULWord newRegNum(0);
	if (regNum >= sAncExtBaseRegNum[0] && regNum < sAncExtBaseRegNum[0]+sAncExtNumRegs)
		newRegNum = regNum + ULWord(mChannel) * AncExtRegOffsetPerChannel;		//	Map to anc ext 0 reg on target
	if (newRegNum)
		return mCard.ReadRegister (newRegNum, outRegValue, regMask, regShift);
	outRegValue = 0;	//	Otherwise return zero value and ignore
	return true;
}

bool NTV2VirtualDev::HandleWriteAncExt (const ULWord regNum, const ULWord inRegValue, const ULWord regMask, const ULWord regShift)
{
	ULWord newRegNum(0);
	if (regNum >= sAncExtBaseRegNum[0] && regNum < sAncExtBaseRegNum[0]+sAncExtNumRegs)
		newRegNum = regNum + ULWord(mChannel) * AncExtRegOffsetPerChannel;		//	Map to anc ext 0 reg on target
	if (newRegNum)
		return mCard.WriteRegister (newRegNum, inRegValue, regMask, regShift);
	return true;	//	otherwise ignore
}

bool NTV2VirtualDev::isMyAncInsRegister	(const ULWord regNum) const
{
	return regNum >= sAncInsBaseRegNum[0]  &&  regNum < (sAncInsBaseRegNum[0] + sAncInsNumRegs);
}

bool NTV2VirtualDev::isMyAncExtRegister (const ULWord regNum) const
{
	return regNum >= sAncExtBaseRegNum[0] && regNum < (sAncExtBaseRegNum[0] + sAncExtNumRegs);
}

bool NTV2VirtualDev::HandleReadGlobalControl (const ULWord regNum, ULWord & outValue, const ULWord mask, const ULWord shift)
{
	if (!mCard.ReadRegister(regNum, outValue))	//	Read underlying card global ctrl register, no mask, no shift (yet)
		return false;
	if (regNum == kRegGlobalControl)
	{
		if ((mask & kRegMaskRefSource) == kRegMaskRefSource)
		{	//	Patch outValue's RefSource bits...
			ULWord val(outValue & kRegMaskRefSource);
			const NTV2ReferenceSource refSrcSDIIn(::NTV2InputSourceToReferenceSource(::NTV2ChannelToInputSource(NTV2Channel(mChannel))));
			const ULWord		cardSDIInBits	(ULWord(refSrcSDIIn) << kRegShiftRefSource);
			static const ULWord	SDIIn1Bits		(ULWord(NTV2_REFERENCE_INPUT1) << kRegShiftRefSource);
			static const ULWord	ExtRefBits		(ULWord(NTV2_REFERENCE_EXTERNAL) << kRegShiftRefSource);
			static const ULWord	FreeRunBits		(ULWord(NTV2_REFERENCE_FREERUN) << kRegShiftRefSource);
			if (val == cardSDIInBits)
				outValue = (outValue & ~(kRegMaskRefSource))  |  SDIIn1Bits;	//	Patch ref source mChannel to SDIIn1
			else if (val == ExtRefBits)
				;	//	OK, leave intact
			else if (val == FreeRunBits)
				;	//	OK, leave intact
			else	//	Ref input on underlying card doesn't exist on VDev
				outValue = (outValue & ~(kRegMaskRefSource))  |  FreeRunBits;	//	Return FreeRun instead
		}	//	RefSource bits
	}	//	Reg 0 -- GlobalControl
	else if (regNum == kRegGlobalControl2)
	{	//	VDev can't do 4K/UHD/8K/UHD2, nor multiformat mode...
		outValue = (outValue & ~(kRegMaskQuadMode));		//	QuadMode bit OFF!
		outValue = (outValue & ~(kRegMaskQuadMode2));		//	QuadMode2 bit OFF!
		outValue = (outValue & ~(kRegMaskIndependentMode));	//	IndependentMode bit OFF!
		outValue = (outValue & ~(kRegMask425FB12));			//	425FB1&2 mode bit OFF!
		outValue = (outValue & ~(kRegMask425FB34));			//	425FB3&4 mode bit OFF!
		outValue = (outValue & ~(kRegMask425FB56));			//	425FB5&6 mode bit OFF!
		outValue = (outValue & ~(kRegMask425FB78));			//	425FB7&8 mode bit OFF!
	}
	else return false;
	outValue &= mask;
	outValue >>= shift;
	return true;
}

bool NTV2VirtualDev::HandleReadChannelControl (const ULWord regNum, ULWord & outValue, const ULWord mask, const ULWord shift)
{
	if (!mCard.ReadRegister(gChannelToControlRegNum[regNum == kRegCh1Control ? mChannel : mChannel+1], outValue))
		return false;
	if (regNum == kRegCh1Control)
	{
		if ((mask & kK2RegMaskFrameSize) == kK2RegMaskFrameSize)	//	Reading intrinsic frame size?
		{	//	Must pull this from kRegCh1Control
			ULWord val(0);
			// if (!mCard.ReadRegister(regNum, val, kK2RegMaskFrameSize))   On Mac, this is returning a "shifted" value (not expected)
			if (!mCard.ReadRegister(regNum, val))
				return false;
			outValue = (outValue & ~(kK2RegMaskFrameSize))  |  val;
		}
	}
	outValue &= mask;
	outValue >>= shift;
	return true;
}

// Given a virtual channel, checks against all virtual widgets for any IO widgets that match that channel.
// When the first match is found, sets outCardChan to the corresponding phsycial channel 
// Returns false if no matches are found
bool NTV2VirtualDev::GetCardIOChannelFromVirtual(NTV2Channel inVirtualChan, NTV2Channel & outCardChan)
{
	for (const auto& pair : mVDevToCardWgts)
	{
		//const NTV2WidgetID wgtID = pair.first;  //  cardWgt(*wit);
		const NTV2WidgetType wgtType (CNTV2SignalRouter::WidgetIDToType(pair.first));
		if (wgtType == NTV2WidgetType_SDIIn || wgtType == NTV2WidgetType_SDIIn3G || wgtType == NTV2WidgetType_SDIOut ||
		      wgtType == NTV2WidgetType_SDIOut3G || wgtType == NTV2WidgetType_SDIIn12G || wgtType == NTV2WidgetType_SDIOut12G)
		{
			const NTV2Channel VDWgtChannel (CNTV2SignalRouter::WidgetIDToChannel(pair.first));
			if (VDWgtChannel == inVirtualChan)
			{
				outCardChan = CNTV2SignalRouter::WidgetIDToChannel(pair.second);
				return true;
			}
		}
	}

	return false;
}

bool NTV2VirtualDev::HandleWriteSDITransmitControl(const ULWord inRegNum, const ULWord inRegVal, const ULWord inRegMask, const ULWord inRegShift)
{
	// This register controls multiple channels.  Examine mask to see which channels are being set to the VD.
	// If we have an I/O widget corresponding to this channel, look up the phsyical card channel and set it.

	if (!mCard.features().HasBiDirectionalSDI())
		return true;  // ignore

	ULWord VDRegVal ((inRegVal << inRegShift) & inRegMask);

	for (int i=0; i < sizeof(sSDIXmitEnableMasks) / sizeof(sSDIXmitEnableMasks[0]); i++) //Iterate virtual channels by mask
	{
		if (sSDIXmitEnableMasks[i] & inRegMask) // Is this virtual channel being set?
		{	
			ULWord VDValue = (VDRegVal & sSDIXmitEnableMasks[i]) >> sSDIXmitEnableShifts[i];
			NTV2Channel virtualChannel = static_cast<NTV2Channel>(i);
			NTV2Channel cardChannel;
			if(GetCardIOChannelFromVirtual(virtualChannel,cardChannel)) // get the physical IO channel
			{
				if (!mCard.WriteRegister (inRegNum, VDValue, sSDIXmitEnableMasks[cardChannel], sSDIXmitEnableShifts[cardChannel]))
					return false;
			}
		}
	}

	return true;  
}

bool NTV2VirtualDev::HandleReadSDITransmitControl(const ULWord inRegNum, ULWord & OutValue, const ULWord inRegMask, const ULWord inRegShift)
{
	// This register controls multiple channels.  Examine mask to see which channels are being read from the VD.
	// If we have an I/O widget corresponding to this channel, look up the phsyical card, read the value, and set it at the VD's mask location

	if (!mCard.features().HasBiDirectionalSDI())
		return true;  // ignore

	ULWord cardValue;
	for (int i=0; i < sizeof(sSDIXmitEnableMasks) / sizeof(sSDIXmitEnableMasks[0]); i++) //Iterate virtual channels by mask
	{
		if (sSDIXmitEnableMasks[i] & inRegMask) // Is this virtual channel being read?
		{	
			cardValue = 0;
			NTV2Channel virtualChannel = static_cast<NTV2Channel>(i);
			NTV2Channel cardChannel;
			if(GetCardIOChannelFromVirtual(virtualChannel,cardChannel)) // get the physical IO channel
			{
				if (!mCard.ReadRegister (inRegNum, cardValue, sSDIXmitEnableMasks[cardChannel], sSDIXmitEnableShifts[cardChannel]))
					return false;
				if (cardValue)
					OutValue |= sSDIXmitEnableMasks[i];  // set bit
				else
					OutValue &= ~sSDIXmitEnableMasks[i];   // clear bit
			}
		}
	}
	if (inRegShift  &&  inRegShift < 31)
		OutValue >>= inRegShift;	//	Perform shift requested by caller

	return true;  
}

static const ULWord kRegMaskPCMCtrlA1(kRegMaskPCMControlA1P1_2|kRegMaskPCMControlA1P3_4|kRegMaskPCMControlA1P5_6|kRegMaskPCMControlA1P7_8|kRegMaskPCMControlA1P9_10|kRegMaskPCMControlA1P11_12|kRegMaskPCMControlA1P13_14|kRegMaskPCMControlA1P15_16);
static const ULWord kRegMaskPCMCtrlA2(kRegMaskPCMControlA2P1_2|kRegMaskPCMControlA2P3_4|kRegMaskPCMControlA2P5_6|kRegMaskPCMControlA2P7_8|kRegMaskPCMControlA2P9_10|kRegMaskPCMControlA2P11_12|kRegMaskPCMControlA2P13_14|kRegMaskPCMControlA2P15_16);

bool NTV2VirtualDev::NTV2ReadRegisterRemote (const ULWord inRegNum, ULWord & outValue, const ULWord inRegMask, const ULWord inRegShift)
{	//	Translate VDev register reads into underlying device's register reads...
	ULWord regNum(inRegNum), regMask(inRegMask), regShift(inRegShift);
	if (isMyAncExtRegister(regNum))
		return HandleReadAncExt (regNum, outValue, regMask, regShift);	//	Handle my anc ext regs
	if (isMyAncInsRegister(regNum))
		return HandleReadAncIns (regNum, outValue, regMask, regShift);	//	Handle my anc ins regs
	if (isMyXptSelectRegister(regNum))
		return HandleReadXptSelectReg (inRegNum, outValue, inRegMask, inRegShift);	//	Handle my routing regs
	switch (regNum)
	{
		case kRegBoardID:
			outValue = mSimDeviceID & inRegMask;
			if (inRegShift  &&  inRegShift < 31)
				outValue >>= inRegShift;
			return true;

		case kRegAud1Control:			regNum = gAudioSystemToAudioControlRegNum[KonaToCardAudSys(NTV2_AUDIOSYSTEM_1)];	break;
		case kRegAud1SourceSelect:		regNum = gAudioSystemToSrcSelectRegNum[KonaToCardAudSys(NTV2_AUDIOSYSTEM_1)];		break;
		case kRegAud1OutputLastAddr:	regNum = gChannelToAudioOutLastAddrRegNum[KonaToCardAudSys(NTV2_AUDIOSYSTEM_1)];	break;
		case kRegAud1InputLastAddr:		regNum = gChannelToAudioInLastAddrRegNum[KonaToCardAudSys(NTV2_AUDIOSYSTEM_1)];		break;
		case kRegAud1Delay:				regNum = gAudioDelayRegisterNumbers[KonaToCardAudSys(NTV2_AUDIOSYSTEM_1)];			break;
		case kRegAud1Detect:			regNum = sAudioDetectRegs[KonaToCardAudSys(NTV2_AUDIOSYSTEM_1)];					break;
		case kRegAud2Control:			regNum = gAudioSystemToAudioControlRegNum[KonaToCardAudSys(NTV2_AUDIOSYSTEM_2)];	break;
		case kRegAud2SourceSelect:		regNum = gAudioSystemToSrcSelectRegNum[KonaToCardAudSys(NTV2_AUDIOSYSTEM_2)];		break;
		case kRegAud2OutputLastAddr:	regNum = gChannelToAudioOutLastAddrRegNum[KonaToCardAudSys(NTV2_AUDIOSYSTEM_2)];	break;
		case kRegAud2InputLastAddr:		regNum = gChannelToAudioInLastAddrRegNum[KonaToCardAudSys(NTV2_AUDIOSYSTEM_2)];		break;
		case kRegAud2Delay:				regNum = gAudioDelayRegisterNumbers[KonaToCardAudSys(NTV2_AUDIOSYSTEM_2)];			break;
		case kRegAudDetect2:			regNum = sAudioDetectRegs[KonaToCardAudSys(NTV2_AUDIOSYSTEM_2)];					break;

		case kRegPCMControl4321:
		{	NTV2AudioSystem cardAudSys(KonaToCardAudSys(NTV2_AUDIOSYSTEM_1));
			//	Mask & Shift for AudSys1 is mapped to KonaToCardAudSys
			if (cardAudSys == NTV2_AUDIOSYSTEM_1)
				return mCard.ReadRegister(regNum, outValue, regMask, regShift);	//	No mapping necessary
			if (cardAudSys > NTV2_AUDIOSYSTEM_4)
			{
				regNum = kRegPCMControl8765;
				cardAudSys = NTV2AudioSystem(cardAudSys - 4);
			}
			//	Read raw reg value...
			if (!mCard.ReadRegister(regNum, outValue))
				return false;
			if (regMask & kRegMaskPCMCtrlA1)
			{	//	AudSys1 request
				ULWord value(outValue);
				value &= 0x000000FF << (cardAudSys * 8);
				value >>= (cardAudSys - NTV2_AUDIOSYSTEM_1) * 8;
				outValue &= 0xFFFFFF00;	//	clear Ch1 bits
				outValue |= value;		//	set Ch1 bits
			}
			if (regMask & kRegMaskPCMCtrlA2)
			{	//	AudSys2 request
				cardAudSys = NTV2AudioSystem(cardAudSys + 1);
				ULWord value(outValue);
				value &= 0x000000FF << (cardAudSys * 8);
				value >>= (cardAudSys - NTV2_AUDIOSYSTEM_2) * 8;
				outValue &= 0xFFFF00FF;	//	clear Ch2 bits
				outValue |= value;		//	set Ch2 bits
			}
			outValue &= inRegMask;
			if (inRegShift  &&  inRegShift < 31)
				outValue >>= inRegShift;
			return true;
		}

		case kRegGlobalControl:
		case kRegGlobalControl2:		return HandleReadGlobalControl(regNum, outValue, regMask, regShift);

		case kRegCh1Control:
		case kRegCh2Control:			return HandleReadChannelControl (regNum, outValue, regMask, regShift);

		case kRegSDITransmitControl:	return HandleReadSDITransmitControl(regNum, outValue, regMask, regShift);

		case kRegSDIOut1Control:
		regNum = gChannelToSDIOutControlRegNum[mChannel+1];
		if (inRegMask & (BIT(18)|BIT(19)|BIT(28)|BIT(29)|BIT(30)|BIT(31)))	//	Any audio system bits set?
		{
			//	Bits 18|28|30 have DS1 AudioSystem;	Bits 19|29|31 have DS2 AudioSystem
			//	Have to translate the AudioSystem values:
			if (!mCard.ReadRegister(regNum, outValue))
				return false;
			NTV2AudioSystem cardAudSys(NTV2_AUDIOSYSTEM_INVALID), konaAudSys(NTV2_AUDIOSYSTEM_INVALID);
			ULWord newBits(0);
			//	DS1 audio system:
			cardAudSys = NTV2AudioSystem( (outValue & BIT(18) ? 4 : 0)
										+ (outValue & BIT(28) ? 2 : 0)
										+ (outValue & BIT(30) ? 1 : 0));
			if (HasCardAudSys(cardAudSys))
			{
				konaAudSys = CardToKonaAudSys(cardAudSys);
				outValue &= 0xFFFFFFFF - BIT(30) - BIT(28) - BIT(18);	//	Clear those bits
				newBits = (konaAudSys & 4 ? BIT(18) : 0)
							| (konaAudSys & 2 ? BIT(28) : 0)
							| (konaAudSys & 1 ? BIT(30) : 0);
				outValue |= newBits;	//	Set those bits
			}
			//	DS2 audio system:
			cardAudSys = NTV2AudioSystem( (outValue & BIT(19) ? 4 : 0)
										+ (outValue & BIT(29) ? 2 : 0)
										+ (outValue & BIT(31) ? 1 : 0));
			if (HasCardAudSys(cardAudSys))
			{
				konaAudSys = CardToKonaAudSys(cardAudSys);
				outValue &= 0xFFFFFFFF - BIT(31) - BIT(29) - BIT(19);	//	Clear those bits
				newBits = (konaAudSys & 4 ? BIT(19) : 0)
							| (konaAudSys & 2 ? BIT(29) : 0)
							| (konaAudSys & 1 ? BIT(31) : 0);
				outValue |= newBits;	//	Set those bits
			}
			outValue &= inRegMask;
			if (inRegShift  &&  inRegShift < 31)
				outValue >>= inRegShift;
			return true;
		}
		break;
		
		case kRegCh1OutputFrame:			regNum = gChannelToOutputFrameRegNum[mChannel];			break;
		case kRegCh2OutputFrame:			regNum = gChannelToOutputFrameRegNum[mChannel+1];		break;
		case kRegCh1InputFrame:				regNum = gChannelToInputFrameRegNum[mChannel];			break;
		case kRegCh2InputFrame:				regNum = gChannelToInputFrameRegNum[mChannel+1];		break;
#if !defined(NTV2_DEPRECATE_16_2)
		case kRegCh1PCIAccessFrame:			regNum = gChannelToPCIAccessFrameRegNum[mChannel];		break;
		case kRegCh2PCIAccessFrame:			regNum = gChannelToPCIAccessFrameRegNum[mChannel+1];	break;
#endif	//	NTV2_DEPRECATE_16_2
		case kRegOutputTimingControl:		regNum = gChannelToOutputTimingCtrlRegNum[mChannel+1];	break;
		case kRegVidProc1Control:			regNum = gIndexToVidProcControlRegNum[VDevToCardMixer(NTV2_CHANNEL1)];		break;
		case kRegMixer1Coefficient:			regNum = gIndexToVidProcMixCoeffRegNum[VDevToCardMixer(NTV2_CHANNEL1)];		break;
		case kRegFlatMatteValue:			regNum = gIndexToVidProcFlatMatteRegNum[VDevToCardMixer(NTV2_CHANNEL1)];	break;
		case kRegRXSDI1Status:				regNum = gChannelToRXSDIStatusRegs[mChannel];			break;
		case kRegRXSDI2Status:				regNum = gChannelToRXSDIStatusRegs[mChannel+1];			break;
		case kRegRXSDI1CRCErrorCount:		regNum = gChannelToRXSDICRCErrorCountRegs[mChannel];	break;
		case kRegSDIIn1VPIDA:				regNum = gChannelToSDIInVPIDARegNum[mChannel];			break;
		case kRegSDIIn1VPIDB:				regNum = gChannelToSDIInVPIDBRegNum[mChannel];			break;
		
		case kRegInputStatus:				regNum = gChannelToSDIInputStatusRegNum[mChannel];
											if (regMask == kRegMaskInput1FrameRate)					regMask = gChannelToSDIInputRateMask[mChannel];
											else if (regMask == kRegMaskInput2FrameRate)			regMask = gChannelToSDIInputRateMask[mChannel+1];
											else if (regMask == kRegMaskInput1FrameRateHigh)		regMask = gChannelToSDIInputRateHighMask[mChannel];
											else if (regMask == kRegMaskInput2FrameRateHigh)		regMask = gChannelToSDIInputRateHighMask[mChannel+1];
											else if (regMask == kRegMaskInput1Progressive)			regMask = gChannelToSDIInputProgressiveMask[mChannel];
											else if (regMask == kRegMaskInput2Progressive)			regMask = gChannelToSDIInputProgressiveMask[mChannel+1];
											if (regShift == kRegShiftInput1FrameRate)				regShift = gChannelToSDIInputRateShift[mChannel];
											else if (regShift == kRegShiftInput2FrameRate)			regShift = gChannelToSDIInputRateShift[mChannel+1];
											else if (regShift == kRegShiftInput1FrameRateHigh)		regShift = gChannelToSDIInputRateHighShift[mChannel];
											else if (regShift == kRegShiftInput2FrameRateHigh)		regShift = gChannelToSDIInputRateHighShift[mChannel+1];
											else if (regShift == kRegShiftInput1Progressive)		regShift = gChannelToSDIInputProgressiveShift[mChannel];
											else if (regShift == kRegShiftInput2Progressive)		regShift = gChannelToSDIInputProgressiveShift[mChannel+1];
											break;
		case kRegSDIInput3GStatus:			regNum = gChannelToSDIInput3GStatusRegNum[mChannel];
											if (regMask == kRegMaskSDIIn3GbpsSMPTELevelBMode)			regMask = gChannelToSDIIn3GbModeMask[mChannel];
											else if (regMask == kRegMaskSDIIn23GbpsSMPTELevelBMode)		regMask = gChannelToSDIIn3GbModeMask[mChannel+1];
											else if (regMask == kRegMaskSDIIn3GbpsMode)					regMask = gChannelToSDIIn3GModeMask[mChannel];
											else if (regMask == kRegMaskSDIIn23GbpsMode)				regMask = gChannelToSDIIn3GModeMask[mChannel+1];
											else if (regMask == kRegMaskSDIInVPIDLinkAValid)			regMask = gChannelToSDIInVPIDLinkAValidMask[mChannel];
 											// Caller did not use a mask, pass along all relevant data to mChannel, shifted back to channel 1
											else if (regMask == 0xFFFFFFFF) {
												if (regNum == kRegSDIInput3GStatus || regNum == kRegSDIInput3GStatus2 )
												{
													regMask =  (mChannel % 2 == 0 ) ? 0xFFFFFFFF : 0xFF00;
													regShift =  (mChannel % 2 == 0 ) ? 0 : 8;
												}
												else if (regNum == kRegSDI5678Input3GStatus)
												{
													regMask =  0x000000FF << (mChannel - 4) * 8;
													regShift =  8 * (mChannel-4);
												}
											}

											if( regMask == kRegMaskSDIIn3GbpsSMPTELevelBMode ||  regMask == kRegMaskSDIIn23GbpsSMPTELevelBMode ||
												regMask == kRegMaskSDIIn3GbpsMode ||  regMask == kRegMaskSDIIn23GbpsMode)
											{
												if (regShift == kRegShiftSDIIn3GbpsSMPTELevelBMode)			regShift = gChannelToSDIIn3GbModeShift[mChannel];
												else if (regShift == kRegShiftSDIIn23GbpsSMPTELevelBMode)	regShift = gChannelToSDIIn3GbModeShift[mChannel+1];
												else if (regShift == kRegShiftSDIIn3GbpsMode)				regShift = gChannelToSDIIn3GModeShift[mChannel];
												else if (regShift == kRegShiftSDIIn23GbpsMode)				regShift = gChannelToSDIIn3GModeShift[mChannel+1];
											}
											break;

		default:	break;
	}
	return mCard.ReadRegister(regNum, outValue, regMask, regShift);
}	//	NTV2ReadRegisterRemote

bool NTV2VirtualDev::NTV2WriteRegisterRemote (const ULWord inRegNum, const ULWord inRegVal, const ULWord inRegMask, const ULWord inRegShift)
{
	if (isMyAncExtRegister(inRegNum))
		return HandleWriteAncExt(inRegNum, inRegVal, inRegMask, inRegShift);	//	Handle all anc ext regs
	if (isMyAncInsRegister(inRegNum))
		return HandleWriteAncIns(inRegNum, inRegVal, inRegMask, inRegShift);	//	Handle all anc ins regs
	if (isMyXptSelectRegister(inRegNum))
		return HandleWriteXptSelectReg(inRegNum, inRegVal, inRegMask, inRegShift);
	ULWord regNum(inRegNum), regMask(inRegMask), regShift(inRegShift);
	switch (regNum)
	{
		case kRegAud1Control:				regNum = gAudioSystemToAudioControlRegNum[KonaToCardAudSys(NTV2_AUDIOSYSTEM_1)];	break;
		case kRegAud1SourceSelect:			regNum = gAudioSystemToSrcSelectRegNum[KonaToCardAudSys(NTV2_AUDIOSYSTEM_1)];		break;
		case kRegAud1OutputLastAddr:		regNum = gChannelToAudioOutLastAddrRegNum[KonaToCardAudSys(NTV2_AUDIOSYSTEM_1)];	break;
		case kRegAud1InputLastAddr:			regNum = gChannelToAudioInLastAddrRegNum[KonaToCardAudSys(NTV2_AUDIOSYSTEM_1)];		break;
		case kRegAud1Delay:					regNum = gAudioDelayRegisterNumbers[KonaToCardAudSys(NTV2_AUDIOSYSTEM_1)];			break;
		case kRegAud2Control:				regNum = gAudioSystemToAudioControlRegNum[KonaToCardAudSys(NTV2_AUDIOSYSTEM_2)];	break;
		case kRegAud2SourceSelect:			regNum = gAudioSystemToSrcSelectRegNum[KonaToCardAudSys(NTV2_AUDIOSYSTEM_2)];		break;
		case kRegAud2OutputLastAddr:		regNum = gChannelToAudioOutLastAddrRegNum[KonaToCardAudSys(NTV2_AUDIOSYSTEM_2)];	break;
		case kRegAud2InputLastAddr:			regNum = gChannelToAudioInLastAddrRegNum[KonaToCardAudSys(NTV2_AUDIOSYSTEM_2)];		break;
		case kRegAud2Delay:					regNum = gAudioDelayRegisterNumbers[KonaToCardAudSys(NTV2_AUDIOSYSTEM_2)];			break;

		case kRegPCMControl4321:
		{	NTV2AudioSystem cardAudSys(KonaToCardAudSys(NTV2_AUDIOSYSTEM_1));
			if (cardAudSys == NTV2_AUDIOSYSTEM_1)
				return mCard.WriteRegister(regNum, inRegVal, regMask, regShift);	//	No mapping necessary
			if (inRegMask == kRegMaskPCMControlA1P1_2  &&  inRegShift == kRegShiftPCMControlA1P1_2)
				return mCard.SetAudioPCMControl(cardAudSys, NTV2_AudioChannel1_2, inRegVal ? true : false);
			if (inRegMask == kRegMaskPCMControlA1P3_4  &&  inRegShift == kRegShiftPCMControlA1P3_4)
				return mCard.SetAudioPCMControl(cardAudSys, NTV2_AudioChannel3_4, inRegVal ? true : false);
			if (inRegMask == kRegMaskPCMControlA1P5_6  &&  inRegShift == kRegShiftPCMControlA1P5_6)
				return mCard.SetAudioPCMControl(cardAudSys, NTV2_AudioChannel5_6, inRegVal ? true : false);
			if (inRegMask == kRegMaskPCMControlA1P7_8  &&  inRegShift == kRegShiftPCMControlA1P7_8)
				return mCard.SetAudioPCMControl(cardAudSys, NTV2_AudioChannel7_8, inRegVal ? true : false);
			if (inRegMask == kRegMaskPCMControlA1P9_10  &&  inRegShift == kRegShiftPCMControlA1P9_10)
				return mCard.SetAudioPCMControl(cardAudSys, NTV2_AudioChannel9_10, inRegVal ? true : false);
			if (inRegMask == kRegMaskPCMControlA1P11_12  &&  inRegShift == kRegShiftPCMControlA1P11_12)
				return mCard.SetAudioPCMControl(cardAudSys, NTV2_AudioChannel11_12, inRegVal ? true : false);
			if (inRegMask == kRegMaskPCMControlA1P13_14  &&  inRegShift == kRegShiftPCMControlA1P13_14)
				return mCard.SetAudioPCMControl(cardAudSys, NTV2_AudioChannel13_14, inRegVal ? true : false);
			if (inRegMask == kRegMaskPCMControlA1P15_16  &&  inRegShift == kRegShiftPCMControlA1P15_16)
				return mCard.SetAudioPCMControl(cardAudSys, NTV2_AudioChannel15_16, inRegVal ? true : false);

			cardAudSys = NTV2AudioSystem(cardAudSys+1);
			if (inRegMask == kRegMaskPCMControlA2P1_2  &&  inRegShift == kRegShiftPCMControlA2P1_2)
				return mCard.SetAudioPCMControl(cardAudSys, NTV2_AudioChannel1_2, inRegVal ? true : false);
			if (inRegMask == kRegMaskPCMControlA2P3_4  &&  inRegShift == kRegShiftPCMControlA2P3_4)
				return mCard.SetAudioPCMControl(cardAudSys, NTV2_AudioChannel3_4, inRegVal ? true : false);
			if (inRegMask == kRegMaskPCMControlA2P5_6  &&  inRegShift == kRegShiftPCMControlA2P5_6)
				return mCard.SetAudioPCMControl(cardAudSys, NTV2_AudioChannel5_6, inRegVal ? true : false);
			if (inRegMask == kRegMaskPCMControlA2P7_8  &&  inRegShift == kRegShiftPCMControlA2P7_8)
				return mCard.SetAudioPCMControl(cardAudSys, NTV2_AudioChannel7_8, inRegVal ? true : false);
			if (inRegMask == kRegMaskPCMControlA2P9_10  &&  inRegShift == kRegShiftPCMControlA2P9_10)
				return mCard.SetAudioPCMControl(cardAudSys, NTV2_AudioChannel9_10, inRegVal ? true : false);
			if (inRegMask == kRegMaskPCMControlA2P11_12  &&  inRegShift == kRegShiftPCMControlA2P11_12)
				return mCard.SetAudioPCMControl(cardAudSys, NTV2_AudioChannel11_12, inRegVal ? true : false);
			if (inRegMask == kRegMaskPCMControlA2P13_14  &&  inRegShift == kRegShiftPCMControlA2P13_14)
				return mCard.SetAudioPCMControl(cardAudSys, NTV2_AudioChannel13_14, inRegVal ? true : false);
			if (inRegMask == kRegMaskPCMControlA2P15_16  &&  inRegShift == kRegShiftPCMControlA2P15_16)
				return mCard.SetAudioPCMControl(cardAudSys, NTV2_AudioChannel15_16, inRegVal ? true : false);
			return false;
		}

		case kRegSDITransmitControl:	return HandleWriteSDITransmitControl(inRegNum, inRegVal, inRegMask, inRegShift);
		case kRegVidProc1Control:	regNum = gIndexToVidProcControlRegNum[VDevToCardMixer(NTV2_CHANNEL1)];		break;
		case kRegMixer1Coefficient:	regNum = gIndexToVidProcMixCoeffRegNum[VDevToCardMixer(NTV2_CHANNEL1)];		break;
		case kRegFlatMatteValue:	regNum = gIndexToVidProcFlatMatteRegNum[VDevToCardMixer(NTV2_CHANNEL1)];	break;

		case kRegCh1Control:		regNum = gChannelToControlRegNum[mChannel];		break;
		case kRegCh2Control:		regNum = gChannelToControlRegNum[mChannel+1];	break;

//		case kRegGlobalControl:
//		case kRegGlobalControl2:	return HandleWriteGlobalControl(regNum, inRegVal, regMask, regShift);

		case kRegSDIOut1Control:
		{
			ULWord regVal ((inRegVal << regShift) & regMask);
			regNum = gChannelToSDIOutControlRegNum[mChannel+1];
			if ((regMask & (BIT(18)|BIT(28)|BIT(30))) == (BIT(18)|BIT(28)|BIT(30)))	//	DS1 bits being set?
			{	//	DS1 audio system:
				//	Bits 18|28|30 have DS1 AudioSystem;	Bits 19|29|31 have DS2 AudioSystem
				//	Have to translate the AudioSystem values:
				NTV2AudioSystem cardAudSys(NTV2_AUDIOSYSTEM_INVALID), konaAudSys(NTV2_AUDIOSYSTEM_INVALID);
				ULWord newBits(0);
				konaAudSys = NTV2AudioSystem( (regVal & BIT(18) ? 4 : 0)
											+ (regVal & BIT(28) ? 2 : 0)
											+ (regVal & BIT(30) ? 1 : 0));
				if (HasKonaAudSys(konaAudSys))
				{
					cardAudSys = KonaToCardAudSys(cardAudSys);
					regVal &= 0xFFFFFFFF - BIT(30) - BIT(28) - BIT(18);	//	Clear those bits
					newBits = (cardAudSys & 4 ? BIT(18) : 0)
								| (cardAudSys & 2 ? BIT(28) : 0)
								| (cardAudSys & 1 ? BIT(30) : 0);
					regVal |= newBits;	//	Set those bits
				}
			}
			if ((regMask & (BIT(19)|BIT(29)|BIT(31))) == (BIT(19)|BIT(29)|BIT(31)))	//	DS2 bits being set?
			{	//	DS2 audio system:
				NTV2AudioSystem cardAudSys(NTV2_AUDIOSYSTEM_INVALID), konaAudSys(NTV2_AUDIOSYSTEM_INVALID);
				ULWord newBits(0);
				konaAudSys = NTV2AudioSystem( (regVal & BIT(19) ? 4 : 0)
											+ (regVal & BIT(29) ? 2 : 0)
											+ (regVal & BIT(31) ? 1 : 0));
				if (HasKonaAudSys(konaAudSys))
				{
					cardAudSys = KonaToCardAudSys(konaAudSys);
					regVal &= 0xFFFFFFFF - BIT(31) - BIT(29) - BIT(19);	//	Clear those bits
					newBits = (cardAudSys & 4 ? BIT(19) : 0)
								| (cardAudSys & 2 ? BIT(29) : 0)
								| (cardAudSys & 1 ? BIT(31) : 0);
					regVal |= newBits;	//	Set those bits
				}
			}
			return mCard.WriteRegister(regNum, regVal);
		}
		break;

		default:	break;
	}
	return mCard.WriteRegister(regNum, inRegVal, regMask, regShift);
}	//	NTV2WriteRegisterRemote

bool NTV2VirtualDev::NTV2AutoCirculateRemote (AUTOCIRCULATE_DATA & acData)
{
	acData.channelSpec = KonaToCardACXpt(acData.channelSpec);
	if (acData.eCommand == eInitAutoCirc)
		acData.lVal3 = KonaToCardAudSys(NTV2AudioSystem(acData.lVal3));

	bool result = mCard.AutoCirculate(acData);

	if (acData.eCommand == eInitAutoCirc)
		acData.lVal3 = CardToKonaAudSys(NTV2AudioSystem(acData.lVal3));
	acData.channelSpec = CardToKonaACXpt(acData.channelSpec);
	return result;
}

bool NTV2VirtualDev::NTV2WaitForInterruptRemote (const INTERRUPT_ENUMS eInterrupt, const ULWord timeOutMs)
{
	return mCard.WaitForInterrupt(VDevToCardInterrupt(eInterrupt), timeOutMs);
}

bool NTV2VirtualDev::NTV2DMATransferRemote (const NTV2DMAEngine inDMAEngine,		const bool inRead,
										const ULWord inFrameNum,			NTV2Buffer & inOutBuffer,
										const ULWord inCardOffsetBytes,		const ULWord inNumSegments,
										const ULWord inSegmentHostPitch,	const ULWord inSegmentCardPitch,
										const bool inSync)
{
	ULWord frameNum(inFrameNum), cardOffsetBytes(inCardOffsetBytes);
	DATKonaToCardFrmOffset(frameNum, cardOffsetBytes);
	if (!frameNum)
	{
		frameNum = cardOffsetBytes / 0x800000;
		cardOffsetBytes -= frameNum * 0x800000;
	}
	return mCard.DmaTransfer (inDMAEngine, inRead, frameNum, inOutBuffer, cardOffsetBytes,
								inOutBuffer.GetByteCount(), inNumSegments, inSegmentHostPitch,
								inSegmentCardPitch, inSync);
}

bool NTV2VirtualDev::NTV2MessageRemote (NTV2_HEADER * pMsg)
{
	if (!pMsg)
		return false;
	//	To simplify things, turn GETREGS/SETREGS messages into individual ReadRegister/WriteRegister calls
	//	so that we have one-stop-shopping for register reads/writes in NTV2ReadRegisterRemote & NTV2WriteRegisterRemote
	if (pMsg->GetType() == NTV2_TYPE_SETREGS)
		return false;	//	force CNTV2DriverInterface::ReadRegisters to call ReadRegister one-at-a-time
	if (pMsg->GetType() == NTV2_TYPE_GETREGS)
		return false;	//	force CNTV2Card::WriteRegisters to call WriteRegister one-at-a-time
	if (pMsg->GetType() == NTV2_TYPE_ACSTATUS)		//	AUTOCIRCULATE_STATUS
	{
		AUTOCIRCULATE_STATUS * pStatus = reinterpret_cast<AUTOCIRCULATE_STATUS*>(pMsg);
		pStatus->acCrosspoint = KonaToCardACXpt(pStatus->acCrosspoint);
		bool ok(mCard.NTV2Message(pMsg));
		pStatus->acCrosspoint = CardToKonaACXpt(pStatus->acCrosspoint);
		pStatus->acAudioSystem = CardToKonaAudSys(pStatus->acAudioSystem);
		return ok;
	}
	if (pMsg->GetType() == NTV2_TYPE_ACXFER)
	{
		AUTOCIRCULATE_TRANSFER * pXfer = reinterpret_cast<AUTOCIRCULATE_TRANSFER*>(pMsg);
		pXfer->acCrosspoint = KonaToCardACXpt(pXfer->acCrosspoint);
		bool ok(mCard.NTV2Message(pMsg));
		pXfer->acCrosspoint = CardToKonaACXpt(pXfer->acCrosspoint);
		return ok;
	}
//	if (pMsg->GetType() == NTV2_TYPE_ACXFERSTATUS)	//	AUTOCIRCULATE_TRANSFER_STATUS
	if (pMsg->GetType() == NTV2_TYPE_ACFRAMESTAMP)	//	FRAME_STAMP
	{
		FRAME_STAMP * pFrmStmp = reinterpret_cast<FRAME_STAMP*>(pMsg);
		pFrmStmp->acFrameTime = LWord64(VDevToCardChannel(NTV2Channel(pFrmStmp->acFrameTime)));
		bool ok(mCard.NTV2Message(pMsg));
		pFrmStmp->acFrameTime = LWord64(CardToVDevChannel(NTV2Channel(pFrmStmp->acFrameTime)));
		return ok;
	}
	return mCard.NTV2Message(pMsg);
}