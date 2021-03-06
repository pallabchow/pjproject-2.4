#include <iostream>
#include "StegSuit.h"
#include "StegiLBC.h"
#include <pj/os.h>

#include <pj/string.h>

#include <pj/log.h>
#include <pjmedia/alaw_ulaw.h>
#include <pjmedia-codec/types.h>
#include <pjmedia/types.h>

iLBC_Enc_Inst_t Enc_Inst;
iLBC_Dec_Inst_t Dec_Inst;
UINT mode20_30;

#define THIS_FILE			"StegSuit.cpp"
void CStegSuit::lock_init(pj_pool_t * pool)
{
	this->pool = pool;
	pj_lock_t *tLock;

	pj_status_t s = pj_lock_create_simple_mutex(this->pool, "steglock", &tLock);
	if (s != PJ_SUCCESS)
	{
		return;
	}

	if (SLock)
	{
		pj_lock_destroy(SLock);
	}

	SLock = tLock;
}

void CStegSuit::lock()
{
	pj_lock_acquire(SLock);
}

void CStegSuit::unlock()
{
	pj_lock_release(SLock);
}

void  on_pager_wrapper1(pj_str_t *from, pj_str_t* to, pj_str_t *body, pj_str_t* mimetype);

void CStegSuit::Create(pj_pool_t * pool)
{
	quit_flag = 0;

	bMessageArrived = false;
	bFileArrived = false;
	bFileSent = false;

	SLock = NULL;
	lock_init(pool);

	initEncode(&Enc_Inst, mode20_30);
	initDecode(&Dec_Inst, mode20_30, 1);

	SD[0].Storage = SD[1].Storage = RC[0].Storage = RC[1].Storage = NULL;
	SD[0].Cursor = SD[1].Cursor = RC[0].Cursor = RC[1].Cursor = NULL;
	SD[0].Length = SD[1].Length = RC[0].Length = RC[1].Length = 0;

	m_SEQ = 0;
	m_LastRSEQ = 0;
	m_LastRANN = 1;
	m_ActualByte = 0;

	for (int i = 0; i<COUNT_WINDOW; i++)
	{
		m_Window[i].Length = 0;  m_Window[i].Time = 0;  m_Window[i].Frame = NULL;
	}
	for (int i = 0; i<COUNT_CACHE; i++)
	{
		m_Cache[i].Length = 0;  m_Cache[i].Time = 0;  m_Cache[i].Frame = NULL;
	}

	m_Crt.Frame = NULL; m_Crt.Length = 0; m_Crt.Time = 0;
	m_Rcv.Frame = NULL; m_Rcv.Length = 0; m_Rcv.Time = 0;
	m_Resend.Frame = NULL; m_Resend.Frame = 0; m_Resend.Time = 0; //重传
	m_retranstep = 0;	//重传步骤

	m_FrmSLength = 0; m_RTPSeq = 0;
	m_FrmS = NULL;  m_FrmR = NULL;
	m_FrmSCursor = NULL;  m_FrmRCursor = NULL;

	m_pRTP = (CStegLSB *)(new CStegRTP());

	CarrierType = 97;
	m_nSegment = 1;
	LThreshold = 40;
	HThreshold = 1000;

	m_Threshold = LThreshold;

	MakeCheckTable();
	PJ_LOG(4, (THIS_FILE, "Create StegSuit once time!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!"));
//	count_resend = 0;
//	count_window_full = 0;
	return;
}

void CStegSuit::Allocate(unsigned int pt)
{
	if (pt == PJMEDIA_RTP_PT_PCMA_SMALL)
	{
		maxSAE = g711_SAEDU_SMALL + m_pRTP->GetParam(1);
	}
	else if (pt == PJMEDIA_RTP_PT_PCMA || pt == PJMEDIA_RTP_PT_PCMU) {
		maxSAE = g711_SAEDU + m_pRTP->GetParam(1);
	}
	else if (pt == PJMEDIA_RTP_PT_ILBC) {
		if (mode20_30 == 20)
		{
			maxSAE = iLBC_SAEDU_20 + m_pRTP->GetParam(1);
		}
		else {
			maxSAE = iLBC_SAEDU_30 + m_pRTP->GetParam(1);
		}
	}
	maxSTM = maxSAE * m_nSegment;
	SIADU = (maxSTM - m_pRTP->GetParam(1) * m_nSegment) * COUNT_WINDOW_CACHE * 2;
	if (SIADU < MAX_PATH * 2)
	{
		SIADU = MAX_PATH * 2;
	}

	Harves = SIADU / 2;

	SD[0].Storage = new BYTE[SIADU]; memset(SD[0].Storage, 0, SIADU);  SD[0].Cursor = SD[0].Storage;
	SD[1].Storage = new BYTE[SIADU]; memset(SD[1].Storage, 0, SIADU);  SD[1].Cursor = SD[1].Storage;
	RC[0].Storage = new BYTE[SIADU]; memset(RC[0].Storage, 0, SIADU);  RC[0].Cursor = RC[0].Storage;
	RC[1].Storage = new BYTE[SIADU]; memset(RC[1].Storage, 0, SIADU);  RC[1].Cursor = RC[1].Storage;

	for (int i = 0; i<COUNT_WINDOW; i++)
	{
		m_Window[i].Frame = new BYTE[maxSTM];
	}
	for (int i = 0; i<COUNT_CACHE; i++)
	{
		m_Cache[i].Frame = new BYTE[maxSTM];
	}
	m_Crt.Frame = new BYTE[maxSTM];
	m_Rcv.Frame = new BYTE[maxSTM];
	m_Resend.Frame = new BYTE[maxSTM];		//重传

	m_FrmS = new BYTE[maxSTM];
	m_FrmSCursor = m_FrmS;		//发送指针
	m_FrmR = new BYTE[maxSTM + maxSAE];
	m_FrmRCursor = m_FrmR;		//接收指针
	memset(m_FrmR, 0, maxSTM + maxSAE);

	m_pRTP->InitPosBook();
}

void CStegSuit::Configure()
{
	bFileSent = false;
	bFileArrived = false;
	if (quit_flag==0)
	{
		PJ_LOG(4, (THIS_FILE, "Configure(): reset RC[1]!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!"));
		//	memset((void *)SD[0].Storage, 0, SIADU); SD[0].Cursor = SD[0].Storage;	SD[0].Length = 0;
		memset((void *)SD[1].Storage, 0, SIADU); SD[1].Cursor = SD[1].Storage;	SD[1].Length = 0;
		//	memset((void *)RC[0].Storage, 0, SIADU); RC[0].Cursor = RC[0].Storage;	RC[0].Length = 0;
		memset((void *)RC[1].Storage, 0, SIADU); RC[1].Cursor = RC[1].Storage;	RC[1].Length = 0;
		for (int i = 0; i<COUNT_WINDOW; i++)
		{
			m_Window[i].Length = 0;
			m_Window[i].Time = 0;
		}
		for (int i = 0; i<COUNT_CACHE; i++)
		{
			m_Cache[i].Length = 0;
			m_Cache[i].Time = 0;
		}
	}
}

void CStegSuit::Clean()
{
	PJ_LOG(4, (THIS_FILE, "Clean():start clean quit flag=%d", quit_flag));
	quit_flag = 1;
	if (SD[0].Storage) delete[] SD[0].Storage;  SD[0].Storage = NULL;
	if (SD[1].Storage) delete[] SD[1].Storage;  SD[1].Storage = NULL;
	if (RC[0].Storage) delete[] RC[0].Storage;  RC[0].Storage = NULL;
	if (RC[1].Storage) delete[] RC[1].Storage;  RC[1].Storage = NULL;
	for (int i = 0; i<COUNT_WINDOW; i++)
	{
		if (m_Window[i].Frame != NULL) delete[] m_Window[i].Frame;  m_Window[i].Frame = NULL;
	}
	for (int i = 0; i<COUNT_CACHE; i++)
	{
		if (m_Cache[i].Frame != NULL) delete[] m_Cache[i].Frame;  m_Cache[i].Frame = NULL;
	}
	if (m_Crt.Frame != NULL) delete[] m_Crt.Frame;  m_Crt.Frame = NULL;
	if (m_Rcv.Frame != NULL) delete[] m_Rcv.Frame;  m_Rcv.Frame = NULL;
	if (m_FrmS != NULL) delete[] m_FrmS;  m_FrmS = NULL;
	if (m_FrmR != NULL) delete[] m_FrmR;  m_FrmR = NULL;

	if (m_Resend.Frame != NULL) delete[] m_Resend.Frame; m_Resend.Frame = NULL;	//重传
	if (thread != NULL) // air
	{
		pj_thread_join(thread);
		pj_thread_destroy(thread);
	}

	thread = NULL;
	if (SLock)
	{
		SLock = NULL;
	}
	PJ_LOG(4, (THIS_FILE, "Clean():finish clean success!"));
	delete m_pRTP;
}

void CStegSuit::Control(unsigned int pt, UINT Command)
{
	//一个RTP包中共嵌入字节数
	if (pt== PJMEDIA_RTP_PT_PCMA_SMALL)
	{
		SAEDU = g711_SAEDU_SMALL;
	}
	else if (pt== PJMEDIA_RTP_PT_PCMA|| pt==PJMEDIA_RTP_PT_PCMU) {
		SAEDU = g711_SAEDU;
	}
	else if (pt== PJMEDIA_RTP_PT_ILBC) {
		if (mode20_30==20)
		{
			SAEDU = iLBC_SAEDU_20;
		}
		else {
			SAEDU = iLBC_SAEDU_30;
		}
	}
	

	STMDU = m_nSegment * (SAEDU + m_pRTP->GetParam(1));			//.ini中设置m_nSegment = 1, STM包头3字节 (24bit)
}

//0至255的数，在二进制表示下，每位相加的奇偶性;数组用于奇偶校验
void CStegSuit::MakeCheckTable()
{
	for (int i = 0; i<256; i++)
	{
		int n = 0;
		for (int k = 0; k<8; k++)
			n = n + ((i >> k) & 0x1);
		m_CheckTable[i] = n % 2;
	}
}

//将界面信息和文件缓存至隐蔽消息应用层缓冲区(SD)
UINT CStegSuit::Send(void * pSrc, int length, int type)
{
	if (((UINT)length > SIADU) || (type != 1 && type != 2) || !pSrc) return 0;
	if (type == 1)
	{
		memset(SD[0].Storage, 0, SIADU);
		memcpy(SD[0].Storage, pSrc, length);
		SD[0].Length = length;
		SD[0].Cursor = SD[0].Storage;
	}
	else
	{
		memcpy(SD[1].Storage, pSrc, length);
		SD[1].Cursor = SD[1].Storage;
		SD[1].Length = length;
	}
	return 1;
}
//接收
UINT CStegSuit::Receive(void * pDst, int maxlength, int type)
{
	if (type == 1)
	{
		for (UINT i = 0; i<SIADU - 1; i += 1)
		{
			if (RC[0].Storage[i] == 0)
			{
				strcpy((char *)pDst, (char *)RC[0].Storage);
				memset((void *)RC[0].Storage, 0, SIADU);
				RC[0].Cursor = RC[0].Storage;
				RC[0].Length = 0;
				return i;
			}
		}
		return 0;
	}

	if (type == 2)
	{
		if (maxlength == -1)
		{
//			PJ_LOG(4, (THIS_FILE, "Receive(): receive name: RC[1].length=%d", RC[1].Length));
			UINT begin = 0, end = 0;
			for (int cur = RC[1].Length-1; cur >= 0; --cur)
			{
				if (RC[1].Storage[cur] == '>' && RC[1].Storage[cur + 1] == '\0') {
					end = cur;
//					PJ_LOG(4, (THIS_FILE, "Receive():RC[1].length=%d, end=%d", RC[1].Length, end));
				}
				else if (end > 0 && RC[1].Storage[cur] == '<')
				{
					begin = cur;
//					PJ_LOG(4, (THIS_FILE, "Receive():after RC[1].length=%d, begin=%d, end=%d", RC[1].Length, begin, end));
					memcpy(pDst, RC[1].Storage + begin + 1, end - begin - 1);
					RC[1].Length = RC[1].Length - (end + 2);
					BYTE* swap = new BYTE[SIADU];
					memcpy(swap, RC[1].Storage + end + 2, RC[1].Length);
					memcpy(RC[1].Storage, swap, RC[1].Length);
					RC[1].Cursor = &RC[1].Storage[RC[1].Length];
					delete[] swap;
					return 1;
				}
			}
//			PJ_LOG(4, (THIS_FILE, "Receive(): receive name after for: RC[1].length=%d， begin=%d, end=%d.", RC[1].Length, begin, end));
			if (end == 0 && begin==end && RC[1].Length>0)
			{
				//Clean Storage
				memset((void *)RC[1].Storage, 0, SIADU);
				RC[1].Cursor = RC[1].Storage;
				RC[1].Length = 0;
				return 1234;
			}
		}
		if (maxlength == -2 && RC[1].Length >= 4)
		{
			memcpy(pDst, RC[1].Storage, 4);
			RC[1].Length = RC[1].Length - 4;
			BYTE* swap = new BYTE[RC[1].Length];
			memcpy(swap, RC[1].Storage + 4, RC[1].Length);
			memcpy(RC[1].Storage, swap, RC[1].Length);
			RC[1].Cursor = &RC[1].Storage[RC[1].Length];
			delete[] swap;
			return 2;
		}
		//TODO:2018.08.17截止
		//接收方没问题，因为一直没有新的数据接收了。所以问题在于发送方，提前停止了发送？
//		PJ_LOG(4, (THIS_FILE, "Receive():maxLength=%d, rc.length=%d, Harves=%d, storage=%s", maxlength, RC[1].Length, Harves, RC[1].Storage));
		if (((UINT)maxlength >= Harves && RC[1].Length >= Harves)
			|| ((UINT)maxlength < Harves && maxlength >0))
		{
			UINT ret = RC[1].Length;
			memcpy(pDst, RC[1].Storage, RC[1].Length);
			memset((void *)RC[1].Storage, 0, SIADU);
			RC[1].Cursor = RC[1].Storage;
			RC[1].Length = 0;
			return ret;
		}
		return 0;
	}

	return 0;
}

//接收方检验收到包是否在待接收窗口内
//检验Seq与LastRSEQ是否符合，即Seq-LastRSEQ=0~7,Seq为当前帧序号，LastRSEQ为上次处理的帧序号
bool CStegSuit::Between(UINT Seq, UINT LastRSEQ)
{
	bool Hit = false;
	UINT Cnt = 0;
	while (!Hit)
	{
		if (Seq == (LastRSEQ + 1) % 16) Hit = true;
		LastRSEQ++;
		Cnt++;
		if (Cnt == COUNT_CACHE) break;
	}
	return Hit;
}

//发送方检验对方是否收到这个包
bool CStegSuit::Inside(UINT Seq, UINT LastRANN)
{
	UINT Cnt = 0;
	bool Hit = false;
	while (!Hit)
	{
		if (Seq == (LastRANN + 16 - 1) % 16)
		{
			Hit = true;
		}
		LastRANN = LastRANN + 16 - 1;
		Cnt++;
		if (Cnt == COUNT_WINDOW) break;
	}
	return Hit;
}

//RTP包，RTP包头长度，语音
UINT CStegSuit::Embedding(void * pCarrier, UINT RTPheadlen, pj_size_t dataLen, char* pPcmIn, UINT channel_pt)
{
	m_channel_pt_send = channel_pt;
	int datatype = 0;	//数据类型
	char* pPcm = pPcmIn;
	Control(m_channel_pt_send, this->m_Seclev);

	Retransmission(); //重传检测
	int status = STMSdata(&datatype);
	if (status == 1)
	{
//		PJ_LOG(4, (THIS_FILE, "Embedding:-----------------------It will re-send after STMdata! count=%d------------------------", count_resend));
		SAESdata(pCarrier, RTPheadlen, dataLen, pPcm);	//嵌入数据
		if (m_ActualByte == m_Resend.Length - m_pRTP->GetParam(1))	//重传成功，要求重传的字节数
		{
			delete[] m_Retrans.front().Frame;
			m_Retrans.front().Frame = NULL;
			m_Retrans.front().Length = 0;
			m_Retrans.pop();	//嵌入成功，丢弃
			STMSheader(datatype);	//组装STM包头
			SAESheader(pCarrier);	//嵌入STM包头

			memset(m_Resend.Frame, 0, maxSTM);
			m_Resend.Length = 0;
			m_Resend.Time = 0;
			return 2;
		}
	}
//	else if (status == 2)
//	{
//		PJ_LOG(4, (THIS_FILE, "Embedding:-----------------------It has full windows after STMdata! count=%d.",count_window_full));
//	}
//	else {
//		PJ_LOG(4, (THIS_FILE, "Embedding:-----------------------It work well!------------------------"));
//	}

	//重嵌入失败或正常发送
	memset(m_Resend.Frame, 0, maxSTM);
	m_Resend.Length = 0;
	m_Resend.Time = 0;

	SAESdata(pCarrier, RTPheadlen, dataLen, pPcm);	//嵌入数据
	STMSheader(datatype);	//组装STM包头,并修改SIA缓存
	SAESheader(pCarrier);	//嵌入STM包头

	m_retranstep = 0; //开启重传

	return 3;
}


UINT CStegSuit::Retransmission()
{
	//维护重传状态
	if (m_Threshold > HThreshold)
	{
		m_Threshold = HThreshold;
	}
	for (UINT i = 0; i < COUNT_WINDOW; i++)
		if (m_Window[i].Length != 0) m_Window[i].Time++;   // Add time

	if (m_Window[(m_LastRANN - 1) & (COUNT_WINDOW - 1)].Length != 0)
		m_Threshold = LThreshold;	//网络通畅

	for (UINT i = 0; i<COUNT_WINDOW; i++)
	{
		if (m_Window[i].Length != 0 && Inside(m_Window[i].Frame[2] >> 4, m_LastRANN))	//对方已处理，滑动窗口
		{
			memset(m_Window[i].Frame, 0, maxSTM);
			m_Window[i].Length = 0;
			m_Window[i].Time = 0;
		}
	}
	UINT delay = 0, pos = 0;
	for (int i = 0; i < COUNT_WINDOW; i++) // retransmit
	{
		if (m_Window[i].Time > delay)
		{
			pos = i;
			delay = m_Window[i].Time;		//求最大延迟的包
		}
	}
	if (m_Window[pos].Time > m_Threshold)		//重传
	{

		m_Threshold += LThreshold;		//加大时间门限

		if (m_Retrans.empty() == false)
		{
//			++count_resend;
			//			PJ_LOG(4, (THIS_FILE, "Retransmission:-----------------------delete retrans! retrans.size=%d, count=%d.------------------------", m_Retrans.size(), count_resend));
			//重传队列清空
			delete[] m_Retrans.front().Frame;
			m_Retrans.front().Frame = NULL;
			m_Retrans.front().Length = 0;
			m_Retrans.pop();
		}
		//重传,压入队列
		STMFrame ReSTM;
		ReSTM.Frame = new BYTE[STMDU];
		memcpy(ReSTM.Frame, m_Window[pos].Frame, STMDU);
		ReSTM.Length = m_Window[pos].Length;
		m_Retrans.push(ReSTM);			//压入队列
		return 1;
	}
	return 2;
}

//第一步
//返回：0：全局超时； 1：重传；2：发送窗口满； 3：正常 
UINT CStegSuit::STMSdata(int *datatype)		//向SIA申请数据
{
	//m_Crt为传给SAE层的STM包,初始化
	memset(m_Crt.Frame, 0, maxSTM);
	m_Crt.Length = 0;	//长度置零

	if (m_retranstep == 0 && m_Retrans.empty() == false)	//有重传
	{
	//	PJ_LOG(4, (THIS_FILE, "STMSdata(): if 1, need retranslate!"));
		m_retranstep = 1;
		memcpy(m_Resend.Frame, m_Retrans.front().Frame, STMDU);	//读一个重传包
		m_Resend.Length = m_Retrans.front().Length;
		memcpy(m_Crt.Frame, m_Resend.Frame, m_Resend.Length);	//将重传包形成STM帧
		m_Crt.Length = m_Resend.Length;
		return 1;
	}
	else if (m_Window[(m_SEQ + 1) % COUNT_WINDOW].Length != 0)	//发送窗口满禁止发送
	{
//		count_window_full++;
//		(4, (THIS_FILE, "STMSdata():if 2, windows full!"));
		//TODO: 2018\05\30 by BobMu: if windows full will ignore this frame or stop-wait?!
		return 2;  // Window full
	}
	else  //正常情况
	{
//		count_window_full = 0;
		//从SD拉一个最长为STMDU长的数据
		for (int i = 0; i < 2; ++i)
		{
			if (SD[i].Length != 0)
			{
				UINT len;	//帧载荷长度
				if (SD[i].Length >= STMDU - m_pRTP->GetParam(1))
				{
					len = STMDU - m_pRTP->GetParam(1);	//最大传输长度
				}
				else
				{
					len = SD[i].Length;
				}

				memcpy(&m_Crt.Frame[3], SD[i].Cursor, len);
				m_Crt.Length = len + m_pRTP->GetParam(1);
				*datatype = i;	//返回数据类型,0为消息，1为文件
				break;
			}
		}
		return 3;
	}
}

//第二步，SAE层嵌入数据
UINT CStegSuit::SAESdata(void * pCarrier, UINT RTPheadlen, pj_size_t dataLen, char* pPcmIn)
{
	memcpy(m_FrmS, m_Crt.Frame, STMDU);	//取STMDU长度（3+1byte）
	m_FrmSLength = m_Crt.Length;
	m_FrmSCursor = m_FrmS;
	m_ActualByte = 0;	//真实传输字节数

	if (m_FrmSLength > 0)	//待发送的STM帧数据非空
	{
		memcpy(m_chEmdSecMsg, m_Crt.Frame + m_pRTP->GetParam(1), m_Crt.Length - m_pRTP->GetParam(1));
		int bitpos[3] = { 0,6,4 };
		int hdTxt_pos[3] = { 0,9,19 };
		for (int i = 0; i < 1; ++i)
		{
			Enc_Inst.ste.bitpos = bitpos[i];
			Enc_Inst.ste.hdTxt_pos = hdTxt_pos[i];
			Encode((unsigned char *)(m_pFrmBuf + dataLen * i), (pPcmIn + g711_VOICE_LENTH * i), dataLen,
				1, m_Crt.Frame + m_pRTP->GetParam(1));
		}
		m_ActualByte = m_FrmSLength - m_pRTP->GetParam(1);
		m_FrmSLength = 0;
		//		PJ_LOG(4, (THIS_FILE, "-------------------------after encode m_ActualByte=%d", m_ActualByte));
	}
	else
	{
		//THZ: 长度调整为一帧的长度
		for (int i = 0; i < 1; ++i)
		{
			Encode((unsigned char *)(m_pFrmBuf + dataLen * i), (pPcmIn + g711_VOICE_LENTH * i), dataLen,
				0, NULL);
		}
		m_ActualByte = 0;
	}
	memcpy((char*)pCarrier + RTPheadlen, m_pFrmBuf, dataLen);
	return 1;
}

//第三步，STM组装包头域 修改SIA缓存
UINT CStegSuit::STMSheader(int datatype)
{
	UINT len = m_ActualByte;	//实际嵌入字节数
	int i = datatype;			//数据来源
								//STM数据包头部
	if (m_Resend.Length > 0)	//重传包
	{
		//重写一次
		memcpy(m_Crt.Frame, m_Resend.Frame, m_Resend.Length);	//将重传包形成STM帧
		m_Crt.Length = m_Resend.Length;
		m_Crt.Frame[2] = (m_Crt.Frame[2] & 0xF0) + ((m_LastRSEQ + 1) % 16);	//重传仅修改一个地方		
	}
	else	//新包
	{
		UINT odd = 0;

		//奇偶校验
		for (UINT k = 0; k < len; k++)
		{
			odd = odd + m_CheckTable[*(SD[i].Cursor + k)];
		}
		odd = odd % 2;

		if (len > 0)		//机密信息包
		{
			m_SEQ = (m_SEQ + 1) % 16;		//分段标号，数据太长可能会分片发送,SEQ范围[0,15]
			setLenToCursor(len, i, odd, m_SEQ, m_LastRSEQ, m_Crt.Frame);
			
			m_Crt.Length = len + m_pRTP->GetParam(1);		//对SAE层来说的数据大小，包括STM头

			SD[i].Length -= len;		//滑动窗口
			SD[i].Cursor += len;

			memcpy(m_Window[m_SEQ & (COUNT_WINDOW - 1)].Frame, m_Crt.Frame, STMDU);	//加入发送滑动窗口
			m_Window[m_SEQ & (COUNT_WINDOW - 1)].Length = m_Crt.Length;				//加入发送滑动窗口
			
			if (SD[i].Length == 0)				//隐秘信息应用层数据发送完毕
			{
				//应用层数据发送完毕，TODO：对接口层发送提示
				//AfxGetMainWnd()->PostMessage(WM_SIACLEAR, (UINT)(i + 1), 0);
				if (i==0)
				{
					this->bMessageSent = true;
				}
				if (i == 1)
				{
					this->bFileSent = true;
				}
			}
		}
		else	//空包，无机密信息
		{
			UINT seq = (m_LastRANN + 16 - 1) % 16;		//分段标号为对方接收窗口外
			setLenToCursor(len, i, odd, seq, m_LastRSEQ, m_Crt.Frame);
			m_Crt.Length = len + m_pRTP->GetParam(1);		//对SAE层来说的数据大小
		}

	}
	return 1;
}



//第四步，嵌入STM头域
UINT CStegSuit::SAESheader(void * pCarrier)
{
	memcpy(m_FrmS, m_Crt.Frame, m_pRTP->GetParam(1));	//取STMDU头域
	m_FrmSLength = m_pRTP->GetParam(1);
	m_FrmSCursor = m_FrmS;

	m_pRTP->PreparePosBook();

	m_pRTP->Embed(m_FrmSCursor, m_pRTP->GetParam(1), NULL, 0, (BYTE *)pCarrier);		//STM头域3字节填入RTP

	return 1;
}

//提取机密信息
UINT CStegSuit::Retriving(void *hdr, void * pCarrier, int pCarrierLength, char* pPcmOut, UINT channel_pt)
{
	m_channel_pt_receive = channel_pt;
	if (SAER(hdr, pCarrier, pCarrierLength, pPcmOut))		//提取 组成STM帧
	{
		STMR();
	}
	return 1;
}

//SAE层提取
UINT CStegSuit::SAER(void *hdr, void * pCarrier, int pCarrierLength, char* pPcmOut)
{
	//rtppacket

	BYTE *DstPacket = new BYTE[12];
	memcpy(DstPacket, hdr, 12);

	BYTE *DstData = new BYTE[pCarrierLength + 1];
	memcpy(DstData, pCarrier, pCarrierLength);

	m_pRTP->PreparePosBook();
	m_pRTP->Extract(m_FrmRCursor, m_pRTP->GetParam(1), NULL, 0, DstPacket);	//从RTP中获取STM头域

	UINT len = getLenFromCursor(m_FrmRCursor);

	if (len > 0)
	{
		//长度不正确，不是机密信息的包，丢弃
		if (len > SAEDU)
		{
			delete[] DstPacket;
			delete[] DstData;
			return 0;
		}
		int bitpos[3] = { 0,6,4 };
		int hdTxt_pos[3] = { 0,9,19 };
		for (int i = 0; i < 1; ++i)
		{
			Dec_Inst.ste.bitpos = bitpos[i];
			Dec_Inst.ste.hdTxt_pos = hdTxt_pos[i];
			Decode((pPcmOut + g711_VOICE_LENTH * i), (unsigned char *)(DstData + g711_FRAME_LENTH * i), pCarrierLength,
				1, 1, m_chRtrSecMsg);
		}
		//		PJ_LOG(4, (THIS_FILE, "SAER:msg=%s!", m_chRtrSecMsg));
		memcpy(m_FrmRCursor + m_pRTP->GetParam(1), (BYTE*)m_chRtrSecMsg, SAEDU);
	}
	else
	{
		for (int i = 0; i < 1; ++i)
		{
			Decode((pPcmOut + g711_VOICE_LENTH * i), (unsigned char *)(DstData + g711_FRAME_LENTH * i), pCarrierLength, 1, 0, NULL);
		}

	}
	delete[] DstPacket;
	delete[] DstData;
	return 1;
}

UINT CStegSuit::STMR()
{
	memcpy(m_Rcv.Frame, m_FrmR, STMDU);		//获取机密信息	m_FrmRCursor == m_FrmR;
	memset(m_FrmR, 0, maxSTM + maxSAE);

	//处理包头域
	UINT Syn = m_Rcv.Frame[0] & 0xE0;
	UINT Seq = getSeqFromCursor(m_Rcv.Frame);
	UINT len = getLenFromCursor(m_Rcv.Frame);
	UINT odd = 0;

	if ((Syn & 0xE0) == 0x40)		//防止接收到非机密信息包
	{
		if (len == 0)	//空包的作用是确认
			m_LastRANN = m_Rcv.Frame[2] & 0xF;		//为发送端起始序号
		else
		{
			//			PJ_LOG(4, (THIS_FILE, "frame[0] = %d, frame[1] = %d, len = %d!", m_Rcv.Frame[0], m_Rcv.Frame[1], len));
			for (UINT i = 0; i < len; i++)
			{
				odd = odd + m_CheckTable[m_Rcv.Frame[m_pRTP->GetParam(1) + i]];
			}
			if ((odd % 2) != ((m_Rcv.Frame[1] >> 7) & 0x1)) return 0;	//奇偶校验


																		//Seq与LastRSEQ符合就处理接收信息,Seq为当前帧序号，LastRSEQ为上次处理的帧序号
			if (Between(Seq, m_LastRSEQ))
			{
				//获取机密信息至接收滑动窗口，这里并不考虑顺序
				memcpy(m_Cache[Seq & (COUNT_CACHE - 1)].Frame, m_Rcv.Frame, STMDU);
				m_Cache[Seq & (COUNT_CACHE - 1)].Length = 1; // denote there is a valid frame
				m_LastRANN = m_Rcv.Frame[2] & 0xF;		//为发送端起始序号
			}	//其他为重发的包,确认号没有时效性

		}
	}

	//按序提交数据
	bool NewRequst1 = false, NewRequst2 = false;
	while (m_Cache[(m_LastRSEQ + 1) & (COUNT_CACHE - 1)].Length != 0)		//保证顺序
	{
		memcpy(m_Rcv.Frame, m_Cache[(m_LastRSEQ + 1) & (COUNT_CACHE - 1)].Frame, STMDU);	//按序处理一个数据
		UINT Seq = getSeqFromCursor(m_Rcv.Frame);
		UINT len = getLenFromCursor(m_Rcv.Frame);
		UINT type = getTypeFromCursor(m_Rcv.Frame);

		m_Cache[(m_LastRSEQ + 1) & (COUNT_CACHE - 1)].Length = 0;		//滑动窗口向前移动
		m_LastRSEQ = Seq;		//LastRSEQ表示己方已经处理的序号

		if (len != 0)	//按序提交
		{
			if (type == 1) NewRequst1 = true;
			if (type == 2) NewRequst2 = true;
//			PJ_LOG(4, (THIS_FILE, "STMR(): seq=%d, len=%d, taking Ann = %d, type=%d", Seq, len, m_LastRANN, type));
			if (RC[type - 1].Length > SIADU - len)
			{
				//缓存已满，丢弃旧数据等待重传，接收新数据
				memset(RC[type - 1].Storage, 0, SIADU);
				RC[type - 1].Cursor = RC[type - 1].Storage;
				RC[type - 1].Length = 0;
			}

			memcpy(RC[type - 1].Cursor, &m_Rcv.Frame[m_pRTP->GetParam(1)], len);
			RC[type - 1].Cursor += len;
			RC[type - 1].Length += len;
//			PJ_LOG(4, (THIS_FILE, "STMR():		after re copy, seq=%d, rc.length=%d, storage=%s", Seq, RC[type - 1].Length, RC[type - 1].Storage));
		}
	}

	if (NewRequst1)
	{
		this->bMessageArrived = true;
	}
	//By BobMu at 2018/08/16: receive small file is lost in steg thread
	if (NewRequst2 || RC[1].Length > 0)
	{
		this->bFileArrived = true;
	}
	return 1;
}

void CStegSuit::Encode(unsigned char *encoded_data, void *block, pj_size_t dataLen, short bHide, void *hdTxt)
{
	char *msg = (char *)hdTxt;
	pj_int16_t *samples = (pj_int16_t *)block;
	pj_uint8_t *dst = (pj_uint8_t *)encoded_data;
	if (m_channel_pt_send == PJMEDIA_RTP_PT_ILBC)
	{
		iLBCEncode(encoded_data, (float *)block, &Enc_Inst, bHide, msg);
	}
	else if (m_channel_pt_send == PJMEDIA_RTP_PT_PCMA || m_channel_pt_send == PJMEDIA_RTP_PT_PCMU|| m_channel_pt_send == PJMEDIA_RTP_PT_PCMA_SMALL)
	{
		if (bHide != 0)
		{
			dst = (pj_uint8_t *)encoded_data;
			int length = strlen(msg) + 1;
			for (size_t i = 0; i < length || i<dataLen; ++i, dst++)
			{
				*dst = msg[i];
			}
//			PJ_LOG(4, (THIS_FILE, "Encode: src=%d, dst=%d, hdTxt=%s, length=%d", (pj_int16_t *)block, *encoded_data, msg, length));
		}
		else {
			for (size_t i = 0; i < dataLen; ++i, ++dst)
			{
				*dst = pjmedia_linear2ulaw(samples[i]);
			}
		}
	}

}

void CStegSuit::Decode(void *decblock, unsigned char *bytes, int bytes_length, int mode, short bHide, char *msg)
{
	pj_uint8_t *src = (pj_uint8_t*)bytes;
	pj_uint16_t *dst;
	if (m_channel_pt_receive == PJMEDIA_RTP_PT_ILBC)
	{
		//ilbc
		iLBCDecode((float *)decblock, bytes, &Dec_Inst, mode, bHide, msg);
	}
	else if (m_channel_pt_receive == PJMEDIA_RTP_PT_PCMA || m_channel_pt_receive == PJMEDIA_RTP_PT_PCMU|| m_channel_pt_receive== PJMEDIA_RTP_PT_PCMA_SMALL)
	{
		//pcmu
		dst = (pj_uint16_t *)decblock;

		if (bHide != 0)
		{
			src = (pj_uint8_t*)bytes;
			char * buffer = msg;
			size_t i = 0;
			for (; ((char *)src)[i] != '\0' || i<bytes_length; ++buffer, ++i)
			{
				*buffer = ((char *)src)[i];
			}
			*buffer = ((char *)src)[i];
//			PJ_LOG(4, (THIS_FILE, "Decode:decoded block = %d, src byte = %d, msg=%s, index=%d!", (pj_uint16_t *)decblock, *bytes, msg, i));
		}
		else {
			for (size_t i = 0; i < bytes_length; ++i, ++dst)
			{
				*dst = (pj_uint16_t)pjmedia_ulaw2linear(src[i]);  //pcmu
			}
		}
	}
}

UINT CStegSuit::getSeqFromCursor(unsigned char* cursor) {
	return (cursor[2] >> 4) & 0xF;
}
UINT CStegSuit::getTypeFromCursor(unsigned char * cursor) {
	return (cursor[0] >> 3) & 0x3;
}
UINT CStegSuit::getLenFromCursor(unsigned char* cursor) {
	return (cursor[0] & 0x7) + ((cursor[1] & 0x7F) << 3);
}
void CStegSuit::setLenToCursor(unsigned int len, int type, unsigned int odd, unsigned int seq, unsigned int lastRSeq, unsigned char* &cursor) {
	cursor[0] = 0x40 + 8 * (type + 1) + (len & 0x7);
	cursor[1] = (len >> 3) + (odd << 7);
	cursor[2] = (seq << 4) + ((lastRSeq + 1) % 16);
}