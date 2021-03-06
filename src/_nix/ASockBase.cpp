
#include "../../include/ASockBase.hpp"

using namespace asocklib ;

///////////////////////////////////////////////////////////////////////////////
ASockBase::ASockBase(int nMaxMsgLen)
{
    SetBufferCapacity(nMaxMsgLen);
}

///////////////////////////////////////////////////////////////////////////////
bool ASockBase::SetBufferCapacity(int nMaxMsgLen)
{
    if(nMaxMsgLen<0)
    {
        strErr_ = "length is negative";
        std::cerr <<"["<< __func__ <<"-"<<__LINE__ <<"] "<< strErr_<<"\n";
        return false;
    }
    if(nMaxMsgLen==0 ||  nMaxMsgLen < asocklib::DEFAULT_CAPACITY) 
    {
        nBufferCapcity_ = asocklib::DEFAULT_CAPACITY ;
    }
    else
    {
        nBufferCapcity_ = nMaxMsgLen*2; 
    }

    return true;
}

///////////////////////////////////////////////////////////////////////////////
bool   ASockBase::SetNonBlocking(int nSockFd)
{
    int oldflags  ;

    if ((oldflags = fcntl( nSockFd,F_GETFL, 0)) < 0 )
    {
        strErr_ = "fcntl F_GETFL error [" + std::string(strerror(errno))+ "]";
        return  false;
    }

    int ret  = fcntl( nSockFd,F_SETFL,oldflags | O_NONBLOCK) ;
    if ( ret < 0 )
    {
        strErr_ = "fcntl O_NONBLOCK error [" + std::string(strerror(errno))+ "]";
        return  false;
    }

    return true;
}

///////////////////////////////////////////////////////////////////////////////
bool ASockBase::Recv(Context* pContext) 
{
    int nToRecvLen = asocklib::DEFAULT_PACKET_SIZE ;
    if(asocklib::DEFAULT_PACKET_SIZE > pContext->recvBuffer_.GetLinearFreeSpace() )
    {
        nToRecvLen = pContext->recvBuffer_.GetLinearFreeSpace() ; 
    }

    if(nToRecvLen==0) 
    {
        strErr_ = "no linear free space left ";
        std::cerr <<"["<< __func__ <<"-"<<__LINE__ <<"] "<< strErr_<<"\n";
        return false; 
    }

    int nRecvedLen = recv( pContext->socket_, pContext->recvBuffer_.GetLinearAppendPtr(), nToRecvLen, 0); 

    if( nRecvedLen > 0)
    {
        pContext->recvBuffer_.IncreaseData(nRecvedLen);

        while(pContext->recvBuffer_.GetCumulatedLen())
        {
            //invoke user specific implementation
            if(!pContext->bPacketLenCalculated )
            {
                //only when calculation is necessary
                pContext->nOnePacketLength = GetOnePacketLength( pContext ); 
                pContext->bPacketLenCalculated = true;
            }

            if(pContext->nOnePacketLength == asocklib::MORE_TO_COME)
            {
                pContext->bPacketLenCalculated = false;
                return true; //need to recv more
            }
            else if(pContext->nOnePacketLength > pContext->recvBuffer_.GetCumulatedLen())
            {
                return true; //need to recv more
            }
            else
            {
                //got complete packet 
                if(cumbuffer_defines::OP_RSLT_OK!=pContext->recvBuffer_.GetData(pContext->nOnePacketLength, szOnePacketData_ ))
                {
                    //error !
                    strErr_ = pContext->recvBuffer_.GetErrMsg();
                    std::cerr <<"["<< __func__ <<"-"<<__LINE__ <<"] "<< strErr_ <<"\n";
                    pContext->bPacketLenCalculated = false;
                    return false; 
                }
                
                //invoke user specific implementation
                OnRecvOnePacketData(pContext, szOnePacketData_ , pContext->nOnePacketLength ); 
                pContext->bPacketLenCalculated = false;
            }
        } //while
    }   
    else if( nRecvedLen == 0 )
    {
        strErr_ = "recv 0, client disconnected , fd:" + std::to_string(pContext->socket_);
        std::cerr <<"["<< __func__ <<"-"<<__LINE__ <<"] "<< strErr_<<"\n";
        return false ;
    }

    return true ;
}


///////////////////////////////////////////////////////////////////////////////
bool ASockBase::Send (Context* pClientContext, const char* pData, int nLen) 
{
    char* pDataPosition = const_cast<char*>(pData) ;   
    int nSendBytes = 0;           

    while( nSendBytes < nLen ) 
    {
        int nSent = send(pClientContext->socket_, pDataPosition, nLen-nSendBytes, 0);
        if(nSent > 0)
        {
            nSendBytes += nSent ;  
            pDataPosition += nSent ;      
        }
        else if( nSent < 0 )
        {
            if ( errno == EWOULDBLOCK || errno == EAGAIN )
            {
                //send later
                cumbuffer_defines::OP_RESULT opRslt ; 
                {
                std::lock_guard<std::mutex> guard(pClientContext->clientSendLock_);
                opRslt =pClientContext->sendBuffer_.Append(nLen-nSendBytes, pDataPosition); 
                }
                if(cumbuffer_defines::OP_RSLT_OK!=opRslt) 
                {
                    strErr_ = pClientContext->sendBuffer_.GetErrMsg();
                    std::cerr <<"["<< __func__ <<"-"<<__LINE__ <<"] error! "<< GetLastErrMsg() <<"\n"; 
                    return false;
                }
#ifdef __APPLE__
                if(!KqueueCtl(pClientContext, EVFILT_WRITE, EV_ADD|EV_ENABLE ))
#elif __linux__
                if(!EpollCtl (pClientContext, EPOLLIN | EPOLLOUT | EPOLLERR | EPOLLRDHUP, EPOLL_CTL_MOD ) )
#endif
                {
                    return false;
                }
                return true;
            }
            else if ( errno != EINTR )
            {
                strErr_ = "send error [" + std::string(strerror(errno)) + "]";
                std::cerr <<"["<< __func__ <<"-"<<__LINE__ <<"] "<< GetLastErrMsg() <<"\n"; 
                return false;
            }
        }
    }

    return true;
}

#ifdef __APPLE__
///////////////////////////////////////////////////////////////////////////////
bool ASockBase::KqueueCtl(Context* pContext , uint32_t events, uint32_t fflags)
{
    struct  kevent kEvent;
    memset(&kEvent, 0, sizeof(struct kevent));
    EV_SET(&kEvent, pContext->socket_, events,fflags , 0, 0, pContext); //udata = pContext

    int nRslt = kevent(nKqfd_, &kEvent, 1, NULL, 0, NULL);
    if (nRslt == -1)
    {
        strErr_ = "kevent error [" + std::string(strerror(errno)) + "]";
        std::cerr <<"["<< __func__ <<"-"<<__LINE__ <<"] "<< GetLastErrMsg() <<"\n"; 
        return false; 
    }
    return true;
    //man:Re-adding an existing event will modify the parameters of the
    //    original event, and not result in a duplicate entry.
}
#elif __linux__
///////////////////////////////////////////////////////////////////////////////
bool ASockBase::EpollCtl(Context* pContext , uint32_t events, int op)
{
    struct  epoll_event evClient{};
    evClient.data.fd    = pContext->socket_;
    evClient.events     = events ;
    evClient.data.ptr   = pContext;

    if(epoll_ctl(nEpfd_, op, pContext->socket_, &evClient)<0)
    {
        strErr_ = "kevent error [" + std::string(strerror(errno)) + "]";
        std::cerr <<"["<< __func__ <<"-"<<__LINE__ <<"] "<< GetLastErrMsg() <<"\n"; 
        return false; 
    }
    return true;
}
#endif

