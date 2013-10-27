#include <string.h>
#include <stdlib.h>
#include <endian.h>
#include <time.h>
#include <boost/bind.hpp>
#include <cryptopp/dh.h>
#include <cryptopp/secblock.h>
#include <cryptopp/dsa.h>
#include "base64.h"
#include "Log.h"
#include "CryptoConst.h"
#include "I2NPProtocol.h"
#include "RouterContext.h"
#include "Transports.h"
#include "NTCPSession.h"

using namespace i2p::crypto;

namespace i2p
{
namespace ntcp
{
	NTCPSession::NTCPSession (boost::asio::ip::tcp::socket& s, const i2p::data::RouterInfo * in_RemoteRouterInfo): 
		m_Socket (s), m_IsEstablished (false), m_ReceiveBufferOffset (0),
		m_NextMessage (nullptr), m_DelayedMessage (nullptr)
	{		
		if (in_RemoteRouterInfo)
			m_RemoteRouterInfo = *in_RemoteRouterInfo;
	}
	
	void NTCPSession::CreateAESKey (uint8_t * pubKey, uint8_t * aesKey)
	{
		CryptoPP::DH dh (elgp, elgg);
		CryptoPP::SecByteBlock secretKey(dh.AgreedValueLength());
		if (!dh.Agree (secretKey, i2p::context.GetPrivateKey (), pubKey))
		{    
		    LogPrint ("Couldn't create shared key");
			Terminate ();
			return;
		};

		if (secretKey[0] & 0x80)
		{
			aesKey[0] = 0;
			memcpy (aesKey + 1, secretKey, 31);
		}	
		else	
			memcpy (aesKey, secretKey, 32);
	}	

	void NTCPSession::Terminate ()
	{
		m_IsEstablished = false;
		m_Socket.close ();
		if (m_DelayedMessage)
			delete m_DelayedMessage;
		// TODO: notify tunnels
		i2p::transports.RemoveNTCPSession (this);
		delete this;
		LogPrint ("NTCP session terminated");
	}	

	void NTCPSession::Connected ()
	{
		LogPrint ("NTCP session connected");
		m_IsEstablished = true;
		i2p::transports.AddNTCPSession (this);

		SendTimeSyncMessage ();
		SendI2NPMessage (CreateDatabaseStoreMsg ()); // we tell immediately who we are		

		if (m_DelayedMessage)
		{
			i2p::I2NPMessage * delayedMessage = m_DelayedMessage;
			m_DelayedMessage = 0;
			SendI2NPMessage (delayedMessage);
		}	
	}	
		
	void NTCPSession::ClientLogin ()
	{
		// send Phase1
		const uint8_t * x = i2p::context.GetRouterIdentity ().publicKey;
		memcpy (m_Phase1.pubKey, x, 256);
		CryptoPP::SHA256().CalculateDigest(m_Phase1.HXxorHI, x, 256);
		const uint8_t * ident = m_RemoteRouterInfo.GetIdentHash ();
		for (int i = 0; i < 32; i++)
			m_Phase1.HXxorHI[i] ^= ident[i];
		
		boost::asio::async_write (m_Socket, boost::asio::buffer (&m_Phase1, sizeof (m_Phase1)), boost::asio::transfer_all (),
        	boost::bind(&NTCPSession::HandlePhase1Sent, this, boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred));
	}	

	void NTCPSession::ServerLogin ()
	{
		// receive Phase1
		boost::asio::async_read (m_Socket, boost::asio::buffer(&m_Phase1, sizeof (m_Phase1)),                     
			boost::bind(&NTCPSession::HandlePhase1Received, this, 
				boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred));
	}	
		
	void NTCPSession::HandlePhase1Sent (const boost::system::error_code& ecode, std::size_t bytes_transferred)
	{
		if (ecode)
        {
			LogPrint ("Couldn't send Phase 1 message: ", ecode.message ());
			Terminate ();
		}
		else
		{	
			LogPrint ("Phase 1 sent: ", bytes_transferred);
			boost::asio::async_read (m_Socket, boost::asio::buffer(&m_Phase2, sizeof (m_Phase2)),                  
				boost::bind(&NTCPSession::HandlePhase2Received, this, 
					boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred));
		}	
	}	

	void NTCPSession::HandlePhase1Received (const boost::system::error_code& ecode, std::size_t bytes_transferred)
	{
		if (ecode)
        {
			LogPrint ("Phase 1 read error: ", ecode.message ());
			Terminate ();
		}
		else
		{	
			LogPrint ("Phase 1 received: ", bytes_transferred);
			// verify ident
			uint8_t digest[32];
			CryptoPP::SHA256().CalculateDigest(digest, m_Phase1.pubKey, 256);
			const uint8_t * ident = i2p::context.GetRouterInfo ().GetIdentHash ();
			for (int i = 0; i < 32; i++)
			{	
				if ((m_Phase1.HXxorHI[i] ^ ident[i]) != digest[i])
				{
					LogPrint ("Wrong ident");
					Terminate ();
					return;
				}	
			}	
			
			SendPhase2 ();
		}	
	}	

	void NTCPSession::SendPhase2 ()
	{
		const uint8_t * y = i2p::context.GetRouterIdentity ().publicKey;
		memcpy (m_Phase2.pubKey, y, 256);
		uint8_t xy[512];
		memcpy (xy, m_Phase1.pubKey, 256);
		memcpy (xy + 256, y, 256);
		CryptoPP::SHA256().CalculateDigest(m_Phase2.encrypted.hxy, xy, 512); 
		uint32_t tsB = htobe32 (time(0));
		m_Phase2.encrypted.timestamp = tsB;
		// TODO: fill filler

		uint8_t aesKey[32];
		CreateAESKey (m_Phase1.pubKey, aesKey);
		m_Encryption.SetKeyWithIV (aesKey, 32, y + 240);
		m_Decryption.SetKeyWithIV (aesKey, 32, m_Phase1.HXxorHI + 16);
		
		m_Encryption.ProcessData((uint8_t *)&m_Phase2.encrypted, (uint8_t *)&m_Phase2.encrypted, sizeof(m_Phase2.encrypted));
		boost::asio::async_write (m_Socket, boost::asio::buffer (&m_Phase2, sizeof (m_Phase2)), boost::asio::transfer_all (),
        	boost::bind(&NTCPSession::HandlePhase2Sent, this, boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred, tsB));

	}	
		
	void NTCPSession::HandlePhase2Sent (const boost::system::error_code& ecode, std::size_t bytes_transferred, uint32_t tsB)
	{
		if (ecode)
        {
			LogPrint ("Couldn't send Phase 2 message: ", ecode.message ());
			Terminate ();
		}
		else
		{	
			LogPrint ("Phase 2 sent: ", bytes_transferred);
			boost::asio::async_read (m_Socket, boost::asio::buffer(&m_Phase3, sizeof (m_Phase3)),                   
				boost::bind(&NTCPSession::HandlePhase3Received, this, 
					boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred, tsB));
		}	
	}	
		
	void NTCPSession::HandlePhase2Received (const boost::system::error_code& ecode, std::size_t bytes_transferred)
	{
		if (ecode)
        {
			LogPrint ("Phase 2 read error: ", ecode.message ());
			Terminate ();
		}
		else
		{	
			LogPrint ("Phase 2 received: ", bytes_transferred);
		
			uint8_t aesKey[32];
			CreateAESKey (m_Phase2.pubKey, aesKey);
			m_Decryption.SetKeyWithIV (aesKey, 32, m_Phase2.pubKey + 240);
			m_Encryption.SetKeyWithIV (aesKey, 32, m_Phase1.HXxorHI + 16);
			
			m_Decryption.ProcessData((uint8_t *)&m_Phase2.encrypted, (uint8_t *)&m_Phase2.encrypted, sizeof(m_Phase2.encrypted));
			// verify
			uint8_t xy[512], hxy[32];
			memcpy (xy, i2p::context.GetRouterIdentity ().publicKey, 256);
			memcpy (xy + 256, m_Phase2.pubKey, 256);
			CryptoPP::SHA256().CalculateDigest(hxy, xy, 512); 
			if (memcmp (hxy, m_Phase2.encrypted.hxy, 32))
			{
				LogPrint ("Incorrect hash");
				Terminate ();
				return ;
			}	
			SendPhase3 ();
		}	
	}	

	void NTCPSession::SendPhase3 ()
	{
		m_Phase3.size = htons (sizeof (m_Phase3.ident));
		memcpy (&m_Phase3.ident, &i2p::context.GetRouterIdentity (), sizeof (m_Phase3.ident));		
		uint32_t tsA = htobe32 (time(0));
		m_Phase3.timestamp = tsA;
		
		SignedData s;
		memcpy (s.x, m_Phase1.pubKey, 256);
		memcpy (s.y, m_Phase2.pubKey, 256);
		memcpy (s.ident, m_RemoteRouterInfo.GetIdentHash (), 32);
		s.tsA = tsA;
		s.tsB = m_Phase2.encrypted.timestamp;
		i2p::context.Sign ((uint8_t *)&s, sizeof (s), m_Phase3.signature);

		m_Encryption.ProcessData((uint8_t *)&m_Phase3, (uint8_t *)&m_Phase3, sizeof(m_Phase3));
		        
		boost::asio::async_write (m_Socket, boost::asio::buffer (&m_Phase3, sizeof (m_Phase3)), boost::asio::transfer_all (),
        	boost::bind(&NTCPSession::HandlePhase3Sent, this, boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred, tsA));				
	}	
		
	void NTCPSession::HandlePhase3Sent (const boost::system::error_code& ecode, std::size_t bytes_transferred, uint32_t tsA)
	{
		if (ecode)
        {
			LogPrint ("Couldn't send Phase 3 message: ", ecode.message ());
			Terminate ();
		}
		else
		{	
			LogPrint ("Phase 3 sent: ", bytes_transferred);
			boost::asio::async_read (m_Socket, boost::asio::buffer(&m_Phase4, sizeof (m_Phase4)),                  
				boost::bind(&NTCPSession::HandlePhase4Received, this, 
					boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred, tsA));
		}	
	}	

	void NTCPSession::HandlePhase3Received (const boost::system::error_code& ecode, std::size_t bytes_transferred, uint32_t tsB)
	{	
		if (ecode)
        {
			LogPrint ("Phase 3 read error: ", ecode.message ());
			Terminate ();
		}
		else
		{	
			LogPrint ("Phase 3 received: ", bytes_transferred);
			m_Decryption.ProcessData((uint8_t *)&m_Phase3, (uint8_t *)&m_Phase3, sizeof(m_Phase3));
			m_RemoteRouterInfo.SetRouterIdentity (m_Phase3.ident);

			SignedData s;
			memcpy (s.x, m_Phase1.pubKey, 256);
			memcpy (s.y, m_Phase2.pubKey, 256);
			memcpy (s.ident, i2p::context.GetRouterInfo ().GetIdentHash (), 32);
			s.tsA = m_Phase3.timestamp;
			s.tsB = tsB;
			
			CryptoPP::DSA::PublicKey pubKey;
			pubKey.Initialize (dsap, dsaq, dsag, CryptoPP::Integer (m_RemoteRouterInfo.GetRouterIdentity ().signingKey, 128));
			CryptoPP::DSA::Verifier verifier (pubKey);
			if (!verifier.VerifyMessage ((uint8_t *)&s, sizeof(s), m_Phase3.signature, 40))
			{	
				LogPrint ("signature verification failed");
				Terminate ();
				return;
			}	

			SendPhase4 (tsB);
		}	
	}

	void NTCPSession::SendPhase4 (uint32_t tsB)
	{
		SignedData s;
		memcpy (s.x, m_Phase1.pubKey, 256);
		memcpy (s.y, m_Phase2.pubKey, 256);
		memcpy (s.ident, m_RemoteRouterInfo.GetIdentHash (), 32);
		s.tsA = m_Phase3.timestamp;
		s.tsB = tsB;
		i2p::context.Sign ((uint8_t *)&s, sizeof (s), m_Phase4.signature);
		m_Encryption.ProcessData((uint8_t *)&m_Phase4, (uint8_t *)&m_Phase4, sizeof(m_Phase4));

		boost::asio::async_write (m_Socket, boost::asio::buffer (&m_Phase4, sizeof (m_Phase4)), boost::asio::transfer_all (),
        	boost::bind(&NTCPSession::HandlePhase4Sent, this, boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred));
	}	

	void NTCPSession::HandlePhase4Sent (const boost::system::error_code& ecode,  std::size_t bytes_transferred)
	{
		if (ecode)
        {
			LogPrint ("Couldn't send Phase 4 message: ", ecode.message ());
			Terminate ();
		}
		else
		{	
			LogPrint ("Phase 4 sent: ", bytes_transferred);
			Connected ();
			m_ReceiveBufferOffset = 0;
			m_NextMessage = nullptr;
			Receive ();
		}	
	}	
		
	void NTCPSession::HandlePhase4Received (const boost::system::error_code& ecode, std::size_t bytes_transferred, uint32_t tsA)
	{
		if (ecode)
        {
			LogPrint ("Phase 4 read error: ", ecode.message ());
			Terminate ();
		}
		else
		{	
			LogPrint ("Phase 4 received: ", bytes_transferred);
			m_Decryption.ProcessData((uint8_t *)&m_Phase4, (uint8_t *)&m_Phase4, sizeof(m_Phase4));

			// verify signature
			SignedData s;
			memcpy (s.x, m_Phase1.pubKey, 256);
			memcpy (s.y, m_Phase2.pubKey, 256);
			memcpy (s.ident, i2p::context.GetRouterInfo ().GetIdentHash (), 32);
			s.tsA = tsA;
			s.tsB = m_Phase2.encrypted.timestamp;

			CryptoPP::DSA::PublicKey pubKey;
			pubKey.Initialize (dsap, dsaq, dsag, CryptoPP::Integer (m_RemoteRouterInfo.GetRouterIdentity ().signingKey, 128));
			CryptoPP::DSA::Verifier verifier (pubKey);
			if (!verifier.VerifyMessage ((uint8_t *)&s, sizeof(s), m_Phase4.signature, 40))
			{	
				LogPrint ("signature verification failed");
				Terminate ();
				return;
			}	
			Connected ();
						
			m_ReceiveBufferOffset = 0;
			m_NextMessage = nullptr;
			Receive ();
		}
	}

	void NTCPSession::Receive ()
	{
		m_Socket.async_read_some (boost::asio::buffer(m_ReceiveBuffer + m_ReceiveBufferOffset, NTCP_MAX_MESSAGE_SIZE*2 -m_ReceiveBufferOffset),                
			boost::bind(&NTCPSession::HandleReceived, this, 
			boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred));
	}	
		
	void NTCPSession::HandleReceived (const boost::system::error_code& ecode, std::size_t bytes_transferred)
	{
		if (ecode)
        {
			LogPrint ("Read error: ", ecode.message ());
			Terminate ();
		}
		else
		{
			LogPrint ("Received: ", bytes_transferred);
			m_ReceiveBufferOffset += bytes_transferred;

			if (m_ReceiveBufferOffset >= 16)
			{	
				uint8_t * nextBlock = m_ReceiveBuffer;
				while (m_ReceiveBufferOffset >= 16)
				{
					DecryptNextBlock (nextBlock); // 16 bytes
					nextBlock += 16;
					m_ReceiveBufferOffset -= 16;
				}	
				if (m_ReceiveBufferOffset > 0)
					memcpy (m_ReceiveBuffer, nextBlock, m_ReceiveBufferOffset);
			}	
		
			Receive ();
		}	
	}	

	void NTCPSession::DecryptNextBlock (const uint8_t * encrypted) // 16 bytes
	{
		if (!m_NextMessage) // new message, header expected
		{	
			m_NextMessage = i2p::NewI2NPMessage ();
			m_NextMessageOffset = 0;
			
			m_Decryption.ProcessData (m_NextMessage->buf, encrypted, 16);
			uint16_t dataSize = be16toh (*(uint16_t *)m_NextMessage->buf);
			if (dataSize)
			{
				// new message
				m_NextMessageOffset += 16;
				m_NextMessage->offset = 2; // size field
				m_NextMessage->len = dataSize + 2; 
			}	
			else
			{	
				// timestamp
				LogPrint ("Timestamp");	
				i2p::DeleteI2NPMessage (m_NextMessage);
				m_NextMessage = nullptr;
				return;
			}	
		}	
		else // message continues
		{	
			m_Decryption.ProcessData (m_NextMessage->buf + m_NextMessageOffset, encrypted, 16);
			m_NextMessageOffset += 16;
		}		
		
		if (m_NextMessageOffset >= m_NextMessage->len + 4) // +checksum
		{	
			// we have a complete I2NP message
			i2p::HandleI2NPMessage (m_NextMessage);	
			m_NextMessage = nullptr;
		}	
 	}	

	void NTCPSession::Send (i2p::I2NPMessage * msg)
	{
		uint8_t * sendBuffer;
		int len;

		if (msg)
		{	
			// regular I2NP
			if (msg->offset < 2)
			{
				LogPrint ("Malformed I2NP message");
				i2p::DeleteI2NPMessage (msg);
			}	
			sendBuffer = msg->GetBuffer () - 2; 
			len = msg->GetLength ();
			*((uint16_t *)sendBuffer) = htobe16 (len);
		}	
		else
		{
			// prepare timestamp
			sendBuffer = m_TimeSyncBuffer;
			len = 4;
			*((uint16_t *)sendBuffer) = 0;
			*((uint32_t *)(sendBuffer + 2)) = htobe32 (time (0));
		}	
		int rem = (len + 6) % 16;
		int padding = 0;
		if (rem > 0) padding = 16 - rem;
		// TODO: fill padding 
		m_Adler.CalculateDigest (sendBuffer + len + 2 + padding, sendBuffer, len + 2+ padding);

		int l = len + padding + 6;
		{
			std::lock_guard<std::mutex> lock (m_EncryptionMutex);
			m_Encryption.ProcessData(sendBuffer, sendBuffer, l);
		}	

		boost::asio::async_write (m_Socket, boost::asio::buffer (sendBuffer, l), boost::asio::transfer_all (),                      
        	boost::bind(&NTCPSession::HandleSent, this, boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred, msg));				
	}
		
	void NTCPSession::HandleSent (const boost::system::error_code& ecode, std::size_t bytes_transferred, i2p::I2NPMessage * msg)
	{		
		if (msg)
			i2p::DeleteI2NPMessage (msg);
		if (ecode)
        {
			LogPrint ("Couldn't send msg: ", ecode.message ());
			Terminate ();
		}
		else
		{	
			LogPrint ("Msg sent: ", bytes_transferred);
		}	
	}

	void NTCPSession::SendTimeSyncMessage ()
	{
		Send (nullptr);
	}	

	void NTCPSession::SendI2NPMessage (I2NPMessage * msg)
	{
		if (msg)
		{
			if (m_IsEstablished)
				Send (msg);
			else
				m_DelayedMessage = msg;	
		}	
	}	
		
	NTCPClient::NTCPClient (boost::asio::io_service& service, const char * address, 
		int port, const i2p::data::RouterInfo& in_RouterInfo): NTCPSession (m_Socket, &in_RouterInfo),
		m_Socket (service), m_Endpoint (boost::asio::ip::address::from_string (address), port)	
	{
		Connect ();
	}

	void NTCPClient::Connect ()
	{
		LogPrint ("Connecting to ", m_Endpoint.address ().to_string (),":",  m_Endpoint.port ());
		 m_Socket.async_connect (m_Endpoint, boost::bind (&NTCPClient::HandleConnect,
			this, boost::asio::placeholders::error));
	}	

	void NTCPClient::HandleConnect (const boost::system::error_code& ecode)
	{
		if (ecode)
        {
			LogPrint ("Connect error: ", ecode.message ());
			Terminate ();
		}
		else
		{
			LogPrint ("Connected");
			ClientLogin ();
		}	
	}	

	NTCPServerConnection::NTCPServerConnection (boost::asio::io_service& service):
		NTCPSession (m_Socket), m_Socket (service)
	{
	}	
}	
}	
