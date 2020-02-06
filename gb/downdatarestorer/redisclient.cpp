#include "redisclient.h"
#include "redisconnection.h"
#include"util.h"
#include <algorithm>
#include<cassert>


RedisClient::RedisClient():m_logger("cdn.common.redisclient")
{
	m_clusterMap.clear();
	m_serverList.clear();
	m_redisMode=CLUSTER_MODE;
	m_connectionNum = 0;
	m_keepaliveTime = 0;
	m_slotMap.clear();
	m_unusedHandlers.clear();
    m_connected=false;

    m_logger.debug("construct RedisClient ok");
}

RedisClient::~RedisClient()
{
    if(m_connected==true)
        freeRedisClient();
}

bool RedisClient::freeRedisClient()
{
    if(m_connected==false)
    {
        m_logger.error("why RedisClient freeRedisClient called when m_connected is false");
        return false;
    }
	if(m_redisMode==CLUSTER_MODE)
	{
		freeRedisCluster();
	}
	else
	{
		freeRedisProxy();
	}
    m_connected=false;
    return true;
}

bool RedisClient::init(REDIS_SERVER_LIST& serverList,uint32_t connectionNum,uint32_t keepaliveTime)
{
    if(m_connected==true)
    {
        m_logger.error("why RedisClient init called, when m_connected is true");
        return false;
    }
	//first do cluster nodes
	m_serverList = serverList;
	if(m_serverList.size()==1)
	{
		m_redisMode=PROXY_MODE; // proxy or just one redis server instance
	}
	m_connectionNum = connectionNum;
	m_keepaliveTime = keepaliveTime;
	if(m_redisMode==CLUSTER_MODE)
	{
		if (!getRedisClusterNodes())
		{
			m_logger.error("get redis cluster info failed.please check redis config.");
			return false;
		}
		//init cluster.
		if (!initRedisCluster())
		{
			m_logger.warn("init redis cluster failed.");
			return false;
		}
	}
	else
	{
		if (!initRedisProxy())
		{
			m_logger.warn("init redis proxy failed.");
			return false;
		}
		m_logger.info("init the redis proxy or redis server %s, %d ok", m_redisProxy.connectIp.c_str(), m_redisProxy.connectPort);
	}
    m_connected=true;
	return true;
}

bool RedisClient::initRedisCluster()
{
	WriteGuard guard(m_rwClusterMutex);
	REDIS_CLUSTER_MAP::iterator clusterIter;
	for (clusterIter = m_clusterMap.begin(); clusterIter != m_clusterMap.end(); clusterIter++)
	{
		((*clusterIter).second).clusterHandler = new RedisCluster();
		if (!((*clusterIter).second).clusterHandler->initConnectPool(((*clusterIter).second).connectIp, ((*clusterIter).second).connectPort, ((*clusterIter).second).connectionNum, ((*clusterIter).second).keepaliveTime))
		{
			m_logger.warn("init cluster:[%s] connect pool failed.", (*clusterIter).first.c_str());
			return false;
		}
		//fill slot map
		if (((*clusterIter).second).isMaster)
		{
			WriteGuard guard(m_rwSlotMutex);
			map<uint16_t,uint16_t>::iterator iter;
			for(iter = ((*clusterIter).second).slotMap.begin(); iter != ((*clusterIter).second).slotMap.end(); iter++)
			{
				uint16_t startSlotNum = (*iter).first;
				uint16_t stopSlotNum = (*iter).second;
				for(int i = startSlotNum; i <= stopSlotNum; i++)
				{
					m_slotMap[i] = (*clusterIter).first;
				}
			}
		}
	}
	return true;
}

//void RedisClient::checkState()
//{
//    if(m_redisMode==PROXY_MODE)
//    {
//        m_logger.info("m_redisProxy : proxyId %s, connectIp %s, connnecPort %d, connectionNum %d, keepaliveTime %d, clusterHandler %x", m_redisProxy.proxyId.c_str(), m_redisProxy.connectIp.c_str(), m_redisProxy.connectPort, m_redisProxy.connectionNum, m_redisProxy.keepaliveTime, m_redisProxy.clusterHandler);
//        if(m_redisProxy.clusterHandler==NULL)
//            m_logger.error("redis client proxy check failed");
//    }
//    else
//        m_logger.info("CLUSTER MODE");
//}

bool RedisClient::initRedisProxy()
{
//    WriteGuard guard(m_rwProxyMutex);
	m_redisProxy.connectIp=m_serverList.front().serverIp;
	m_redisProxy.connectPort=m_serverList.front().serverPort;
	m_redisProxy.proxyId=m_redisProxy.connectIp+":"+toStr(m_redisProxy.connectPort);
	m_redisProxy.connectionNum=m_connectionNum;
	m_redisProxy.keepaliveTime=m_keepaliveTime;

    if(m_redisProxy.clusterHandler != NULL)
    {
        m_logger.error("why m_redisProxy.clusterHandler not NULL when initRedisProxy");
        freeRedisProxy();
    }
	m_redisProxy.clusterHandler = new RedisCluster();
    if(m_redisProxy.clusterHandler==NULL)
    {
        return false;
    }
	if (!m_redisProxy.clusterHandler->initConnectPool(m_redisProxy.connectIp, m_redisProxy.connectPort, m_redisProxy.connectionNum, m_redisProxy.keepaliveTime))
	{
		m_logger.warn("init proxy:[%s] connect pool failed.", m_redisProxy.proxyId.c_str());
		return false;
	}
	m_logger.debug("init proxy:[%s] connect pool ok.", m_redisProxy.proxyId.c_str());
	return true;
}

bool RedisClient::freeRedisCluster()
{
	WriteGuard guard(m_rwClusterMutex);
	REDIS_CLUSTER_MAP::iterator clusterIter;
	for (clusterIter = m_clusterMap.begin(); clusterIter != m_clusterMap.end(); clusterIter++)
	{
		RedisClusterInfo clusterInfo = (*clusterIter).second;
		if (clusterInfo.clusterHandler != NULL)
		{
			clusterInfo.clusterHandler->freeConnectPool();
			delete clusterInfo.clusterHandler;
			clusterInfo.clusterHandler = NULL;
		}
	}
	return true;
}

bool RedisClient::freeRedisProxy()
{
//	WriteGuard guard(m_rwProxyMutex);
	if (m_redisProxy.clusterHandler != NULL)
	{
        m_logger.info("freeRedisProxy call freeConnectionPool");
		m_redisProxy.clusterHandler->freeConnectPool();
        if(m_redisProxy.clusterHandler)
    		delete m_redisProxy.clusterHandler;
		m_redisProxy.clusterHandler = NULL;
	}

	return true;
}


bool RedisClient::getSerial(const string & key,DBSerialize & serial)
{
	list<RedisCmdParaInfo> paraList;
	int32_t paraLen = 0;
	fillCommandPara("get", 3, paraList);
	paraLen += 15;
	fillCommandPara(key.c_str(), key.length(), paraList);
	paraLen += key.length() + 20;
	bool success = false;
	success = doRedisCommand( key,paraLen,paraList,RedisCommandType::REDIS_COMMAND_GET, &serial);
	freeCommandList(paraList);
	return success;
}

bool RedisClient::getSerialWithLock(const string & key,DBSerialize & serial,RedisLockInfo & lockInfo)
{
	//add for  redis Optimistic Lock command, includes watch key, get key, or unwatch,when get key failed.
	bool success = false;
	list<RedisCmdParaInfo> watchParaList;
	int32_t watchParaLen = 0;
	fillCommandPara("watch", 5, watchParaList);
	watchParaLen += 18;
	fillCommandPara(key.c_str(), key.length(), watchParaList);
	watchParaLen += key.length() + 20;
	success = doRedisCommandWithLock(key,watchParaLen,watchParaList,RedisCommandType::REDIS_COMMAND_WATCH,lockInfo);
	freeCommandList(watchParaList);
	if (!success)
	{
		m_logger.warn("do watch key:%s failed.", key.c_str());
		return false;
	}
	list<RedisCmdParaInfo> getParaList;
	int32_t getParaLen = 0;
	fillCommandPara("get", 3, getParaList);
	getParaLen += 15;
	fillCommandPara(key.c_str(), key.length(), getParaList);
	getParaLen += key.length() + 20;
	success = doRedisCommandWithLock(key,getParaLen,getParaList,RedisCommandType::REDIS_COMMAND_GET,lockInfo,true,&serial);
	freeCommandList(getParaList);
	if (success)
	{
		m_logger.info("get key:%s serial with optimistic lock success.", key.c_str());
	}
	else
	{
		m_logger.warn("get key:%s serial with optimistic lock failed.", key.c_str());
		list<RedisCmdParaInfo> unwatchParaList;
		int32_t unwatchParaLen = 0;
		fillCommandPara("unwatch", 7, unwatchParaList);
		unwatchParaLen += 20;
		doRedisCommandWithLock(key,unwatchParaLen,unwatchParaList,RedisCommandType::REDIS_COMMAND_UNWATCH,lockInfo);
		freeCommandList(unwatchParaList);
	}
	return success;
}

bool RedisClient::setSerialWithLock(const string & key, const DBSerialize& serial,RedisLockInfo & lockInfo)
{
	//add for  redis Optimistic Lock command, includes multi, set key, exec.
	bool success = false;
	//do multi command.
	list<RedisCmdParaInfo> multiParaList;
	int32_t multiParaLen = 0;
	fillCommandPara("multi", 5, multiParaList);
	multiParaLen += 18;
	success = doRedisCommandWithLock(key,multiParaLen,multiParaList,RedisCommandType::REDIS_COMMAND_MULTI,lockInfo);
	freeCommandList(multiParaList);
	if (!success)
	{
		m_logger.warn("do multi key:%s failed.", key.c_str());
		list<RedisCmdParaInfo> unwatchParaList;
		int32_t unwatchParaLen = 0;
		fillCommandPara("unwatch", 7, unwatchParaList);
		unwatchParaLen += 20;
		doRedisCommandWithLock(key,unwatchParaLen,unwatchParaList,RedisCommandType::REDIS_COMMAND_UNWATCH,lockInfo);
		freeCommandList(unwatchParaList);
		return false;
	}
	//do set key value
	list<RedisCmdParaInfo> setParaList;
	int32_t setParaLen = 0;
	fillCommandPara("set", 3, setParaList);
	setParaLen += 15;
	fillCommandPara(key.c_str(), key.length(), setParaList);
	setParaLen += key.length() + 20;
	DBOutStream out;
	serial.save(out);
	fillCommandPara(out.getData(), out.getSize(), setParaList);
	setParaLen += out.getSize() + 20;
	success = doRedisCommandWithLock(key,setParaLen,setParaList,RedisCommandType::REDIS_COMMAND_SET,lockInfo);
	freeCommandList(setParaList);
	if (success)
	{
		m_logger.info("set key:%s serial with optimistic lock success.", key.c_str());
	}
	else
	{
		m_logger.warn("set key:%s serial with optimistic lock failed.", key.c_str());
		list<RedisCmdParaInfo> unwatchParaList;
		int32_t unwatchParaLen = 0;
		fillCommandPara("unwatch", 7, unwatchParaList);
		unwatchParaLen += 20;
		doRedisCommandWithLock(key,unwatchParaLen,unwatchParaList,RedisCommandType::REDIS_COMMAND_UNWATCH,lockInfo);
		freeCommandList(unwatchParaList);
		return false;
	}
	//do exec,need check exec response.
	list<RedisCmdParaInfo> execParaList;
	int32_t execParaLen = 0;
	fillCommandPara("exec", 4, execParaList);
	execParaLen += 18;
	success = doRedisCommandWithLock(key,execParaLen,execParaList,RedisCommandType::REDIS_COMMAND_EXEC,lockInfo);
	freeCommandList(execParaList);
	if (!success)
	{
		m_logger.warn("update data error,key[%s]",key.c_str());
        return false;
	}
	return success;
}

bool RedisClient::releaseLock(const string & key,RedisLockInfo & lockInfo)
{
	if (lockInfo.clusterId.empty())
	{
		m_logger.warn("lock cluster id is empty.");
		return false;
	}
	list<RedisCmdParaInfo> unwatchParaList;
	int32_t unwatchParaLen = 0;
	fillCommandPara("unwatch", 7, unwatchParaList);
	unwatchParaLen += 20;
	doRedisCommandWithLock(key,unwatchParaLen,unwatchParaList,RedisCommandType::REDIS_COMMAND_UNWATCH,lockInfo);
	freeCommandList(unwatchParaList);
	return true;
}

bool RedisClient::find(const string& key)
{
	list<RedisCmdParaInfo> paraList;
	int32_t paraLen = 0;
	fillCommandPara("exists", 6, paraList);
	paraLen += 15;
	fillCommandPara(key.c_str(), key.length(), paraList);
	paraLen += key.length() + 20;
	bool success = false;
	success = doRedisCommand( key,paraLen,paraList,RedisCommandType::REDIS_COMMAND_EXISTS);
	freeCommandList(paraList);
	return success;
}

bool RedisClient::delKeys(const string & keyPrefix)
{
	list<string> keys;
	getKeys(keyPrefix,keys);

	list<string>::iterator it;
	for(it = keys.begin();it != keys.end();it++)
	{
		del(*it);
	}
	keys.clear();
	return true;
}
bool RedisClient::setSerial(const string& key, const DBSerialize& serial)
{
	list<RedisCmdParaInfo> paraList;
	int32_t paraLen = 0;
	fillCommandPara("set", 3, paraList);
	paraLen += 15;
	fillCommandPara(key.c_str(), key.length(), paraList);
	paraLen += key.length() + 20;
	DBOutStream out;
	serial.save(out);
	fillCommandPara(out.getData(), out.getSize(), paraList);
	paraLen += out.getSize() + 20;
	bool success = false;
	success = doRedisCommand( key,paraLen,paraList,RedisCommandType::REDIS_COMMAND_SET);
	freeCommandList(paraList);
	return success;
}

bool RedisClient::setSerialWithExpire(const string & key, const DBSerialize & serial,uint32_t expireTime)
{
	list<RedisCmdParaInfo> paraList;
	int32_t paraLen = 0;
	fillCommandPara("setex", 5, paraList);
	paraLen += 17;
	fillCommandPara(key.c_str(), key.length(), paraList);
	paraLen += key.length() + 20;
	//add expire time
	string expireStr = toStr(expireTime);
	fillCommandPara(expireStr.c_str(), expireStr.length(), paraList);
	paraLen += expireStr.length() + 20;
	DBOutStream out;
	serial.save(out);
	fillCommandPara(out.getData(), out.getSize(), paraList);
	paraLen += out.getSize() + 20;
	bool success = false;
	success = doRedisCommand(key,paraLen,paraList,RedisCommandType::REDIS_COMMAND_SET);
	freeCommandList(paraList);
	return success;
}

bool RedisClient::del(const string & key)
{
	list<RedisCmdParaInfo> paraList;
	int32_t paraLen = 0;
	fillCommandPara("del", 3, paraList);
	paraLen += 15;
	fillCommandPara(key.c_str(), key.length(), paraList);
	paraLen += key.length() + 20;
	bool success = false;
	success = doRedisCommand( key,paraLen,paraList,RedisCommandType::REDIS_COMMAND_DEL);
	freeCommandList(paraList);
	return success;
}

bool RedisClient::doRedisCommand(const string & key,
							int32_t commandLen,
							list < RedisCmdParaInfo > & commandList,
							int commandType,
							DBSerialize * serial)
{
	if(m_redisMode==CLUSTER_MODE)
	{
		return doRedisCommandCluster(key, commandLen, commandList, commandType, serial);
	}
	else
	{
		return doRedisCommandProxy(key, commandLen, commandList, commandType, serial);
	}
}

bool RedisClient::doRedisCommandProxy(const string & key,
							int32_t commandLen,
							list < RedisCmdParaInfo > & commandList,
							int commandType,
							DBSerialize * serial)
{
	RedisReplyInfo replyInfo;
	bool needRedirect;
	string redirectInfo;
    if(m_redisProxy.clusterHandler==NULL)
    {
        m_logger.error("m_redisProxy.clusterHandler is NULL");
        return false;
    }
	if(!m_redisProxy.clusterHandler->doRedisCommand(commandList, commandLen, replyInfo))
	{
		freeReplyInfo(replyInfo);
		m_logger.warn("proxy:%s do redis command failed.", m_redisProxy.proxyId.c_str());
		return false;
	}

	switch (commandType)
	{
		case RedisCommandType::REDIS_COMMAND_GET:
			
			if(!parseGetSerialReply(replyInfo,*serial,needRedirect,redirectInfo))
			{
				m_logger.warn("parse key:[%s] get string reply failed. reply string:%s.", key.c_str(), replyInfo.resultString.c_str());
				freeReplyInfo(replyInfo);
				return false;
			}
			break;
		case RedisCommandType::REDIS_COMMAND_SET:
			if(!parseSetSerialReply(replyInfo,needRedirect,redirectInfo))
			{
				m_logger.warn("parse key:[%s] set string reply failed. reply string:%s.", key.c_str(), replyInfo.resultString.c_str());
				freeReplyInfo(replyInfo);
				return false;
			}
			break;
		case RedisCommandType::REDIS_COMMAND_EXISTS:
			if(!parseFindReply(replyInfo,needRedirect,redirectInfo))
			{
				m_logger.warn("parse key:[%s] get string reply failed. reply string:%s.", key.c_str(), replyInfo.resultString.c_str());
				freeReplyInfo(replyInfo);
				return false;
			}
			//find
			if(replyInfo.replyType == RedisReplyType::REDIS_REPLY_INTEGER && replyInfo.intValue == 1)
			{
				m_logger.debug("find key:%s in redis db",key.c_str());
				freeReplyInfo(replyInfo);
				return true;
			}
			//not find
			else if(replyInfo.replyType == RedisReplyType::REDIS_REPLY_INTEGER && replyInfo.intValue == 0)
			{
				m_logger.warn("not find key:%s in redis db",key.c_str());
				freeReplyInfo(replyInfo);
				return false;
			}
			break;
		case RedisCommandType::REDIS_COMMAND_DEL:
			if(!parseFindReply(replyInfo,needRedirect,redirectInfo))
			{
				m_logger.warn("parse key:[%s] del string reply failed. reply string:%s.", key.c_str(), replyInfo.resultString.c_str());
				freeReplyInfo(replyInfo);
				return false;
			}
			//del success
			if(replyInfo.replyType == RedisReplyType::REDIS_REPLY_INTEGER && replyInfo.intValue == 1)
			{
				m_logger.debug("del key:%s from redis db success.",key.c_str());
				freeReplyInfo(replyInfo);
				return true;
			}
			//del failed
			else if(replyInfo.replyType == RedisReplyType::REDIS_REPLY_INTEGER && replyInfo.intValue == 0)
			{
				m_logger.warn("del key:%s from redis db failed.",key.c_str());
				freeReplyInfo(replyInfo);
				return false;
			}
			break;
		case RedisCommandType::REDIS_COMMAND_EXPIRE:
			if(!parseFindReply(replyInfo,needRedirect,redirectInfo))
			{
				m_logger.warn("parse key:[%s] set expire reply failed. reply string:%s.", key.c_str(), replyInfo.resultString.c_str());
				freeReplyInfo(replyInfo);
				return false;
			}
			//set expire success
			if(replyInfo.replyType == RedisReplyType::REDIS_REPLY_INTEGER && replyInfo.intValue == 1)
			{
				m_logger.debug("set key:%s expire success.",key.c_str());
				freeReplyInfo(replyInfo);
				return true;
			}
			//set expire failed
			else if(replyInfo.replyType == RedisReplyType::REDIS_REPLY_INTEGER && replyInfo.intValue == 0)
			{
				m_logger.warn("set key:%s expire failed.",key.c_str());
				freeReplyInfo(replyInfo);
				return false;
			}
			break;
		case RedisCommandType::REDIS_COMMAND_ZADD:
			if(!parseFindReply(replyInfo,needRedirect,redirectInfo))
			{
				m_logger.warn("parse key:[%s] zset add reply failed. reply string:%s.", key.c_str(), replyInfo.resultString.c_str());
				freeReplyInfo(replyInfo);
				return false;
			}
			// success
			if(replyInfo.replyType == RedisReplyType::REDIS_REPLY_INTEGER && replyInfo.intValue == 1)
			{
				m_logger.debug("zset key:%s add success.",key.c_str());
				freeReplyInfo(replyInfo);
				return true;
			}
			// failed
			else if(replyInfo.replyType == RedisReplyType::REDIS_REPLY_INTEGER && replyInfo.intValue == 0)
			{
				m_logger.debug("zset key:%s add done,maybe exists.",key.c_str());
				freeReplyInfo(replyInfo);
				return false;
			}				
			break;
		case RedisCommandType::REDIS_COMMAND_ZREM:
			if(!parseFindReply(replyInfo,needRedirect,redirectInfo))
			{
				m_logger.warn("parse key:[%s] zset rem reply failed. reply string:%s.", key.c_str(), replyInfo.resultString.c_str());
				freeReplyInfo(replyInfo);
				return false;
			}
			// success
			if(replyInfo.replyType == RedisReplyType::REDIS_REPLY_INTEGER && replyInfo.intValue == 1)
			{
				m_logger.debug("set key:%s rem success.",key.c_str());
				freeReplyInfo(replyInfo);
				return true;
			}
			// failed
			else if(replyInfo.replyType == RedisReplyType::REDIS_REPLY_INTEGER && replyInfo.intValue == 0)
			{
				m_logger.warn("set key:%s rem failed.",key.c_str());
				freeReplyInfo(replyInfo);
				return false;
			}

			break;
		case RedisCommandType::REDIS_COMMAND_ZINCRBY:
			// success
			if(replyInfo.replyType == RedisReplyType::REDIS_REPLY_STRING)
			{
				m_logger.debug("set key:%s zincrby success.",key.c_str());
				freeReplyInfo(replyInfo);
				return true;
			}
			// failed
			else
			{
				m_logger.warn("set key:%s zincrby failed.",key.c_str());
				freeReplyInfo(replyInfo);
				return false;
			}
			break;

		case RedisCommandType::REDIS_COMMAND_ZREMRANGEBYSCORE:
			if(!parseFindReply(replyInfo,needRedirect,redirectInfo))
			{
				m_logger.warn("parse key:[%s] zset zremrangebyscore reply failed. reply string:%s.", key.c_str(), replyInfo.resultString.c_str());
				freeReplyInfo(replyInfo);
				return false;
			}
			// success
			if(replyInfo.replyType == RedisReplyType::REDIS_REPLY_INTEGER && replyInfo.intValue > 0)
			{
				m_logger.debug("set key:%s zremrangebyscore success.",key.c_str());
				freeReplyInfo(replyInfo);
				return true;
			}
			// failed
			else if(replyInfo.replyType == RedisReplyType::REDIS_REPLY_INTEGER && replyInfo.intValue == 0)
			{
				m_logger.warn("set key:%s zremrangebyscore failed.",key.c_str());
				freeReplyInfo(replyInfo);
				return false;
			}
			break;
		case RedisCommandType::REDIS_COMMAND_ZCOUNT:
        case RedisCommandType::REDIS_COMMAND_DBSIZE:
		case RedisCommandType::REDIS_COMMAND_ZCARD:
		case RedisCommandType::REDIS_COMMAND_SCARD:
		case RedisCommandType::REDIS_COMMAND_SADD:
		case RedisCommandType::REDIS_COMMAND_SREM:
		case RedisCommandType::REDIS_COMMAND_SISMEMBER:
			if(!parseFindReply(replyInfo,needRedirect,redirectInfo))
			{
				m_logger.warn("parse key:[%s] zset add reply failed. reply string:%s.", key.c_str(), replyInfo.resultString.c_str());
				freeReplyInfo(replyInfo);
				return -1;
			}
			
			if(replyInfo.replyType == RedisReplyType::REDIS_REPLY_INTEGER)
			{
				int count = replyInfo.intValue;
				m_logger.info("key %s commandType %d success, return integer %d", key.c_str(), commandType, count);
				freeReplyInfo(replyInfo);
				return count;
			}
			break;
		case RedisCommandType::REDIS_COMMAND_ZSCORE:
			if (checkIfNeedRedirect(replyInfo, needRedirect, redirectInfo))
			{
				m_logger.info("need direct to cluster:[%s].", redirectInfo.c_str());
			}
			else
			{	
				if (replyInfo.replyType != RedisReplyType::REDIS_REPLY_STRING)
				{
					m_logger.warn("recv redis wrong reply type:[%d].", replyInfo.replyType);
					freeReplyInfo(replyInfo);
					return -1;
				}
				list<ReplyArrayInfo>::iterator iter = replyInfo.arrayList.begin();
				if (iter == replyInfo.arrayList.end())
				{
					m_logger.warn("reply not have array info.");
					freeReplyInfo(replyInfo);
					return -1;
				}
				if ((*iter).replyType == RedisReplyType::REDIS_REPLY_NIL)
				{
					m_logger.warn("get failed,the key not exist.");
					freeReplyInfo(replyInfo);
					return -1;
				}
				char score_c[64] = {0};
				memcpy(score_c,(*iter).arrayValue,(*iter).arrayLen);
				int score = atoi(score_c);
				m_logger.info("key:%s,score:%d",key.c_str(),score);
				return score;	
			}
			break;
		default:
			m_logger.warn("recv unknown command type:%d", commandType);
			return false;
		
	}
	freeReplyInfo(replyInfo);
	return true;
}


bool RedisClient::doRedisCommandCluster(const string & key,
							int32_t commandLen,
							list < RedisCmdParaInfo > & commandList,
							int commandType,
							DBSerialize * serial)
{
	assert(m_redisMode==CLUSTER_MODE);
	//first calc crc16 value.
	uint16_t crcValue = crc16(key.c_str(), key.length());
	crcValue %= REDIS_SLOT_NUM;
	string clusterId;
	if (getClusterIdBySlot(crcValue, clusterId))
	{
		//add for redirect end endless loop;
		vector<string> redirects;
		RedisReplyInfo replyInfo;
		m_logger.debug("key:[%s] hit slot:[%d] select cluster[%s].", key.c_str(), crcValue, clusterId.c_str());
		list<string> bakClusterList;
		list<string>::iterator bakIter;

REDIS_COMMAND:
		//get cluster
		RedisClusterInfo clusterInfo;
		if (getRedisClusterInfo(clusterId,clusterInfo))
		{
			if(!clusterInfo.clusterHandler->doRedisCommand(commandList, commandLen, replyInfo))
			{
				freeReplyInfo(replyInfo);
				m_logger.warn("cluster:%s do redis command failed.", clusterId.c_str());
				//need send to another cluster. check bak cluster.
                if(bakClusterList.empty()==false)
                {
                    bakIter++;
                    if (bakIter != bakClusterList.end())
                    {
                        clusterId = (*bakIter);
                        goto REDIS_COMMAND;
                    }
                    else
                    {
                        m_logger.warn("key:[%s] send to all bak cluster failed", key.c_str());
                        return false;
                    }
                }
                else
                {
                    bakClusterList = clusterInfo.bakClusterList;
                    bakIter = bakClusterList.begin();
                    if (bakIter != bakClusterList.end())
                    {
                        clusterId = (*bakIter);
                        goto REDIS_COMMAND;
                    }
                    else
                    {
                        m_logger.warn("key:[%s] send to all bak cluster failed", key.c_str());
                        return false;
                    }
                }
//				if (clusterInfo.bakClusterList.size() != 0)
//				{
//					bakClusterList = clusterInfo.bakClusterList;
//					bakIter = bakClusterList.begin();
//					if (bakIter != bakClusterList.end())
//					{
//						clusterId = (*bakIter);
//						goto REDIS_COMMAND;
//					}
//					else
//					{
//						m_logger.warn("key:[%s] send to all redis cluster failed, may be slot:[%d] is something wrong", key.c_str(), crcValue);
//						return false;
//					}
//				}
//				else
//				{
//					if (bakClusterList.size() > 0)
//					{
//						bakIter++;
//						if (bakIter != bakClusterList.end())
//						{
//							clusterId = (*bakIter);
//							goto REDIS_COMMAND;
//						}
//						else
//						{
//							m_logger.warn("key:[%s] send to all redis cluster failed, may be slot:[%d] is something wrong", key.c_str(), crcValue);
//							return false;
//						}
//					}
//				}
			}
			bool needRedirect = false;
			string redirectInfo;
			redirectInfo.clear();
			m_logger.debug("start to parse command type:%d redis reply.", commandType);
			//switch command type
			switch (commandType)
			{
				case RedisCommandType::REDIS_COMMAND_GET:
					
					if(!parseGetSerialReply(replyInfo,*serial,needRedirect,redirectInfo))
					{
						m_logger.warn("parse key:[%s] get string reply failed. reply string:%s.", key.c_str(), replyInfo.resultString.c_str());
						freeReplyInfo(replyInfo);
						return false;
					}
					break;
				case RedisCommandType::REDIS_COMMAND_SET:
					if(!parseSetSerialReply(replyInfo,needRedirect,redirectInfo))
					{
						m_logger.warn("parse key:[%s] set string reply failed. reply string:%s.", key.c_str(), replyInfo.resultString.c_str());
						freeReplyInfo(replyInfo);
						return false;
					}
					break;
				case RedisCommandType::REDIS_COMMAND_EXISTS:
					if(!parseFindReply(replyInfo,needRedirect,redirectInfo))
					{
						m_logger.warn("parse key:[%s] get string reply failed. reply string:%s.", key.c_str(), replyInfo.resultString.c_str());
						freeReplyInfo(replyInfo);
						return false;
					}
					//find
					if(replyInfo.replyType == RedisReplyType::REDIS_REPLY_INTEGER && replyInfo.intValue == 1)
					{
						m_logger.debug("find key:%s in redis db",key.c_str());
						freeReplyInfo(replyInfo);
						return true;
					}
					//not find
					else if(replyInfo.replyType == RedisReplyType::REDIS_REPLY_INTEGER && replyInfo.intValue == 0)
					{
						m_logger.warn("not find key:%s in redis db",key.c_str());
						freeReplyInfo(replyInfo);
						return false;
					}
					break;
				case RedisCommandType::REDIS_COMMAND_DEL:
					if(!parseFindReply(replyInfo,needRedirect,redirectInfo))
					{
						m_logger.warn("parse key:[%s] del string reply failed. reply string:%s.", key.c_str(), replyInfo.resultString.c_str());
						freeReplyInfo(replyInfo);
						return false;
					}
					//del success
					if(replyInfo.replyType == RedisReplyType::REDIS_REPLY_INTEGER && replyInfo.intValue == 1)
					{
						m_logger.debug("del key:%s from redis db success.",key.c_str());
						freeReplyInfo(replyInfo);
						return true;
					}
					//del failed
					else if(replyInfo.replyType == RedisReplyType::REDIS_REPLY_INTEGER && replyInfo.intValue == 0)
					{
						m_logger.warn("del key:%s from redis db failed.",key.c_str());
						freeReplyInfo(replyInfo);
						return false;
					}
					break;
				case RedisCommandType::REDIS_COMMAND_EXPIRE:
					if(!parseFindReply(replyInfo,needRedirect,redirectInfo))
					{
						m_logger.warn("parse key:[%s] set expire reply failed. reply string:%s.", key.c_str(), replyInfo.resultString.c_str());
						freeReplyInfo(replyInfo);
						return false;
					}
					//set expire success
					if(replyInfo.replyType == RedisReplyType::REDIS_REPLY_INTEGER && replyInfo.intValue == 1)
					{
						m_logger.debug("set key:%s expire success.",key.c_str());
						freeReplyInfo(replyInfo);
						return true;
					}
					//set expire failed
					else if(replyInfo.replyType == RedisReplyType::REDIS_REPLY_INTEGER && replyInfo.intValue == 0)
					{
						m_logger.warn("set key:%s expire failed.",key.c_str());
						freeReplyInfo(replyInfo);
						return false;
					}
					break;
				case RedisCommandType::REDIS_COMMAND_ZADD:
					if(!parseFindReply(replyInfo,needRedirect,redirectInfo))
					{
						m_logger.warn("parse key:[%s] zset add reply failed. reply string:%s.", key.c_str(), replyInfo.resultString.c_str());
						freeReplyInfo(replyInfo);
						return false;
					}
					// success
					if(replyInfo.replyType == RedisReplyType::REDIS_REPLY_INTEGER && replyInfo.intValue == 1)
					{
						m_logger.debug("zset key:%s add success.",key.c_str());
						freeReplyInfo(replyInfo);
						return true;
					}
					// failed
					else if(replyInfo.replyType == RedisReplyType::REDIS_REPLY_INTEGER && replyInfo.intValue == 0)
					{
						m_logger.debug("zset key:%s add done,maybe exists.",key.c_str());
						freeReplyInfo(replyInfo);
						return false;
					}				
					break;
				case RedisCommandType::REDIS_COMMAND_ZREM:
					if(!parseFindReply(replyInfo,needRedirect,redirectInfo))
					{
						m_logger.warn("parse key:[%s] zset rem reply failed. reply string:%s.", key.c_str(), replyInfo.resultString.c_str());
						freeReplyInfo(replyInfo);
						return false;
					}
					// success
					if(replyInfo.replyType == RedisReplyType::REDIS_REPLY_INTEGER && replyInfo.intValue == 1)
					{
						m_logger.debug("set key:%s rem success.",key.c_str());
						freeReplyInfo(replyInfo);
						return true;
					}
					// failed
					else if(replyInfo.replyType == RedisReplyType::REDIS_REPLY_INTEGER && replyInfo.intValue == 0)
					{
						m_logger.warn("set key:%s rem failed.",key.c_str());
						freeReplyInfo(replyInfo);
						return false;
					}

					break;
				case RedisCommandType::REDIS_COMMAND_ZINCRBY:
					if (checkIfNeedRedirect(replyInfo, needRedirect, redirectInfo))
					{
						m_logger.info("need direct to cluster:[%s].", redirectInfo.c_str());
					}
					else
					{
						// success
						if(replyInfo.replyType == RedisReplyType::REDIS_REPLY_STRING)
						{
							m_logger.debug("set key:%s zincrby success.",key.c_str());
							freeReplyInfo(replyInfo);
							return true;
						}
						// failed
						else
						{
							m_logger.warn("set key:%s zincrby failed.",key.c_str());
							freeReplyInfo(replyInfo);
							return false;
						}
					}
					break;

				case RedisCommandType::REDIS_COMMAND_ZREMRANGEBYSCORE:
					if(!parseFindReply(replyInfo,needRedirect,redirectInfo))
					{
						m_logger.warn("parse key:[%s] zset zremrangebyscore reply failed. reply string:%s.", key.c_str(), replyInfo.resultString.c_str());
						freeReplyInfo(replyInfo);
						return false;
					}
					// success
					if(replyInfo.replyType == RedisReplyType::REDIS_REPLY_INTEGER && replyInfo.intValue > 0)
					{
						m_logger.debug("set key:%s zremrangebyscore success.",key.c_str());
						freeReplyInfo(replyInfo);
						return true;
					}
					// failed
					else if(replyInfo.replyType == RedisReplyType::REDIS_REPLY_INTEGER && replyInfo.intValue == 0)
					{
						m_logger.warn("set key:%s zremrangebyscore failed.",key.c_str());
						freeReplyInfo(replyInfo);
						return false;
					}
					break;
				case RedisCommandType::REDIS_COMMAND_ZCOUNT:
                case RedisCommandType::REDIS_COMMAND_DBSIZE:
				case RedisCommandType::REDIS_COMMAND_ZCARD:
				case RedisCommandType::REDIS_COMMAND_SCARD:
				case RedisCommandType::REDIS_COMMAND_SADD:
				case RedisCommandType::REDIS_COMMAND_SREM:
				case RedisCommandType::REDIS_COMMAND_SISMEMBER:
					if(!parseFindReply(replyInfo,needRedirect,redirectInfo))
					{
						m_logger.warn("parse key:[%s] zset add reply failed. reply string:%s.", key.c_str(), replyInfo.resultString.c_str());
						freeReplyInfo(replyInfo);
						return -1;
					}
					
					if(replyInfo.replyType == RedisReplyType::REDIS_REPLY_INTEGER)
					{
						int count = replyInfo.intValue;
						m_logger.info("key %s commandType %d success, return integer %d", key.c_str(), commandType, count);
						freeReplyInfo(replyInfo);
						return count;
					}
					break;
				case RedisCommandType::REDIS_COMMAND_ZSCORE:
					if (checkIfNeedRedirect(replyInfo, needRedirect, redirectInfo))
					{
						m_logger.info("need direct to cluster:[%s].", redirectInfo.c_str());
					}
					else
					{	
						if (replyInfo.replyType != RedisReplyType::REDIS_REPLY_STRING)
						{
							m_logger.warn("recv redis wrong reply type:[%d].", replyInfo.replyType);
							freeReplyInfo(replyInfo);
							return -1;
						}
						list<ReplyArrayInfo>::iterator iter = replyInfo.arrayList.begin();
						if (iter == replyInfo.arrayList.end())
						{
							m_logger.warn("reply not have array info.");
							freeReplyInfo(replyInfo);
							return -1;
						}
						if ((*iter).replyType == RedisReplyType::REDIS_REPLY_NIL)
						{
							m_logger.warn("get failed,the key not exist.");
							freeReplyInfo(replyInfo);
							return -1;
						}
						char score_c[64] = {0};
						memcpy(score_c,(*iter).arrayValue,(*iter).arrayLen);
						int score = atoi(score_c);
						m_logger.info("key:%s,score:%d",key.c_str(),score);
						return score;	
					}
					break;
				default:
					m_logger.warn("recv unknown command type:%d", commandType);
					break;
			}
			
			freeReplyInfo(replyInfo);
			if (needRedirect)
			{
				m_logger.info("key:[%s] need redirect to cluster:[%s].", key.c_str(), redirectInfo.c_str());
				clusterId = redirectInfo;
				//check cluster redirect if exist.
				vector<string>::iterator reIter;
				reIter = ::find(redirects.begin(), redirects.end(), redirectInfo);
				if (reIter == redirects.end())
				{
					redirects.push_back(redirectInfo);
                    // reset clusterId by redirectInfo
                    if(getClusterIdFromRedirectReply(redirectInfo, clusterId))
                    {
                        bakClusterList.clear();
    					goto REDIS_COMMAND;
                    }
                    else
                    {
                        m_logger.error("no cluster of redirect info %s", redirectInfo.c_str());
                        return false;
                    }
				}
				else
				{
					m_logger.warn("redirect:%s is already do redis command,the slot:[%d] may be removed by redis.please check it.", redirectInfo.c_str(), crcValue);
				}
			}
		}
		else
		{
			m_logger.warn("key:[%s] hit slot:[%d], but not find cluster:[%s].", key.c_str(), crcValue, clusterId.c_str());
			return false;
		}
	}
	else
	{
		m_logger.warn("key:[%s] hit slot:[%d] select cluster failed.", key.c_str(), crcValue);
		return false;
	}
	return true;
}

bool RedisClient::getClusterIdFromRedirectReply(const string& redirectInfo, string& clusterId)
{
    string::size_type pos=redirectInfo.find(":");
    if(pos==string::npos)
        return false;
    string redirectIp=redirectInfo.substr(0, pos);
    string redirectPort=redirectInfo.substr(pos+1);
    for(REDIS_CLUSTER_MAP::iterator it=m_clusterMap.begin(); it!=m_clusterMap.end(); it++)
    {
        if(it->second.connectIp==redirectIp  &&  (uint16_t)(atoi(redirectPort.c_str()))==(uint16_t)it->second.connectPort)
        {
            clusterId=it->second.clusterId;
            return true;
        }
    }
    return false;
}

bool RedisClient::doRedisCommandWithLock(const string & key,int32_t commandLen,list < RedisCmdParaInfo > & commandList,int commandType,RedisLockInfo & lockInfo,bool getSerial,DBSerialize * serial)
{
	assert(m_redisMode==CLUSTER_MODE);
	//first calc crc16 value.
	uint16_t crcValue = crc16(key.c_str(), key.length());
	crcValue %= REDIS_SLOT_NUM;
	string clusterId;
	if (lockInfo.clusterId.empty())
	{
		if (!getClusterIdBySlot(crcValue, clusterId))
		{
			m_logger.warn("key:[%s] hit slot:[%d] select cluster failed.", key.c_str(), crcValue);
			return false;
		}
	}
	else
	{
		clusterId = lockInfo.clusterId;
	}
	//add for redirect end endless loop;
	bool release = false;
	if (commandType == RedisCommandType::REDIS_COMMAND_EXEC || commandType == RedisCommandType::REDIS_COMMAND_UNWATCH)
	{
		release = true;
	}
	vector<string> redirects;
REDIS_COMMAND:
	RedisReplyInfo replyInfo;
	m_logger.debug("key:[%s] hit slot:[%d] select cluster[%s].", key.c_str(), crcValue, clusterId.c_str());
	list<string> bakClusterList;
	bakClusterList.clear();
	list<string>::iterator bakIter;

	//get cluster
	RedisClusterInfo clusterInfo;
	if (getRedisClusterInfo(clusterId,clusterInfo))
	{
		if(!clusterInfo.clusterHandler->doRedisCommandOneConnection(commandList, commandLen, replyInfo, release, &lockInfo.connection))
		{
			freeReplyInfo(replyInfo);
			m_logger.warn("cluster:%s do redis command failed.", clusterId.c_str());
			//need check 
			if (!lockInfo.clusterId.empty())
			{
				m_logger.warn("the transaction must do in one connection.");
				return false;
			}
			//need send to another cluster. check bak cluster.
			if (clusterInfo.bakClusterList.size() != 0)
			{
				bakClusterList = clusterInfo.bakClusterList;
				bakIter = bakClusterList.begin();
				if (bakIter != bakClusterList.end())
				{
					clusterId = (*bakIter);
					goto REDIS_COMMAND;
				}
				else
				{
					m_logger.warn("key:[%s] send to all redis cluster failed, may be slot:[%d] is something wrong", key.c_str(), crcValue);
					return false;
				}
			}
			else
			{
				if (bakClusterList.size() > 0)
				{
					bakIter++;
					if (bakIter != bakClusterList.end())
					{
						clusterId = (*bakIter);
						goto REDIS_COMMAND;
					}
					else
					{
						m_logger.warn("key:[%s] send to all redis cluster failed, may be slot:[%d] is something wrong", key.c_str(), crcValue);
						return false;
					}
				}
			}
		}
		bool needRedirect = false;
		string redirectInfo;
		redirectInfo.clear();
		m_logger.debug("start to parse command type:%d redis reply.", commandType);
		//switch command type
		switch (commandType)
		{
			case RedisCommandType::REDIS_COMMAND_GET:
				if(!parseGetSerialReply(replyInfo,*serial,needRedirect,redirectInfo))
				{
					m_logger.warn("parse key:[%s] get string reply failed. reply string:%s.", key.c_str(), replyInfo.resultString.c_str());
					freeReplyInfo(replyInfo);
					return false;
				}
				break;
			case RedisCommandType::REDIS_COMMAND_SET:
				if(!parseSetSerialReply(replyInfo,needRedirect,redirectInfo))
				{
					m_logger.warn("parse key:[%s] set serial reply failed. reply string:%s.", key.c_str(), replyInfo.resultString.c_str());
					freeReplyInfo(replyInfo);
					return false;
				}
				break;
			case RedisCommandType::REDIS_COMMAND_WATCH:
				if(!parseStatusResponseReply(replyInfo,needRedirect,redirectInfo))
				{
					m_logger.warn("parse key:[%s] watch reply failed. reply string:%s.", key.c_str(), replyInfo.resultString.c_str());
					freeReplyInfo(replyInfo);
					return false;
				}
				break;
			case RedisCommandType::REDIS_COMMAND_UNWATCH:
				if(!parseStatusResponseReply(replyInfo,needRedirect,redirectInfo))
				{
					m_logger.warn("parse key:[%s] unwatch reply failed. reply string:%s.", key.c_str(), replyInfo.resultString.c_str());
					freeReplyInfo(replyInfo);
					return false;
				}
				//check UNWATCH reply,if not OK,need free connection.
				if (replyInfo.resultString != "OK")
				{
					m_logger.info("do unwatch command reply is not OK, need free connection.");
					clusterInfo.clusterHandler->freeConnection(lockInfo.connection);
				}
				break;
			case RedisCommandType::REDIS_COMMAND_MULTI:
				if(!parseStatusResponseReply(replyInfo,needRedirect,redirectInfo))
				{
					m_logger.warn("parse key:[%s] multi reply failed. reply string:%s.", key.c_str(), replyInfo.resultString.c_str());
					freeReplyInfo(replyInfo);
					return false;
				}
				break;
			case RedisCommandType::REDIS_COMMAND_EXEC:
				if(!parseExecReply(replyInfo,needRedirect,redirectInfo))
				{
					m_logger.warn("parse key:[%s] exec reply failed. reply string:%s.", key.c_str(), replyInfo.resultString.c_str());
					freeReplyInfo(replyInfo);
					return false;
				}
				break;
			default:
				m_logger.warn("recv unknown command type:%d", commandType);
				break;
		}
			
		freeReplyInfo(replyInfo);
		if (needRedirect)
		{
			m_logger.info("key:[%s] need redirect to cluster:[%s].", key.c_str(), redirectInfo.c_str());
			//
			if (!lockInfo.clusterId.empty())
			{
				m_logger.warn("the transaction must do in one connection.");
				return false;
			}
			clusterId = redirectInfo;
			//check cluster redirect if exist.
			vector<string>::iterator reIter;
			reIter = ::find(redirects.begin(), redirects.end(), redirectInfo);
			if (reIter == redirects.end())
			{
				redirects.push_back(redirectInfo);
				goto REDIS_COMMAND;
			}
			else
			{
				m_logger.warn("redirect:%s is already do redis command,the slot:[%d] may be removed by redis.please check it.", redirectInfo.c_str(), crcValue);
			}
		}
	}
	else
	{
		m_logger.warn("key:[%s] hit slot:[%d], but not find cluster:[%s].", key.c_str(), crcValue, clusterId.c_str());
		return false;
	}
	if (lockInfo.clusterId.empty())
		lockInfo.clusterId = clusterId;
	return true;
}


// do command which return an array of string
bool RedisClient::doCommandArray(const string & key,int32_t commandLen,list < RedisCmdParaInfo > & commandList, int commandType,list<string>& members)
{
	if(m_redisMode==CLUSTER_MODE)
	{
		return doCommandArrayCluster(key, commandLen, commandList, commandType, members);
	}
	else
	{
		return doCommandArrayProxy(key, commandLen, commandList, commandType, members);
	}
}

bool RedisClient::doCommandArrayProxy(const string & key,int32_t commandLen,list < RedisCmdParaInfo > & commandList, int commandType, list<string>& members)
{
	RedisReplyInfo replyInfo;
	bool ret=false;
    //m_logger.info("m_redisProxy : proxyId %s, connectIp %s, connnecPort %d, connectionNum %d, keepaliveTime %d, clusterHandler %x", m_redisProxy.proxyId.c_str(), m_redisProxy.connectIp.c_str(), m_redisProxy.connectPort, m_redisProxy.connectionNum, m_redisProxy.keepaliveTime, m_redisProxy.clusterHandler);
    if(m_redisProxy.clusterHandler==NULL)
    {
        m_logger.error("m_redisProxy.clusterHandler is NULL");
        return false;
    }
	if(!m_redisProxy.clusterHandler->doRedisCommand(commandList, commandLen, replyInfo))
	{
		freeReplyInfo(replyInfo);
		m_logger.warn("proxy:%s do redis command failed.", m_redisProxy.proxyId.c_str());
		return false;
	}
	switch(commandType)
	{
		case RedisCommandType::REDIS_COMMAND_ZRANGEBYSCORE:
		case RedisCommandType::REDIS_COMMAND_SMEMBERS:
			ret=parseGetKeysReply(replyInfo, members);
			break;
		default:
			m_logger.warn("unsupported commandType %d",commandType);
			break;
	}
	return ret;
}


bool RedisClient::doCommandArrayCluster(const string & key,int32_t commandLen,list < RedisCmdParaInfo > & commandList, int commandType, list<string>& members)
{
	assert(m_redisMode==CLUSTER_MODE);
	//first calc crc16 value.
	uint16_t crcValue = crc16(key.c_str(), key.length());
	crcValue %= REDIS_SLOT_NUM;
	string clusterId;
	if (getClusterIdBySlot(crcValue, clusterId))
	{
		//add for redirect end endless loop;
		vector<string> redirects;
REDIS_COMMAND:
		RedisReplyInfo replyInfo;
		m_logger.debug("key:[%s] hit slot:[%d] select cluster[%s].", key.c_str(), crcValue, clusterId.c_str());
		list<string> bakClusterList;
		bakClusterList.clear();
		list<string>::iterator bakIter;

		//get cluster
		RedisClusterInfo clusterInfo;
		if (getRedisClusterInfo(clusterId,clusterInfo))
		{
			if(!clusterInfo.clusterHandler->doRedisCommand(commandList, commandLen, replyInfo))
			{
				freeReplyInfo(replyInfo);
				m_logger.warn("cluster:%s do redis command failed.", clusterId.c_str());
				//need send to another cluster. check bak cluster.
				if (clusterInfo.bakClusterList.size() != 0)
				{
					bakClusterList = clusterInfo.bakClusterList;
					bakIter = bakClusterList.begin();
					if (bakIter != bakClusterList.end())
					{
						clusterId = (*bakIter);
						goto REDIS_COMMAND;
					}
					else
					{
						m_logger.warn("key:[%s] send to all redis cluster failed, may be slot:[%d] is something wrong", key.c_str(), crcValue);
						return false;
					}
				}
				else
				{
					if (bakClusterList.size() > 0)
					{
						bakIter++;
						if (bakIter != bakClusterList.end())
						{
							clusterId = (*bakIter);
							goto REDIS_COMMAND;
						}
						else
						{
							m_logger.warn("key:[%s] send to all redis cluster failed, may be slot:[%d] is something wrong", key.c_str(), crcValue);
							return false;
						}
					}
				}
			}
			bool needRedirect = false;
			string redirectInfo;
			redirectInfo.clear();

			if (checkIfNeedRedirect(replyInfo, needRedirect, redirectInfo))
			{
				m_logger.info("need direct to cluster:[%s].", redirectInfo.c_str());
			}
			else
			{
				parseGetKeysReply(replyInfo, members);
			}
											
			freeReplyInfo(replyInfo);
			if (needRedirect)
			{
				m_logger.info("key:[%s] need redirect to cluster:[%s].", key.c_str(), redirectInfo.c_str());
				clusterId = redirectInfo;
				//check cluster redirect if exist.
				vector<string>::iterator reIter;
				reIter = ::find(redirects.begin(), redirects.end(), redirectInfo);
				if (reIter == redirects.end())
				{
					redirects.push_back(redirectInfo);
					goto REDIS_COMMAND;
				}
				else
				{
					m_logger.warn("redirect:%s is already do redis command,the slot:[%d] may be removed by redis.please check it.", redirectInfo.c_str(), crcValue);
				}
			}
		}
		else
		{
			m_logger.warn("key:[%s] hit slot:[%d], but not find cluster:[%s].", key.c_str(), crcValue, clusterId.c_str());
			return false;
		}
	}
	else
	{
		m_logger.warn("key:[%s] hit slot:[%d] select cluster failed.", key.c_str(), crcValue);
		return false;
	}
	return true;
}

// redis-server> scan m_cursor MATCH m_redisDbPre+index* COUNT count
void RedisClient::fillScanCommandPara(int cursor, const string& queryKey, 
		int count, list<RedisCmdParaInfo>& paraList, int32_t& paraLen, 
		ScanMode scanMode)
{
	int int32_t_max_size = 10; // 4 294 967 296, 10 chars
	paraList.clear();
	paraLen=0;
	string str="scan";
	fillCommandPara(str.c_str(), str.size(), paraList);
	paraLen += str.size()+int32_t_max_size+1;

	char buf[20];
	if(scanMode == SCAN_NOLOOP)
		cursor = 0;
	int len=snprintf(buf, sizeof(buf)/sizeof(char), "%d", cursor);
	fillCommandPara(buf, len, paraList);
	paraLen += len+int32_t_max_size+1;

	str="MATCH";
	fillCommandPara(str.c_str(), str.size(), paraList);;
	paraLen +=str.size()+int32_t_max_size+1;

	string key=queryKey+"*";
	fillCommandPara(key.c_str(), key.length(), paraList);
	paraLen += key.length() +int32_t_max_size+1;

	str="COUNT";
	fillCommandPara(str.c_str(), str.size(), paraList);
	paraLen += str.size() +int32_t_max_size+1;

	len=snprintf(buf, sizeof(buf)/sizeof(char), "%d", count);
	fillCommandPara(buf, len, paraList);
	paraLen += len+int32_t_max_size+1;
}


bool RedisClient::scanKeys(const string& queryKey, uint32_t count, list<string> &keys, ScanMode scanMode)
{
	uint32_t COUNT = 10000; // TODO change according to db size or a maximum number
	list<RedisCmdParaInfo> paraList;
	int32_t paraLen =0;
	REDIS_CLUSTER_MAP clusterMap;
	getRedisClusters(clusterMap);
	REDIS_CLUSTER_MAP::iterator clusterIter;
	int retCursor=-1;
	m_logger.debug("clusterMap size is %d", clusterMap.size());
	for (clusterIter = clusterMap.begin(); clusterIter != clusterMap.end(); /*clusterIter++*/)
	{
		string clusterId = (*clusterIter).first;
		RedisClusterInfo clusterInfo;
		if(!getRedisClusterInfo(clusterId, clusterInfo))
		{
			m_logger.warn("get redis cluster:[%s] info failed.", clusterId.c_str());
			continue;
		}
		
		if (clusterInfo.isMaster && clusterInfo.isAlived)
		{
			m_logger.debug("redis %s is master, alive", clusterId.c_str());
			
			RedisReplyInfo replyInfo;
			fillScanCommandPara(clusterInfo.scanCursor, queryKey, COUNT, paraList, paraLen, scanMode);
			if(!clusterInfo.clusterHandler->doRedisCommand(paraList, paraLen, replyInfo, RedisConnection::SCAN_PARSER)) 
			{
				freeReplyInfo(replyInfo);
				m_logger.debug("%s doRedisCommand failed, send bak redis cluster", clusterId.c_str());

				list<string>::iterator bakIter;
				for (bakIter = clusterInfo.bakClusterList.begin(); bakIter != clusterInfo.bakClusterList.end(); /*bakIter++*/)
				{
					RedisClusterInfo bakClusterInfo;
					if(!getRedisClusterInfo((*bakIter), bakClusterInfo))
					{
						m_logger.warn("get bak redis cluster:[%s] info.", (*bakIter).c_str());
						bakIter++;
						continue;
					}
					fillScanCommandPara(bakClusterInfo.scanCursor, queryKey, COUNT, paraList, paraLen, scanMode);
					if(!bakClusterInfo.clusterHandler->doRedisCommand(paraList, paraLen, replyInfo, RedisConnection::SCAN_PARSER))
					{
						freeReplyInfo(replyInfo);
						m_logger.warn("redis cluster %s, bak redis cluster %s doRedisCommand failed", clusterInfo.clusterId.c_str(), bakClusterInfo.clusterId.c_str());
						bakIter++;
						continue;
					}
					else
					{
						if(parseScanKeysReply(replyInfo, keys, retCursor))
							updateClusterCursor(clusterId, retCursor);
						freeReplyInfo(replyInfo);
						if(keys.size()>=count)
						{
							m_logger.debug("get enough %d keys");
							break;
						}
						else if(retCursor!=0)
						{
							// continue scan this bak cluster
							continue;
						}
					}					
				}
			}
			else
			{
				if(parseScanKeysReply(replyInfo, keys, retCursor))
					updateClusterCursor(clusterId, retCursor);
				freeReplyInfo(replyInfo);
				if(keys.size()>=count)
				{
					m_logger.debug("get enough %d keys");
					break;
				}
				else if(retCursor!=0)
				{
					// continue scan this cluster
					continue;
				}
			}
		}
		else
		{
			m_logger.debug("redis %s is not alive master, isMaster %d, isAlive %d", clusterId.c_str(), clusterInfo.isMaster, clusterInfo.isAlived);	
		}
		if(keys.size()>=count)
		{
			m_logger.debug("get enough %d keys");
			break;
		}
		clusterIter++; // scan next cluster
	}	
	freeCommandList(paraList);
	m_logger.debug("scan keys get %d keys", keys.size());
	return keys.size()>0 ? true : false;
}

bool RedisClient::getKeys(const string & queryKey,list < string > & keys)
{
	//need send to all master cluster
	list<RedisCmdParaInfo> paraList;
	int32_t paraLen = 0;
	fillCommandPara("keys", 4, paraList);
	paraLen += 15;
	string key = queryKey + "*";
	fillCommandPara(key.c_str(), key.length(), paraList);
	paraLen += key.length() + 20;
	//
	REDIS_CLUSTER_MAP clusterMap;
	getRedisClusters(clusterMap);
	REDIS_CLUSTER_MAP::iterator clusterIter;
	for (clusterIter = clusterMap.begin(); clusterIter != clusterMap.end(); clusterIter++)
	{
		string clusterId = (*clusterIter).first;
		RedisClusterInfo clusterInfo;
		if(!getRedisClusterInfo(clusterId, clusterInfo))
		{
			m_logger.warn("get redis cluster:[%s] info.", clusterId.c_str());
			continue;
		}
		if (clusterInfo.isMaster && clusterInfo.isAlived)
		{
			RedisReplyInfo replyInfo;
			if(!clusterInfo.clusterHandler->doRedisCommand(paraList, paraLen, replyInfo))
			{
				freeReplyInfo(replyInfo);
				//need send to backup cluster.
				if (clusterInfo.bakClusterList.size() != 0)
				{
					bool sendBak = false;
					list<string>::iterator bakIter;
					for (bakIter = clusterInfo.bakClusterList.begin(); bakIter != clusterInfo.bakClusterList.end(); bakIter++)
					{
						RedisClusterInfo bakClusterInfo;
						if(!getRedisClusterInfo((*bakIter), bakClusterInfo))
						{
							m_logger.warn("get bak redis cluster:[%s] info.", (*bakIter).c_str());
							continue;
						}
						if(!bakClusterInfo.clusterHandler->doRedisCommand(paraList, paraLen, replyInfo))
						{
							freeReplyInfo(replyInfo);
							continue;
						}
						else
						{
							sendBak = true;
						}
					}
					if (!sendBak)
					{
						continue;
					}
				}
			}
			parseGetKeysReply(replyInfo, keys);
			freeReplyInfo(replyInfo);
		}
	}
	freeCommandList(paraList);
	return true;
}

//bool RedisClient::getSerials(const string& queryKey, map<string,RedisSerialize> &serials)
//{
//	list<string> keys;
//	getKeys(queryKey,keys);
//	list<string>::iterator it;
//	for(it = keys.begin();it != keys.end();it++)
//	{
//		RedisSerialize serial;
//		if(getSerial((*it),serial))
//		{
//			serials[(*it)] = serial;
//		}
//	}
//	keys.clear();	
//	return true;
//}


void RedisClient::getRedisClusters(REDIS_CLUSTER_MAP & clusterMap)
{
	ReadGuard guard(m_rwClusterMutex);
	clusterMap = m_clusterMap;
	return;
}

bool RedisClient::getRedisClustersByCommand(REDIS_CLUSTER_MAP & clusterMap)
{
	assert(m_redisMode==CLUSTER_MODE);
	bool success = false;
	//send cluster nodes.
	list<RedisCmdParaInfo> paraList;
	int32_t paraLen = 0;
	fillCommandPara("cluster", 7, paraList);
	fillCommandPara("nodes", 5, paraList);
	paraLen += 30;
	REDIS_CLUSTER_MAP oldClusterMap;
	getRedisClusters(oldClusterMap);
	REDIS_CLUSTER_MAP::iterator clusterIter;
	for (clusterIter = oldClusterMap.begin(); clusterIter != oldClusterMap.end(); clusterIter++)
	{
		RedisClusterInfo oldClusterInfo;
		string clusterId = (*clusterIter).first;
		if(!getRedisClusterInfo(clusterId, oldClusterInfo))
		{
			m_logger.warn("get cluster:%s info failed.", clusterId.c_str());
			continue;
		}
		RedisReplyInfo replyInfo;
		if (!oldClusterInfo.clusterHandler->doRedisCommand(paraList, paraLen, replyInfo))
		{
			m_logger.warn("do get cluster nodes failed.");
			continue;
		}
		if (replyInfo.replyType == RedisReplyType::REDIS_REPLY_ERROR)
		{
			m_logger.warn("recv redis error response:[%s].", replyInfo.resultString.c_str());
			freeReplyInfo(replyInfo);
			freeCommandList(paraList);
			return false;
		}
		if (replyInfo.replyType != RedisReplyType::REDIS_REPLY_STRING)
		{
			m_logger.warn("recv redis wrong reply type:[%d].", replyInfo.replyType);
			freeReplyInfo(replyInfo);
			freeCommandList(paraList);
			return false;
		}
		//check if master,slave parameter may be not newest.
		list<ReplyArrayInfo>::iterator arrayIter = replyInfo.arrayList.begin();
		string str = (*arrayIter).arrayValue;
		if (str.find("myself,slave") != string::npos)
		{
			m_logger.info("the redis cluster:[%s] is slave. not use it reply,look for master.", (*clusterIter).first.c_str());
			freeReplyInfo(replyInfo);
			success = false;
			continue;
		}
		if (!parseClusterInfo(replyInfo, clusterMap))
		{
			m_logger.error("parse cluster info from redis reply failed.");
			freeCommandList(paraList);
			return false;
		}
		freeReplyInfo(replyInfo);
		success = true;
		break;
	}
	freeCommandList(paraList);
	return success;
}

bool RedisClient::checkAndSaveRedisClusters(REDIS_CLUSTER_MAP & clusterMap)
{
	WriteGuard guard(m_rwClusterMutex);
	REDIS_CLUSTER_MAP::iterator clusterIter;
	{
		WriteGuard guard(m_rwSlotMutex);
		m_slotMap.clear();
		for (clusterIter = clusterMap.begin(); clusterIter != clusterMap.end(); clusterIter++)
		{
			string clusterId = (*clusterIter).first;
			RedisClusterInfo clusterInfo = (*clusterIter).second;
			if (m_clusterMap.find(clusterId) != m_clusterMap.end())
			{
				RedisClusterInfo oldClusterInfo = m_clusterMap[clusterId];
				clusterInfo.clusterHandler = oldClusterInfo.clusterHandler;
				clusterInfo.connectionNum = oldClusterInfo.connectionNum;
				clusterInfo.keepaliveTime = oldClusterInfo.keepaliveTime;
				m_clusterMap[clusterId] = clusterInfo;
				if (clusterInfo.isMaster)
				{
					map<uint16_t,uint16_t>::iterator iter;
					for(iter = clusterInfo.slotMap.begin(); iter != clusterInfo.slotMap.end(); iter++)
					{
						uint16_t startSlotNum = (*iter).first;
						uint16_t stopSlotNum = (*iter).second;
						for(int i = startSlotNum; i <= stopSlotNum; i++)
						{
							m_slotMap[i] = clusterId;
						}
					}
				}
			}
			else
			{
				//need create new connect pool.
				clusterInfo.clusterHandler = new RedisCluster();
				clusterInfo.connectionNum = m_connectionNum;
				clusterInfo.keepaliveTime = m_keepaliveTime;
				if (!clusterInfo.clusterHandler->initConnectPool(clusterInfo.connectIp, clusterInfo.connectPort, clusterInfo.connectionNum, clusterInfo.keepaliveTime))
				{
					m_logger.warn("init cluster:[%s] connect pool failed.", clusterId.c_str());
					return false;
				}
				m_clusterMap[clusterId] = clusterInfo;
				if (clusterInfo.isMaster)
				{
					map<uint16_t,uint16_t>::iterator iter;
					for(iter = clusterInfo.slotMap.begin(); iter != clusterInfo.slotMap.end(); iter++)
					{
						uint16_t startSlotNum = (*iter).first;
						uint16_t stopSlotNum = (*iter).second;
						for(int i = startSlotNum; i <= stopSlotNum; i++)
						{
							m_slotMap[i] = clusterId;
						}
					}
				}
			}
		}
	}
	//check old cluster map if need free.
	list<string> unusedClusters;
	unusedClusters.clear();
	for (clusterIter = m_clusterMap.begin(); clusterIter != m_clusterMap.end(); clusterIter++)
	{
		string clusterId = (*clusterIter).first;
		RedisClusterInfo clusterInfo = (*clusterIter).second;
		if (clusterMap.find(clusterId) == clusterMap.end())
		{
			m_unusedHandlers[clusterId] = clusterInfo.clusterHandler;
			unusedClusters.push_back(clusterId);
		}
	}
	list<string>::iterator unusedIter;
	for (unusedIter = unusedClusters.begin(); unusedIter != unusedClusters.end(); unusedIter++)
	{
		m_clusterMap.erase((*unusedIter));
	}
	return true;
}

bool RedisClient::setKeyExpireTime(const string & key,uint32_t expireTime)
{
	list<RedisCmdParaInfo> paraList;
	int32_t paraLen = 0;
	fillCommandPara("expire", 6, paraList);
	paraLen += 15;
	fillCommandPara(key.c_str(), key.length(), paraList);
	paraLen += key.length() + 20;
	fillCommandPara(toStr(expireTime).c_str(),toStr(expireTime).length(), paraList);
	paraLen += toStr(expireTime).length() + 20;
	bool success = false;
	success = doRedisCommand( key,paraLen,paraList,RedisCommandType::REDIS_COMMAND_EXPIRE);
	freeCommandList(paraList);
	return success;
}

void RedisClient::releaseUnusedClusterHandler()
{
	WriteGuard guard(m_rwClusterMutex);
	list<string> needFreeClusters;
	needFreeClusters.clear();
	map<string, RedisCluster*>::iterator clusterIter;
	for(clusterIter = m_unusedHandlers.begin(); clusterIter != m_unusedHandlers.end(); clusterIter++)
	{
		if(((*clusterIter).second)->checkIfCanFree())
		{
			needFreeClusters.push_back((*clusterIter).first);
		}
	}
	list<string>::iterator iter;
	for (iter = needFreeClusters.begin(); iter != needFreeClusters.end(); iter++)
	{
		RedisCluster* clusterHandler = m_unusedHandlers[(*iter)];
		clusterHandler->freeConnectPool();
		delete clusterHandler;
		clusterHandler = NULL;
		m_unusedHandlers.erase((*iter));
	}
}

bool RedisClient::getRedisClusterNodes()
{
	assert(m_redisMode==CLUSTER_MODE);
	bool success = false;
	list<RedisCmdParaInfo> paraList;
	int32_t paraLen = 0;
	fillCommandPara("cluster", 7, paraList);
	fillCommandPara("nodes", 5, paraList);
	paraLen += 30;
	REDIS_SERVER_LIST::iterator iter;
	for (iter = m_serverList.begin(); iter != m_serverList.end(); iter++)
	{
		RedisServerInfo serverInfo = (*iter);
		//m_logger.warn("EEEEEconnect to serverIp:[%s] serverPort:[%d].", serverInfo.serverIp.c_str(), serverInfo.serverPort);
		
		RedisConnection *connection = new RedisConnection(serverInfo.serverIp, serverInfo.serverPort, m_keepaliveTime);
		if (!connection->connect())
		{
			m_logger.warn("connect to serverIp:[%s] serverPort:[%d] failed.", serverInfo.serverIp.c_str(), serverInfo.serverPort);
			continue;
		}
		//send cluster nodes.
		RedisReplyInfo replyInfo;
		if (!connection->doRedisCommand(paraList, paraLen, replyInfo))
		{
			m_logger.warn("do get cluster nodes failed.");
			continue;
		}
		if (replyInfo.replyType == RedisReplyType::REDIS_REPLY_ERROR)
		{
			m_logger.warn("recv redis error response:[%s].", replyInfo.resultString.c_str());
			connection->close();
			delete connection;
			freeReplyInfo(replyInfo);
			freeCommandList(paraList);
			return false;
		}
		if (replyInfo.replyType != RedisReplyType::REDIS_REPLY_STRING)
		{
			m_logger.warn("recv redis wrong reply type:[%d].", replyInfo.replyType);
			connection->close();
			delete connection;
			freeReplyInfo(replyInfo);
			freeCommandList(paraList);
			return false;
		}
		//check if master,slave parameter may be not newest.
		list<ReplyArrayInfo>::iterator arrayIter = replyInfo.arrayList.begin();
		string str = (*arrayIter).arrayValue;
//		if (str.find("myself,slave") != string::npos)
//		{
//			m_logger.info("the redis cluster:[%s:%d] is slave. not use it reply,look for master.", serverInfo.serverIp.c_str(), serverInfo.serverPort);
//			freeReplyInfo(replyInfo);
//			connection->close();
//			delete connection;
//			continue;
//		}
		REDIS_CLUSTER_MAP clusterMap;
		if (!parseClusterInfo(replyInfo, clusterMap))
		{
			m_logger.error("parse cluster info from redis reply failed.");
			freeCommandList(paraList);
			return false;
		}
		{
			WriteGuard guard(m_rwClusterMutex);
			m_clusterMap = clusterMap;
		}
		freeReplyInfo(replyInfo);
		connection->close();
		delete connection;
		success = true;
		break;
	}	
	freeCommandList(paraList);
	return success;
}

bool RedisClient::parseClusterInfo(RedisReplyInfo & replyInfo,REDIS_CLUSTER_MAP & clusterMap)
{
	//get reply string
	list<ReplyArrayInfo>::iterator iter = replyInfo.arrayList.begin();
	if (iter == replyInfo.arrayList.end())
	{
		m_logger.warn("reply not have array info.");
		return false;
	}
	if ((*iter).replyType != RedisReplyType::REDIS_REPLY_STRING || (*iter).arrayLen == 0)
	{
		m_logger.error("parse cluster info failed.redis reply info is something wrong,replyType:[%d], arrayLen:[%d].", (*iter).replyType, (*iter).arrayLen);
		return false;
	}
	m_logger.debug("recv redis cluster response:[%s].", (*iter).arrayValue);
	string str = (*iter).arrayValue;
	string::size_type startPos,findPos;
	startPos = findPos = 0;
	findPos = str.find("\n", startPos);
	map<string, string> bakMap; //for key is bak cluster address info, value is master cluster id.
	bakMap.clear();
	while (findPos != string::npos)
	{
		string infoStr = str.substr(startPos, findPos-startPos);
		if (infoStr == "\r" )
			break;
		if(	infoStr.find("fail") != string::npos
			|| infoStr.find("noaddr") != string::npos
			|| infoStr.find("disconnected") != string::npos )
		{
			startPos = findPos + 1;
			findPos = str.find("\n", startPos);		
			continue;
		}
		RedisClusterInfo clusterInfo;
		if (!parseOneCluster(infoStr,clusterInfo))
		{
			m_logger.warn("parse one cluster failed.");
			clusterMap.clear();
			return false;
		}
		string clusterId = clusterInfo.connectIp + ":" + toStr(clusterInfo.connectPort);
		//check if bak cluster node info.
		if (!clusterInfo.isMaster && !clusterInfo.masterClusterId.empty())
		{
			bakMap[clusterId] = clusterInfo.masterClusterId;
		}
		clusterInfo.connectionNum = m_connectionNum;
		clusterInfo.keepaliveTime = m_keepaliveTime;
		clusterMap[clusterId] = clusterInfo;
		startPos = findPos + 1;
		findPos = str.find("\n", startPos);
	}
	//
	map<string, string>::iterator bakIter;
	for (bakIter = bakMap.begin(); bakIter != bakMap.end(); bakIter++)
	{
		REDIS_CLUSTER_MAP::iterator clusterIter;
		for (clusterIter = clusterMap.begin(); clusterIter != clusterMap.end(); clusterIter++)
		{
			if (((*clusterIter).second).clusterId == (*bakIter).second)
			{
				((*clusterIter).second).bakClusterList.push_back((*bakIter).first);
			}
		}
	}
	return true;
}

bool RedisClient::parseOneCluster(const string & infoStr,RedisClusterInfo & clusterInfo)
{
	//first parse node id.
	string::size_type startPos, findPos;
	startPos = findPos = 0;
	findPos = infoStr.find(" ");
	if (findPos != string::npos)
	{
		clusterInfo.clusterId = infoStr.substr(0, findPos);
		startPos = findPos + 1;
		findPos = infoStr.find(" ", startPos);
		if (findPos == string::npos)
		{	
			m_logger.warn("parse one cluster:[%s] failed.", infoStr.c_str());
			return false;
		}
		//parse ip port
		string address = infoStr.substr(startPos, findPos-startPos);
		startPos = findPos + 1;
		findPos = address.find(":");
		if (findPos != string::npos)
		{
			clusterInfo.connectIp = address.substr(0, findPos);
			clusterInfo.connectPort = atoi(address.substr(findPos+1, address.length()-findPos).c_str());
		}
		else
		{
			clusterInfo.connectIp = address;
			clusterInfo.connectPort = REDIS_DEFALUT_SERVER_PORT;
		}
		//parse master slave.
		findPos = infoStr.find(" ", startPos);
		if (findPos == string::npos)
		{
			return false;
		}
		string tmpStr;
		tmpStr = infoStr.substr(startPos, findPos-startPos);
		if (tmpStr.find("master") != string::npos)
		{
			clusterInfo.isMaster = true;
		}
		startPos = findPos + 1;
		findPos = infoStr.find(" ", startPos);
		if (findPos == string::npos)
		{
			return false;
		}
		if (!clusterInfo.isMaster)
		{
			clusterInfo.masterClusterId = infoStr.substr(startPos, findPos-startPos);
		}
		startPos = findPos + 1;
		//first find status.
		findPos = infoStr.find("disconnected", startPos);
		if (findPos != string::npos)
		{
			clusterInfo.isAlived = false;
		}
		else
		{
			clusterInfo.isAlived = true;
		}
		findPos = infoStr.find("connected", startPos);
		if (clusterInfo.isMaster && clusterInfo.isAlived)
		{
			startPos = findPos +1;
			findPos = infoStr.find(" ", startPos);
			if (findPos == string::npos)
				return false;
			startPos = findPos +1;
			findPos = infoStr.find(" ", startPos);
			string slotStr;
			uint16_t startSlotNum, stopSlotNum;
			while (findPos != string::npos)
			{
				slotStr = infoStr.substr(startPos, findPos);
				startSlotNum = stopSlotNum = -1;
				parseSlotStr(slotStr,startSlotNum, stopSlotNum);
				clusterInfo.slotMap[startSlotNum] = stopSlotNum;
				startPos = findPos +1;
				m_logger.info("parse cluster slot success,cluster:[%s] has slot[%d-%d].", clusterInfo.clusterId.c_str(), startSlotNum, stopSlotNum);
				findPos = infoStr.find(" ", startPos);
			}
			slotStr = infoStr.substr(startPos, infoStr.length()-startPos);
			startSlotNum = stopSlotNum = -1;
			parseSlotStr(slotStr,startSlotNum, stopSlotNum);
			clusterInfo.slotMap[startSlotNum] = stopSlotNum;
			m_logger.info("parse cluster slot success,cluster:[%s] has slot[%d-%d].", clusterInfo.clusterId.c_str(), startSlotNum, stopSlotNum);
		}
		m_logger.info("parse cluster:[%s] info success, cluster address:[%s:%d] master:[%d], active:[%d], master cluster id:[%s].", clusterInfo.clusterId.c_str(), \
			clusterInfo.connectIp.c_str(), clusterInfo.connectPort, clusterInfo.isMaster, clusterInfo.isAlived, clusterInfo.masterClusterId.c_str());
	}
	return true;
}

void RedisClient::parseSlotStr(string & slotStr,uint16_t & startSlotNum,uint16_t & stopSlotNum)
{
	string::size_type findPos;
	findPos = slotStr.find("-");
	if (findPos != string::npos)
	{
		startSlotNum = atoi(slotStr.substr(0, findPos).c_str());
		stopSlotNum = atoi(slotStr.substr(findPos+1, slotStr.length()-findPos).c_str());
	}
}

// return true if get "MOVED" and redirectInfo, or false
bool RedisClient::checkIfNeedRedirect(RedisReplyInfo & replyInfo,bool & needRedirect,string & redirectInfo)
{
	//assert(m_redisMode==CLUSTER_MODE);
	if(m_redisMode == PROXY_MODE)
	{
		return false;
	}
	if (replyInfo.replyType == RedisReplyType::REDIS_REPLY_ERROR)
	{
		m_logger.warn("recv redis error response:[%s].", replyInfo.resultString.c_str());
		//check if move response.
		if (strncasecmp(replyInfo.resultString.c_str(), "MOVED", 5) == 0)
		{
			needRedirect = true;
			string::size_type findPos;
			findPos = replyInfo.resultString.find_last_of(" ");
			if (findPos != string::npos)
			{
				redirectInfo = replyInfo.resultString.substr(findPos+1, replyInfo.resultString.length()-findPos-1);
                //m_logger.info("redirectInfo is %s", redirectInfo.c_str());
				return true;
			}
			else
			{
				return false;
			}
		}
	}
	return false;
}

bool RedisClient::parseGetSerialReply(RedisReplyInfo & replyInfo,DBSerialize & serial,bool & needRedirect,string & redirectInfo)
{
	if (checkIfNeedRedirect(replyInfo, needRedirect, redirectInfo))
	{
		m_logger.info("need direct to cluster:[%s].", redirectInfo.c_str());
		return true;
	}	
	if (replyInfo.replyType != RedisReplyType::REDIS_REPLY_STRING)
	{
		m_logger.warn("recv redis wrong reply type:[%d].", replyInfo.replyType);
		return false;
	}
	list<ReplyArrayInfo>::iterator iter = replyInfo.arrayList.begin();
	if (iter == replyInfo.arrayList.end())
	{
		m_logger.warn("reply not have array info.");
		return false;
	}
	if ((*iter).replyType == RedisReplyType::REDIS_REPLY_NIL)
	{
		m_logger.warn("get failed,the key not exist.");
		return false;
	}
	DBInStream in((void*)(*iter).arrayValue, (*iter).arrayLen);
	serial.load(in);
	if (in.m_loadError)
	{
		m_logger.warn("load data from redis error.");
		return false;
	}
	return true;	
}

bool RedisClient::parseFindReply(RedisReplyInfo & replyInfo,bool & needRedirect,string & redirectInfo)
{
	if (checkIfNeedRedirect(replyInfo, needRedirect, redirectInfo))
	{
		m_logger.info("need direct to cluster:[%s].", redirectInfo.c_str());
		return true;
	}
	if(replyInfo.replyType != RedisReplyType::REDIS_REPLY_INTEGER)
	{
		return false;
	}
	return true;
}

bool RedisClient::parseSetSerialReply(RedisReplyInfo & replyInfo,bool & needRedirect,string & redirectInfo)
{
	if (checkIfNeedRedirect(replyInfo, needRedirect, redirectInfo))
	{
		m_logger.info("need direct to cluster:[%s].", redirectInfo.c_str());
		return true;
	}	
	if (replyInfo.replyType != RedisReplyType::REDIS_REPLY_STATUS)
	{
		m_logger.warn("set serial failed, redis response:[%s].", replyInfo.resultString.c_str());
		return false;
	}
	return true;
}

bool RedisClient::parseScanKeysReply(RedisReplyInfo & replyInfo, list<string>& keys, int &retCursor)
{
	retCursor=-1;
	m_logger.debug("parseScanKeysReply, replyInfo has replyType %d, resultString %s, intValue %d", replyInfo.replyType, replyInfo.resultString.c_str(), replyInfo.intValue);	

	if (replyInfo.replyType != RedisReplyType::REDIS_REPLY_ARRAY)
	{
		m_logger.warn("recv redis wrong reply type:[%d].", replyInfo.replyType);
		return false;
	}

	retCursor=replyInfo.intValue;
	list<ReplyArrayInfo>::iterator arrayIter;
	for (arrayIter = replyInfo.arrayList.begin(); arrayIter != replyInfo.arrayList.end(); arrayIter++)
	{
		m_logger.debug("arrayList has replyType %d, arrayValue %s, arrayLen %d", (*arrayIter).replyType, arrayIter->arrayValue, arrayIter->arrayLen);
		
		if ((*arrayIter).replyType == RedisReplyType::REDIS_REPLY_STRING)
		{
			string key = (*arrayIter).arrayValue;
			keys.push_back(key);
		}			
	}
	return true;
}

bool RedisClient::parseGetKeysReply(RedisReplyInfo & replyInfo,list < string > & keys)
{
	m_logger.debug("replyInfo has replyType %d, resultString %s, intValue %d", replyInfo.replyType, replyInfo.resultString.c_str(), replyInfo.intValue);

	if (replyInfo.replyType == RedisReplyType::REDIS_REPLY_ERROR)
	{
		m_logger.info("get empty list or set.");
		return true;
	}

	if (replyInfo.replyType != RedisReplyType::REDIS_REPLY_ARRAY)
	{
		m_logger.warn("recv redis wrong reply type:[%d].", replyInfo.replyType);
		return false;
	}
	
	list<ReplyArrayInfo>::iterator arrayIter;
	for (arrayIter = replyInfo.arrayList.begin(); arrayIter != replyInfo.arrayList.end(); arrayIter++)
	{
		m_logger.debug("arrayList has replyType %d, arrayValue %s, arrayLen %d", (*arrayIter).replyType, arrayIter->arrayValue, arrayIter->arrayLen);
		
		if ((*arrayIter).replyType == RedisReplyType::REDIS_REPLY_STRING)
		{
			string key = (*arrayIter).arrayValue;
			keys.push_back(key);
		}
	}
	return true;
}

//for watch,unwatch,multi command response.
bool RedisClient::parseStatusResponseReply(RedisReplyInfo & replyInfo,bool & needRedirect,string & redirectInfo)
{
	m_logger.debug("replyInfo has replyType %d, resultString %s, intValue %d", replyInfo.replyType, replyInfo.resultString.c_str(), replyInfo.intValue);
	
	if (checkIfNeedRedirect(replyInfo, needRedirect, redirectInfo))
	{
		m_logger.info("need direct to cluster:[%s].", redirectInfo.c_str());
		return true;
	}		
	if (replyInfo.replyType != RedisReplyType::REDIS_REPLY_STATUS)
	{
		m_logger.warn("status response failed, redis response:[%s].", replyInfo.resultString.c_str());
		return false;
	}
	return true;
}

bool RedisClient::parseExecReply(RedisReplyInfo & replyInfo,bool & needRedirect,string & redirectInfo)
{
	m_logger.debug("replyInfo has replyType %d, resultString %s, intValue %d", replyInfo.replyType, replyInfo.resultString.c_str(), replyInfo.intValue);

	if (checkIfNeedRedirect(replyInfo, needRedirect, redirectInfo))
	{
		m_logger.info("need direct to cluster:[%s].", redirectInfo.c_str());
		return true;
	}

	if (replyInfo.replyType != RedisReplyType::REDIS_REPLY_ARRAY)
	{	
		m_logger.warn("recv redis wrong reply type:[%d].", replyInfo.replyType);
		return false;
	}
	//parse exec reply
	if (replyInfo.replyType == RedisReplyType::REDIS_REPLY_ARRAY && replyInfo.intValue == -1)
	{	
		m_logger.warn("exec reply -1,set serial exec failed.");
		return false;
	}
	//parse array list.
	list<ReplyArrayInfo>::iterator arrayIter;
	for (arrayIter = replyInfo.arrayList.begin(); arrayIter != replyInfo.arrayList.end(); arrayIter++)
	{
		m_logger.debug("arrayList has replyType %d, arrayValue %s, arrayLen %d", (*arrayIter).replyType, arrayIter->arrayValue, arrayIter->arrayLen);
		if ((*arrayIter).replyType == RedisReplyType::REDIS_REPLY_STRING)
		{
			if (strncmp((*arrayIter).arrayValue, "-", 1) == 0 || strncmp((*arrayIter).arrayValue, ":0", 2) == 0)
			{
				m_logger.warn("recv failed exec reply:%s.", (*arrayIter).arrayValue);
				return false;
			}
			m_logger.debug("recv exec reply:%s.", (*arrayIter).arrayValue);
		}
	}
	return true;
}

void RedisClient::freeReplyInfo(RedisReplyInfo & replyInfo)
{
	if (replyInfo.arrayList.size() > 0)
	{
		list<ReplyArrayInfo>::iterator iter;
		for (iter = replyInfo.arrayList.begin(); iter != replyInfo.arrayList.end(); iter++)
		{
			if ((*iter).arrayValue != NULL)
			{
				free((*iter).arrayValue);
				(*iter).arrayValue = NULL;
			}
		}
		replyInfo.arrayList.clear();
	}
}

void RedisClient::fillCommandPara(const char * paraValue,int32_t paraLen,list < RedisCmdParaInfo > & paraList)
{
	m_logger.debug("fillCommandPara : paraValue %s, paraLen %d", paraValue, paraLen);
	
	RedisCmdParaInfo paraInfo;
	paraInfo.paraLen = paraLen;
	paraInfo.paraValue = (char*)malloc(paraLen+1);
	memset(paraInfo.paraValue, 0, paraLen+1);
	memcpy(paraInfo.paraValue, paraValue, paraLen);
	paraList.push_back(paraInfo);
}

void RedisClient::freeCommandList(list < RedisCmdParaInfo > & paraList)
{
	list<RedisCmdParaInfo>::iterator commandIter;
	for (commandIter = paraList.begin(); commandIter != paraList.end(); commandIter++)
	{
		free((*commandIter).paraValue);
		(*commandIter).paraValue = NULL;
	}
	paraList.clear();
}

bool RedisClient::getRedisClusterInfo(string & clusterId,RedisClusterInfo & clusterInfo)
{
	ReadGuard guard(m_rwClusterMutex);
	if (m_clusterMap.find(clusterId) != m_clusterMap.end())
	{
		clusterInfo = m_clusterMap[clusterId];
		return true;
	}
	m_logger.warn("not find cluster:%s info.", clusterId.c_str());
	return false;
}

void RedisClient::updateClusterCursor(const string& clusterId, int newcursor)
{
	if(newcursor<0)
		return;
	ReadGuard guard(m_rwClusterMutex);
	if (m_clusterMap.find(clusterId) != m_clusterMap.end())
	{
		m_clusterMap[clusterId].scanCursor=newcursor;
		return;
	}
	m_logger.warn("updateClusterCursor non-exist redis cluster %s", clusterId.c_str());
}


bool RedisClient::getClusterIdBySlot(uint16_t slotNum,string & clusterId)
{
	assert(m_redisMode==CLUSTER_MODE);
	ReadGuard guard(m_rwSlotMutex);
	if (m_slotMap.find(slotNum) != m_slotMap.end())
	{
		clusterId = m_slotMap[slotNum];
		m_logger.info("slot:%u hit clusterId:%s.", slotNum, clusterId.c_str());
		return true;
	}
	m_logger.warn("slot:%u not hit any cluster.please check redis cluster.", slotNum);
	return false;
}

bool RedisClient::zadd(const string& key,const string& member, int score)
{
    list<RedisCmdParaInfo> paraList;
    int32_t paraLen = 0;
    fillCommandPara("zadd", 4, paraList);
    paraLen += 16;
    fillCommandPara(key.c_str(), key.length(), paraList);
    paraLen += key.length() + 20;
    
    char score_c[64] = {0};
    sprintf(score_c,"%d",score);
    fillCommandPara(score_c, strlen(score_c), paraList);
    paraLen += strlen(score_c) + 20;
    
    fillCommandPara(member.c_str(), member.length(), paraList);
    paraLen += member.length() + 20;

    bool success = false;
    success = doRedisCommand( key,paraLen,paraList,RedisCommandType::REDIS_COMMAND_ZADD);
    freeCommandList(paraList);
    return success;
}

bool RedisClient::zrem(const string& key,const string& member)
{
    list<RedisCmdParaInfo> paraList;
    int32_t paraLen = 0;
    fillCommandPara("zrem", 4, paraList);
    paraLen += 16;
    fillCommandPara(key.c_str(), key.length(), paraList);
    paraLen += key.length() + 20;
    fillCommandPara(member.c_str(), member.length(), paraList);
    paraLen += member.length() + 20;

    bool success = false;
    success = doRedisCommand( key,paraLen,paraList,RedisCommandType::REDIS_COMMAND_ZREM);
    freeCommandList(paraList);
    return success;
}

bool RedisClient::zincby(const string& key,const string& member,int increment)
{
    list<RedisCmdParaInfo> paraList;
    int32_t paraLen = 0;
    fillCommandPara("zincrby", 7, paraList);
    paraLen += 19;
    fillCommandPara(key.c_str(), key.length(), paraList);
    paraLen += key.length() + 20;
    
    char inc[64] = {0};
    sprintf(inc,"%d",increment);
    fillCommandPara(inc, strlen(inc), paraList);
    paraLen += strlen(inc) + 20;   
    
    fillCommandPara(member.c_str(), member.length(), paraList);
    paraLen += member.length() + 20;

    bool success = false;
    success = doRedisCommand( key,paraLen,paraList,RedisCommandType::REDIS_COMMAND_ZINCRBY);
    freeCommandList(paraList);
    return success;
}

int RedisClient::zcount(const string& key,int start, int end)
{
    list<RedisCmdParaInfo> paraList;
    int32_t paraLen = 0;
    fillCommandPara("zcount", 6, paraList);
    paraLen += 18;
    fillCommandPara(key.c_str(), key.length(), paraList);
    paraLen += key.length() + 20;

    char start_c[64] = {0};
    sprintf(start_c,"%d",start);
    fillCommandPara(start_c, strlen(start_c), paraList);
    paraLen += strlen(start_c) + 20; 
    char end_c[64] = {0};
    sprintf(end_c,"%d",end);
    fillCommandPara(end_c, strlen(end_c), paraList);
    paraLen += strlen(end_c) + 20;

    int count = -1;
    count = doRedisCommand( key,paraLen,paraList,RedisCommandType::REDIS_COMMAND_ZCOUNT);
    freeCommandList(paraList);
    return count;
}

int RedisClient::zcard(const string& key)
{
    list<RedisCmdParaInfo> paraList;
    int32_t paraLen = 0;
    string str="zcard";
    fillCommandPara(str.c_str(), str.size(), paraList);
    paraLen += str.size()+11;
    fillCommandPara(key.c_str(), key.length(), paraList);
    paraLen += key.length() + 20;

    int count = -1;
    count = doRedisCommand( key,paraLen,paraList,RedisCommandType::REDIS_COMMAND_ZCARD);
    freeCommandList(paraList);
    return count;
}

int RedisClient::dbsize()
{
    list<RedisCmdParaInfo> paraList;
    int32_t paraLen = 0;
    string str="DBSIZE";
    fillCommandPara(str.c_str(), str.size(), paraList);
    paraLen += str.size()+11;

    int count = -1;
    count = doRedisCommand( "",paraLen,paraList,RedisCommandType::REDIS_COMMAND_DBSIZE);
    freeCommandList(paraList);
    return count;
}

int RedisClient::zscore(const string& key,const string& member)
{
    list<RedisCmdParaInfo> paraList;
    int32_t paraLen = 0;
    fillCommandPara("zscore", 6, paraList);
    paraLen += 18;
    fillCommandPara(key.c_str(), key.length(), paraList);
    paraLen += key.length() + 20;
    fillCommandPara(member.c_str(), member.length(), paraList);
    paraLen += member.length() + 20;

    int count = -1;
    count = doRedisCommand( key,paraLen,paraList,RedisCommandType::REDIS_COMMAND_ZSCORE);
    freeCommandList(paraList);
    return count;
}

bool RedisClient::zrangebyscore(const string& key,int start, int end, list<string>& members)
{
    list<RedisCmdParaInfo> paraList;
    int32_t paraLen = 0;
    fillCommandPara("zrangebyscore", 13, paraList);
    paraLen += 25;
    fillCommandPara(key.c_str(), key.length(), paraList);
    paraLen += key.length() + 20;

    char start_c[64] = {0};
    sprintf(start_c,"%d",start);
    fillCommandPara(start_c, strlen(start_c), paraList);
    paraLen += strlen(start_c) + 20; 
    char end_c[64] = {0};
    sprintf(end_c,"%d",end);
    fillCommandPara(end_c, strlen(end_c), paraList);
    paraLen += strlen(end_c) + 20;

    bool success = false;
    success = doCommandArray( key,paraLen,paraList, RedisCommandType::REDIS_COMMAND_ZRANGEBYSCORE, members);
    freeCommandList(paraList);
    return success;
}

bool RedisClient::zremrangebyscore(const string& key,int start, int end)
{
    list<RedisCmdParaInfo> paraList;
    int32_t paraLen = 0;
    fillCommandPara("zremrangebyscore", 16, paraList);
    paraLen += 28;
    fillCommandPara(key.c_str(), key.length(), paraList);
    paraLen += key.length() + 20;

    char start_c[64] = {0};
    sprintf(start_c,"%d",start);
    fillCommandPara(start_c, strlen(start_c), paraList);
    paraLen += strlen(start_c) + 20; 
    char end_c[64] = {0};
    sprintf(end_c,"%d",end);
    fillCommandPara(end_c, strlen(end_c), paraList);
    paraLen += strlen(end_c) + 20;

    bool success = false;
    success = doRedisCommand( key,paraLen,paraList,RedisCommandType::REDIS_COMMAND_ZREMRANGEBYSCORE);
    freeCommandList(paraList);
    return success;
}


// sadd  key member [member...]
bool RedisClient::sadd(const string& key, const string& member)
{
	list<RedisCmdParaInfo> paraList;
    int32_t paraLen = 0;
    string str="sadd";
    fillCommandPara(str.c_str(), str.size(), paraList);
    paraLen += str.size()+11;
    fillCommandPara(key.c_str(), key.length(), paraList);
    paraLen += key.length() + 20;
 
    fillCommandPara(member.c_str(), member.length(), paraList);
    paraLen += member.length() + 20;

    int count = -1;
    count = doRedisCommand( key,paraLen,paraList,RedisCommandType::REDIS_COMMAND_SCARD);
    freeCommandList(paraList);
    return (count>0) ? true : false;
}

bool RedisClient::srem(const string& key, const string& member)
{
	list<RedisCmdParaInfo> paraList;
    int32_t paraLen = 0;
    string str="srem";
    fillCommandPara(str.c_str(), str.size(), paraList);
    paraLen += str.size()+11;
    fillCommandPara(key.c_str(), key.length(), paraList);
    paraLen += key.length() + 20;

    fillCommandPara(member.c_str(), member.length(), paraList);
    paraLen += member.length() + 20;

    int count = -1;
    count = doRedisCommand( key,paraLen,paraList,RedisCommandType::REDIS_COMMAND_SCARD);
    freeCommandList(paraList);
    return (count>0) ? true : false;
}

int RedisClient::scard(const string& key)
{
    list<RedisCmdParaInfo> paraList;
    int32_t paraLen = 0;
    string str="scard";
    fillCommandPara(str.c_str(), str.size(), paraList);
    paraLen += str.size()+11;
    fillCommandPara(key.c_str(), key.length(), paraList);
    paraLen += key.length() + 20;

    int count = -1;
    count = doRedisCommand( key,paraLen,paraList,RedisCommandType::REDIS_COMMAND_SCARD);
    freeCommandList(paraList);
    return count;
}

bool RedisClient::sismember(const string& key, const string& member)
{
	list<RedisCmdParaInfo> paraList;
    int32_t paraLen = 0;
    string str="sismember";
    fillCommandPara(str.c_str(), str.size(), paraList);
    paraLen += str.size()+11;
    fillCommandPara(key.c_str(), key.length(), paraList);
    paraLen += key.length() + 20;
   
    fillCommandPara(member.c_str(), member.length(), paraList);
    paraLen += member.length() + 20;

    int count = -1;
    count = doRedisCommand( key,paraLen,paraList,RedisCommandType::REDIS_COMMAND_SCARD);
    freeCommandList(paraList);
    return (count==1) ? true : false;
}

bool RedisClient::smembers(const string& key, list<string>& members)
{
	list<RedisCmdParaInfo> paraList;
    int32_t paraLen = 0;
    string str="smembers";
    fillCommandPara(str.c_str(), str.size(), paraList);
    paraLen += str.size()+11;
    fillCommandPara(key.c_str(), key.length(), paraList);
    paraLen += key.length() + 11;

    bool success = false;
    success = doCommandArray( key,paraLen,paraList, RedisCommandType::REDIS_COMMAND_SMEMBERS, members);
    freeCommandList(paraList);
    return success;
}

//bool RedisClient::clearBySet(string dbPre) 
//{
//	// <1> del all xxx.db_xxxID
//	list<string> keys;
//	smembers(dbPre+"set", keys);
//	for(list<string>::iterator it=keys.begin(); it!=keys.end(); it++)
//	{
//		del(dbPre+*it);
//	}
//	// <2> del xxx.db_set
//	del(dbPre+"set"); 
//	return true;		
//}



/*
	API for transaction
*/
bool RedisClient::PrepareTransaction(RedisConnection** conn)
{
    if(m_redisMode==CLUSTER_MODE)
    {
        m_logger.error("transaction not supported in cluster mode");
        return false;
    }
    
    RedisConnection* con=m_redisProxy.clusterHandler->getAvailableConnection();
    if(con==NULL)
    {
        m_logger.error("cannot acquire a redis connection");
        return false;
    }
    *conn=con;
	return true;
}

bool RedisClient::WatchKeys(const vector<string>& keys, RedisConnection* con)
{
	for(size_t i=0; i<keys.size(); i++)
	{
		if(!WatchKey(keys[i], con))
			return false;
	}
	return true;
}

bool RedisClient::WatchKey(const string& key, RedisConnection* con)
{
	list<RedisCmdParaInfo> watchParaList;
	int32_t watchParaLen = 0;
	fillCommandPara("watch", 5, watchParaList);
	watchParaLen += 18;
	fillCommandPara(key.c_str(), key.length(), watchParaList);
	watchParaLen += key.length() + 20;
	bool success = doTransactionCommandInConnection(watchParaLen,watchParaList,RedisCommandType::REDIS_COMMAND_WATCH, con);
	freeCommandList(watchParaList);
    return success;
}

bool RedisClient::Unwatch(RedisConnection* con)
{
    list<RedisCmdParaInfo> unwatchParaList;
    int32_t unwatchParaLen = 0;
    fillCommandPara("unwatch", 6, unwatchParaList);
    unwatchParaLen += 20;
	bool success = doTransactionCommandInConnection(unwatchParaLen,unwatchParaList,RedisCommandType::REDIS_COMMAND_UNWATCH, con);
    freeCommandList(unwatchParaList);
    return success;
}


bool RedisClient::StartTransaction(RedisConnection* con)
{
    if(m_redisMode==CLUSTER_MODE)
    {
        m_logger.error("transaction not supported in cluster mode");
        return false;
    }    

	bool success = false;
	list<RedisCmdParaInfo> multiParaList;
	int32_t multiParaLen = 0;
	fillCommandPara("multi", 5, multiParaList);
	multiParaLen += 18;
	
	// TODO call con->doRedisCommand ?
	success = doTransactionCommandInConnection(multiParaLen,multiParaList,RedisCommandType::REDIS_COMMAND_MULTI, con);
	freeCommandList(multiParaList);
	if (!success)
	{
		m_logger.warn("do multi command failed.");
    }
    return success;
}


bool RedisClient::DiscardTransaction(RedisConnection* con)
{
    if(m_redisMode==CLUSTER_MODE)
    {
        m_logger.error("transaction not supported in cluster mode");
        return false;
    }

	bool success = false;
	list<RedisCmdParaInfo> paraList;
	int32_t multiParaLen = 0;
	fillCommandPara("discard", 7, paraList);
	multiParaLen += 18;
	success = doTransactionCommandInConnection(multiParaLen,paraList,RedisCommandType::REDIS_COMMAND_DISCARD, con);
	freeCommandList(paraList);
	if (!success)
	{
		m_logger.warn("do discard command failed.");
    }
    return success;
}


bool RedisClient::ExecTransaction(RedisConnection* con)
{
    if(m_redisMode==CLUSTER_MODE)
    {
        m_logger.error("transaction not supported in cluster mode");
        return false;
    }

	bool success = false;
	list<RedisCmdParaInfo> paraList;
	int32_t multiParaLen = 0;
	fillCommandPara("exec", 4, paraList);
	multiParaLen += 18;
	success = doTransactionCommandInConnection(multiParaLen,paraList,RedisCommandType::REDIS_COMMAND_EXEC, con);
	freeCommandList(paraList);
	if (!success)
	{
		m_logger.warn("exec transaction failed.");
		return false;
    }
    else
    {
	    m_logger.debug("exec transaction ok");
	    return true;
	}
}


bool RedisClient::FinishTransaction(RedisConnection** conn)
{
    if(m_redisMode==CLUSTER_MODE)
    {
        m_logger.error("transaction not supported in cluster mode");
        return false;
    }

	if(conn==NULL  ||  *conn==NULL)
		return false;
    m_redisProxy.clusterHandler->releaseConnection(*conn);
    *conn=NULL;
	return true;
}


bool RedisClient::doTransactionCommandInConnection(int32_t commandLen, list<RedisCmdParaInfo> &commandList, int commandType, RedisConnection* con)
{
	RedisReplyInfo replyInfo;
	bool needRedirect;
	string redirectInfo;
    if(m_redisProxy.clusterHandler==NULL)
    {
        m_logger.error("m_redisProxy.clusterHandler is NULL");
        return false;
    }
	if(!m_redisProxy.clusterHandler->doRedisCommandOneConnection(commandList, commandLen, replyInfo, false, &con))
	{
		freeReplyInfo(replyInfo);
		m_logger.warn("proxy:%s do redis command failed.", m_redisProxy.proxyId.c_str());
		return false;
	}

	switch (commandType)
	{
		// expects "QUEUED"			
		case RedisCommandType::REDIS_COMMAND_SET:		
		case RedisCommandType::REDIS_COMMAND_DEL:		
		case RedisCommandType::REDIS_COMMAND_SADD:
		case RedisCommandType::REDIS_COMMAND_SREM:
			if(!parseQueuedResponseReply(replyInfo))
			{
				m_logger.warn("parse queued command reply failed. reply string:%s.", replyInfo.resultString.c_str());
				freeReplyInfo(replyInfo);
				return false;
			}
			break;
		// expects "OK"
		case RedisCommandType::REDIS_COMMAND_WATCH:
		case RedisCommandType::REDIS_COMMAND_UNWATCH:
		case RedisCommandType::REDIS_COMMAND_MULTI:
		case RedisCommandType::REDIS_COMMAND_DISCARD:
			if(!parseStatusResponseReply(replyInfo,needRedirect,redirectInfo))
			{
				m_logger.warn("parse watch reply failed. reply string:%s.", replyInfo.resultString.c_str());
				freeReplyInfo(replyInfo);
				return false;
			}
			break;
		// expects array
		case RedisCommandType::REDIS_COMMAND_EXEC:
			if(!parseExecReply(replyInfo))
			{
				m_logger.warn("parse exec reply failed. reply string:%s.", replyInfo.resultString.c_str());
				freeReplyInfo(replyInfo);
				return false;
			}
			break;
		default:
			m_logger.warn("recv unknown command type:%d", commandType);
			return false;
		
	}
	freeReplyInfo(replyInfo);
	return true;
}

//for watch,unwatch,multi,discard command response.
bool RedisClient::parseStatusResponseReply(RedisReplyInfo & replyInfo)
{
	m_logger.debug("status replyInfo has replyType %d, resultString %s, intValue %d", replyInfo.replyType, replyInfo.resultString.c_str(), replyInfo.intValue);
			
	if (replyInfo.replyType != RedisReplyType::REDIS_REPLY_STATUS)
	{
		m_logger.warn("status response failed, redis response:[%s].", replyInfo.resultString.c_str());
		return false;
	}
	return true;
}

//for queued command in a transaction
bool RedisClient::parseQueuedResponseReply(RedisReplyInfo & replyInfo)
{
	m_logger.debug("queued replyInfo has replyType %d, resultString %s, intValue %d", replyInfo.replyType, replyInfo.resultString.c_str(), replyInfo.intValue);
			
	if (replyInfo.replyType != RedisReplyType::REDIS_REPLY_STATUS)
	{
		m_logger.warn("status response failed, redis response:[%s].", replyInfo.resultString.c_str());
		return false;
	}
	return true;
}

bool RedisClient::parseExecReply(RedisReplyInfo & replyInfo)
{
	m_logger.debug("exec replyInfo has replyType %d, resultString %s, intValue %d", replyInfo.replyType, replyInfo.resultString.c_str(), replyInfo.intValue);

	if(replyInfo.replyType  == RedisReplyType::REDIS_REPLY_NIL)
	{
		m_logger.warn("transaction interrupted: nil");
		return false;
	}
	if (replyInfo.replyType != RedisReplyType::REDIS_REPLY_ARRAY)
	{
		m_logger.warn("recv redis wrong reply type:[%d].", replyInfo.replyType);
		return false;
	}
	// empty array
	if (replyInfo.replyType == RedisReplyType::REDIS_REPLY_ARRAY && replyInfo.intValue == -1)
	{
		m_logger.warn("exec reply -1, set serial exec failed.");
		return false;
	}

	list<ReplyArrayInfo>::iterator arrayIter;
	for (arrayIter = replyInfo.arrayList.begin(); arrayIter != replyInfo.arrayList.end(); arrayIter++)
	{
		m_logger.debug("arrayList has replyType %d, arrayValue %s, arrayLen %d", (*arrayIter).replyType, arrayIter->arrayValue, arrayIter->arrayLen);

		if ((*arrayIter).replyType == RedisReplyType::REDIS_REPLY_STRING)
		{
			// error type
			if (strncmp((*arrayIter).arrayValue, "-", 1) == 0)
			{
				m_logger.warn("recv failed exec reply:%s.", (*arrayIter).arrayValue);
				return false;
			}
			// integer type: 0
			else if(strncmp((*arrayIter).arrayValue, ":0", 2) == 0)
			{
				m_logger.warn("recv failed exec reply:%s.", (*arrayIter).arrayValue);
//				return false;
				return true;
			}
			// bulk string: nil
			else if(strncmp((*arrayIter).arrayValue, "$-1", 3) == 0)
			{
				m_logger.warn("recv failed exec reply:%s.", (*arrayIter).arrayValue);
				return false;
			}
			// array type: empty array
			else if(strncmp((*arrayIter).arrayValue, "*-1", 3) == 0)
			{
				m_logger.warn("recv failed exec reply:%s.", (*arrayIter).arrayValue);
				return false;
			}
		}
	}
	return true;
}

bool RedisClient::Set(RedisConnection* con, const string & key, const DBSerialize& serial)
{
	list<RedisCmdParaInfo> paraList;
	int32_t paraLen = 0;
	fillCommandPara("set", 3, paraList);
	paraLen += 15;
	fillCommandPara(key.c_str(), key.length(), paraList);
	paraLen += key.length() + 20;	
	DBOutStream out; 
	serial.save(out);
	fillCommandPara(out.getData(), out.getSize(), paraList);
	paraLen += out.getSize() + 20;
	bool success = doTransactionCommandInConnection(paraLen,paraList,RedisCommandType::REDIS_COMMAND_SET, con);
	freeCommandList(paraList);
	if(!success)
	{
		m_logger.error("set of transaction failed");
	}
	return success;
}

bool RedisClient::Set(RedisConnection* con, const string & key, const string& value)
{
	list<RedisCmdParaInfo> paraList;
	int32_t paraLen = 0;
	fillCommandPara("set", 3, paraList);
	paraLen += 15;
	fillCommandPara(key.c_str(), key.length(), paraList);
	paraLen += key.length() + 20;
	fillCommandPara(value.c_str(), value.size(), paraList);
	paraLen += value.size() + 20;
	bool success = doTransactionCommandInConnection(paraLen,paraList,RedisCommandType::REDIS_COMMAND_SET, con);
	freeCommandList(paraList);
	if(!success)
	{
		m_logger.error("set of transaction failed");
	}
	return success;
}

bool RedisClient::Del(RedisConnection* con, const string & key)
{
	list<RedisCmdParaInfo> paraList;
	int32_t paraLen = 0;
	fillCommandPara("del", 3, paraList);
	paraLen += 15;
	fillCommandPara(key.c_str(), key.length(), paraList);
	paraLen += key.length() + 20;
	bool success = doTransactionCommandInConnection(paraLen,paraList,RedisCommandType::REDIS_COMMAND_DEL, con);
	freeCommandList(paraList);
	if(!success)
	{
		m_logger.error("del of transaction failed");
	}
	return success;
}

bool RedisClient::Sadd(RedisConnection* con, const string & setKey, const string& member)
{
	list<RedisCmdParaInfo> paraList;
	int32_t paraLen = 0;
	fillCommandPara("sadd", 4, paraList);
	paraLen += 15;
	fillCommandPara(setKey.c_str(), setKey.length(), paraList);
	paraLen += setKey.length() + 20;
	fillCommandPara(member.c_str(), member.size(), paraList);
	paraLen += member.size() + 20;
	bool success = doTransactionCommandInConnection(paraLen,paraList,RedisCommandType::REDIS_COMMAND_SADD, con);
	freeCommandList(paraList);
	if(!success)
	{
		m_logger.error("sadd of transaction failed");
	}
	return success;
}

bool RedisClient::Srem(RedisConnection* con, const string & setKey, const string& member)
{
	list<RedisCmdParaInfo> paraList;
	int32_t paraLen = 0;
	fillCommandPara("srem", 4, paraList);
	paraLen += 15;
	fillCommandPara(setKey.c_str(), setKey.length(), paraList);
	paraLen += setKey.length() + 20;
	fillCommandPara(member.c_str(), member.size(), paraList);
	paraLen += member.size() + 20;
	bool success = doTransactionCommandInConnection(paraLen,paraList,RedisCommandType::REDIS_COMMAND_SREM, con);
	freeCommandList(paraList);
	if(!success)
	{
		m_logger.error("srem of transaction failed");
	}
	return success;
}



