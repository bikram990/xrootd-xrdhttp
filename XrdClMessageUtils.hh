//------------------------------------------------------------------------------
// Copyright (c) 2011-2012 by European Organization for Nuclear Research (CERN)
// Author: Lukasz Janyst <ljanyst@cern.ch>
//------------------------------------------------------------------------------
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//------------------------------------------------------------------------------

#ifndef __XRD_CL_MESSAGE_UTILS_HH__
#define __XRD_CL_MESSAGE_UTILS_HH__

#include "XrdCl/XrdClXRootDResponses.hh"
#include "XrdCl/XrdClURL.hh"
#include "XrdCl/XrdClMessage.hh"
#include "XrdSys/XrdSysPthread.hh"
#include <memory>

namespace XrdCl
{
  //----------------------------------------------------------------------------
  //! Synchronize the response
  //----------------------------------------------------------------------------
  class SyncResponseHandler: public ResponseHandler
  {
    public:
      //------------------------------------------------------------------------
      //! Constructor
      //------------------------------------------------------------------------
      SyncResponseHandler(): pStatus(0), pResponse(0), pSem(0) {}

      //------------------------------------------------------------------------
      //! Handle the response
      //------------------------------------------------------------------------
      virtual void HandleResponse( XRootDStatus *status,
                                   AnyObject    *response )
      {
        pStatus = status;
        pResponse = response;
        pSem.Post();
      }

      //------------------------------------------------------------------------
      //! Get the status
      //------------------------------------------------------------------------
      XRootDStatus *GetStatus()
      {
        return pStatus;
      }

      //------------------------------------------------------------------------
      //! Get the response
      //------------------------------------------------------------------------
      AnyObject *GetResponse()
      {
        return pResponse;
      }

      //------------------------------------------------------------------------
      //! Wait for the arrival of the response
      //------------------------------------------------------------------------
      void WaitForResponse()
      {
        pSem.Wait();
      }

    private:
      XRootDStatus    *pStatus;
      AnyObject       *pResponse;
      XrdSysSemaphore  pSem;
  };

  class MessageUtils
  {
    public:
      //------------------------------------------------------------------------
      //! Wait and return the status of the query
      //------------------------------------------------------------------------
      static XRootDStatus WaitForStatus( SyncResponseHandler *handler )
      {
        handler->WaitForResponse();
        XRootDStatus *status = handler->GetStatus();
        XRootDStatus ret( *status );
        delete status;
        return ret;
      }

      //------------------------------------------------------------------------
      //! Wait for the response
      //------------------------------------------------------------------------
      template<class Type>
      static XrdCl::XRootDStatus WaitForResponse(
                            SyncResponseHandler  *handler,
                            Type                *&response )
      {
        handler->WaitForResponse();

        std::auto_ptr<AnyObject> resp( handler->GetResponse() );
        XRootDStatus *status = handler->GetStatus();
        XRootDStatus ret( *status );
        delete status;

        if( ret.IsOK() )
        {
          if( !resp.get() )
            return XRootDStatus( stError, errInternal );
          resp->Get( response );
          resp->Set( (int *)0 );
          if( !response )
            return XRootDStatus( stError, errInternal );
        }

        return ret;
      }

      //------------------------------------------------------------------------
      //! Create a message
      //------------------------------------------------------------------------
      template<class Type>
      static void CreateRequest( Message  *&msg,
                                 Type     *&req,
                                 uint32_t  payloadSize = 0 )
      {
        msg = new Message( sizeof(Type)+payloadSize );
        req = (Type*)msg->GetBuffer();
        msg->Zero();
      }

      //------------------------------------------------------------------------
      //! Send message
      //------------------------------------------------------------------------
      static Status SendMessage( const URL       &url,
                                 Message         *msg,
                                 ResponseHandler *handler,
                                 uint16_t         timeout,
                                 bool             followRedirects = true,
                                 char            *userBuffer      = 0,
                                 uint32_t         userBufferSize  = 0 );

  };
}

#endif // __XRD_CL_MESSAGE_UTILS_HH__
