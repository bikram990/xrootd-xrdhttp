
/******************************************************************************/
/*                                                                            */
/*                 X r d S e c P r o t o c o l s s l . c c                    */
/*                                                                            */
/* (c) 2007 by the Board of Trustees of the Leland Stanford, Jr., University  */
/*                            All Rights Reserved                             */
/*   Produced by Andrew Hanushevsky for Stanford University under contract    */
/*              DE-AC02-76-SFO0515 with the Department of Energy              */
/******************************************************************************/

//       $Id$


const char *XrdSecProtocolsslCVSID = "$Id$";

#include "XrdSecProtocolssl.hh"

char*  XrdSecProtocolssl::sslcadir=(char*)"/etc/grid-security/certificates";
char*  XrdSecProtocolssl::sslvomsdir=(char*)"/etc/grid-security/vomsdir";
int    XrdSecProtocolssl::verifydepth=10;
int    XrdSecProtocolssl::verifyindex=0;
char*  XrdSecProtocolssl::sslkeyfile=(char*)"/etc/grid-security/hostkey.pem";
char*  XrdSecProtocolssl::sslcertfile=(char*)"/etc/grid-security/hostcert.pem";
char*  XrdSecProtocolssl::sslproxyexportdir=(char*)0;
int    XrdSecProtocolssl::debug=0;
time_t XrdSecProtocolssl::sslsessionlifetime=86400;
bool   XrdSecProtocolssl::isServer=0;
bool   XrdSecProtocolssl::forwardProxy=0;
bool   XrdSecProtocolssl::allowSessions=1;
char*  XrdSecProtocolssl::SessionIdContext = (char*)"xrootdssl"; 
char*  XrdSecProtocolssl::gridmapfile = (char*) "/etc/grid-security/grid-mapfile";
bool   XrdSecProtocolssl::mapuser  = false;
char*  XrdSecProtocolssl::vomsmapfile = (char*) "/etc/grid-security/voms-mapfile";
bool   XrdSecProtocolssl::mapgroup = false;
bool   XrdSecProtocolssl::mapcerncertificates = false;

X509_STORE*    XrdSecProtocolssl::store=0;
X509_LOOKUP*   XrdSecProtocolssl::lookup=0;
SSL_CTX*       XrdSecProtocolssl::ctx=0;
XrdSysError    XrdSecProtocolssl::eDest(0, "secssl_");
XrdSysLogger   XrdSecProtocolssl::Logger;
time_t         XrdSecProtocolssl::storeLoadTime;

XrdSysMutex XrdSecsslSessionLock::sessionmutex;

XrdOucHash<XrdOucString> XrdSecProtocolssl::gridmapstore;
XrdOucHash<XrdOucString> XrdSecProtocolssl::vomsmapstore;
XrdOucHash<XrdOucString> XrdSecProtocolssl::stringstore;
XrdSysMutex              XrdSecProtocolssl::StoreMutex;           
XrdSysMutex              XrdSecProtocolssl::GridMapMutex;           
XrdSysMutex              XrdSecProtocolssl::VomsMapMutex;           

/******************************************************************************/
/*             C l i e n t   O r i e n t e d   F u n c t i o n s              */
/******************************************************************************/

int XrdSecProtocolssl::Fatal(XrdOucErrInfo *erp, const char *msg, int rc)
{
  const char *msgv[8];
  int k, i = 0;
  
  msgv[i++] = "Secssl: ";    //0
  msgv[i++] = msg;            //1

  if (erp) erp->setErrInfo(rc, msgv, i);
  else {for (k = 0; k < i; k++) cerr <<msgv[k];
  cerr <<endl;
  }
  
  return -1;
}

/******************************************************************************/
/*                        g e t C r e d e n t i a l s                         */
/******************************************************************************/


void   
XrdSecProtocolssl::secClient(int theFD, XrdOucErrInfo      *error) {
  
  EPNAME("secClient");

  char *nossl = getenv("XrdSecNoSSL");
  if (nossl) {
    error->setErrInfo(ENOENT,"SSL is disabled by force");
    return ;
  }

  error->setErrInfo(0,"");
  SSLMutex.Lock();  
  
  int err;
  char*    str;
  SSL_METHOD *meth;
  SSL_SESSION *session=0;

  SSL_load_error_strings();  
  SSLeay_add_ssl_algorithms();
  meth = (SSL_METHOD*) TLSv1_client_method();

  ERR_load_crypto_strings();

  XrdOucString sslsessionfile="";
  XrdOucString sslsessionid="";
  
  sslsessionfile = "/tmp/xssl_";
  sslsessionid += (int)geteuid();
  sslsessionid += ":";
  sslsessionid += host.c_str();
  sslsessionfile += sslsessionid;

  XrdSecsslSessionLock sessionlock;
  sessionlock.SoftLock();

  if (allowSessions) {
    struct stat sessionstat;

    if (!stat(sslsessionfile.c_str(),&sessionstat)) {
      // session exists ... I try to read it
      if (sessionlock.HardLock(sslsessionfile.c_str())) {
	FILE* fp = fopen(sslsessionfile.c_str(), "r");
	if (fp) {
	  {
	    session = PEM_read_SSL_SESSION(fp, NULL, NULL, NULL);
	    fclose(fp);
	    if (session) {
	      
	      DEBUG("info: ("<<__FUNCTION__<<") Session loaded from " << sslsessionfile.c_str());
	      char session_id[1024];
	      for (int i=0; i< (int)session->session_id_length; i++) {
		sprintf(session_id+(i*2),"%02x",session->session_id[i]);
	      }
	    
	      unsigned char buf[5],*p;
	      unsigned long l;
	      
	      p=buf;
	      l=session->cipher_id;
	      l2n(l,p);
	      if ((session->ssl_version>>8) == SSL3_VERSION_MAJOR)
		session->cipher=meth->get_cipher_by_char(&(buf[2]));
	      else
		session->cipher=meth->get_cipher_by_char(&(buf[1]));
	      if (session->cipher == NULL) {
		SSL_SESSION_free(session);
		delete session;
		session = NULL;		  
	      } else {
		DEBUG("Info: ("<<__FUNCTION__<<") Session Id: "<< session_id << " Cipher: " << session->cipher->name  << " Verify: " << session->verify_result << " (" << X509_verify_cert_error_string(session->verify_result) << ")");
	      }
	    } else {
	      DEBUG("info: ("<<__FUNCTION__<<") Session load failed from " << sslsessionfile.c_str());
	      ERR_print_errors_fp(stderr);
	    }
	  }
	}
      }
    } else {
    }
  }

  ctx = SSL_CTX_new (meth);


  SSL_CTX_set_options(ctx,  SSL_OP_ALL | SSL_OP_NO_SSLv2);

  if (!ctx) {
    Fatal(error,"Cannot do SSL_CTX_new",-1);
    exit(2);
  }
  
  if (SSL_CTX_use_certificate_chain_file(ctx, sslcertfile) <= 0) {
    ERR_print_errors_fp(stderr);
    exit(3);
  }
  
  if (SSL_CTX_use_PrivateKey_file(ctx, sslkeyfile, SSL_FILETYPE_PEM) <= 0) {
    ERR_print_errors_fp(stderr);
    exit(4);
  }
  
  if (!SSL_CTX_check_private_key(ctx)) {
    fprintf(stderr,"Error: (%s) Private key does not match the certificate public key\n",__FUNCTION__);
    exit(5);
  } else {
    DEBUG("Private key check passed ...");
  }
  
  SSL_CTX_load_verify_locations(ctx, NULL,sslcadir);
  SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT,  GRST_callback_SSLVerify_wrapper);

  SSL_CTX_set_cert_verify_callback(ctx, GRST_verify_cert_wrapper, (void *) NULL);

  grst_cadir   = sslcadir;
  grst_vomsdir = sslvomsdir;

  grst_depth=verifydepth;
  SSL_CTX_set_verify_depth(ctx, verifydepth);

  if (session) {
    SSL_CTX_add_session(ctx,session);
  }


  ssl = SSL_new (ctx);            
  SSL_set_purpose(ssl,X509_PURPOSE_ANY);
  if (session) {
    SSL_set_session(ssl, session);
  }

  sessionlock.SoftUnLock();
  sessionlock.HardUnLock();

  if (!ssl) {
    Fatal(error,"Cannot do SSL_new",-1);
    exit(6);
  }

  SSL_set_fd (ssl, theFD);
  err = SSL_connect (ssl);

  if (err!=1) {
    // we want to see the error message from the server side
    ERR_print_errors_fp(stderr);
    if (theFD>=0) {close(theFD);theFD=-1;}
    SSLMutex.UnLock();  
    return;
  }

  session = SSL_get_session(ssl);
  
  /* Get the cipher - opt */
  
  TRACE(Authen,"SSL connection uses cipher: "<<SSL_get_cipher (ssl));
  
  /* Get server's certificate (note: beware of dynamic allocation) - opt */
  
  server_cert = SSL_get_peer_certificate (ssl);       

  if (!server_cert) {
    TRACE(Authen,"Server didn't provide certificate");
  }

  XrdOucString rdn;
  
  str = X509_NAME_oneline (X509_get_subject_name (server_cert),0,0);
  rdn = str;
  TRACE(Authen,"Server certificate subject:\t" << str);
  OPENSSL_free (str);
  
  str = X509_NAME_oneline (X509_get_issuer_name  (server_cert),0,0);
  TRACE(Authen,"Server certificate  issuer: \t" << str);
  OPENSSL_free (str);
  
  X509_free (server_cert);
  server_cert=0;

  if (forwardProxy) {
    if (!strcmp(sslkeyfile,sslcertfile)) {
      // this is a cert & key in one file atleast ... looks like proxy
      int fd = open(sslkeyfile,O_RDONLY);
      if (fd>=0) {
	int nread = read(fd,proxyBuff, sizeof(proxyBuff));
	if (nread>=0) {
	  TRACE(Authen,"Uploading my Proxy ...\n");
	  err = SSL_write(ssl, proxyBuff,nread);
	  if (err!= nread) {
	    Fatal(error,"Cannot forward proxy",-1);
	    if (theFD>=0) {close(theFD); theFD=-1;}
	    SSLMutex.UnLock();  
	    return;
	  }
	  char ok[16];
	  err = SSL_read(ssl,ok, 3);
	  if (err != 3) {
	    Fatal(error,"Didn't receive OK",-1);
	    if (theFD>=0) {close(theFD); theFD=-1;}
	    SSLMutex.UnLock();  
	    return;
	  } 
	} else {
	  close(fd);
	  Fatal(error,"Cannot read proxy file to forward",-1);
	  if (theFD>=0) {close(theFD);theFD=-1;}
	  SSLMutex.UnLock();  
	  return;
	}
      } else {
	Fatal(error,"Cannot read proxy file to forward",-1);
	if (theFD>=0) {close(theFD);theFD=-1;}
	SSLMutex.UnLock();  
	return;
      }
      close(fd);
    }
  }

  if (allowSessions && session) {
    char session_id[1024];
    for (int i=0; i< (int)session->session_id_length; i++) {
      sprintf(session_id+(i*2),"%02x",session->session_id[i]);
    }
    
    if (session->cipher) {
      DEBUG("Info: ("<<__FUNCTION__<<") Session Id: "<< session_id << " Cipher: " << session->cipher->name  << " Verify: " << session->verify_result << " (" << X509_verify_cert_error_string(session->verify_result) << ")");
    } else {
      DEBUG("Info: ("<<__FUNCTION__<<") Session Id: "<< session_id << " Verify: " << session->verify_result << " (" << X509_verify_cert_error_string(session->verify_result) << ")");
    }
    // write out the session
    FILE* fp = fopen((const char*)(sslsessionfile.c_str()),"w+");
    if (fp) {
      PEM_write_SSL_SESSION(fp, session);
      fclose(fp);
      DEBUG("info: ("<<__FUNCTION__<<") Session stored to " << sslsessionfile.c_str());
    }
  }

  if (!SSL_shutdown(ssl)) {
    SSL_shutdown(ssl);
  }

  if (ssl) {
    SSL_free(ssl);ssl = 0;
  }

  if (theFD>=0) {close(theFD);theFD=-1;}  
  SSLMutex.UnLock();  
  return;
}

/******************************************************************************/
/*               S e r v e r   O r i e n t e d   M e t h o d s                */
/******************************************************************************/



/*----------------------------------------------------------------------------*/
/* this helps to avoid memory leaks by strdup                                 */
/* we maintain a string hash to keep all used user ids/group ids etc.         */

char* 
STRINGSTORE(const char* __charptr__) {
  XrdOucString* yourstring;
  if (!__charptr__ ) return (char*)"";

  if ((yourstring = XrdSecProtocolssl::stringstore.Find(__charptr__))) {
    return (char*)yourstring->c_str();
  } else {
    XrdOucString* newstring = new XrdOucString(__charptr__);
    XrdSecProtocolssl::StoreMutex.Lock();
    XrdSecProtocolssl::stringstore.Add(__charptr__,newstring);
    XrdSecProtocolssl::StoreMutex.UnLock();
    return (char*)newstring->c_str();
  } 
}

/*----------------------------------------------------------------------------*/
void MyGRSTerrorLogFunc (char *lfile, int lline, int llevel, char *fmt, ...) {
  EPNAME("grst");
  va_list args;
  char fullmessage[4096];

  va_start(args, fmt);
  vsprintf(fullmessage,fmt,args);
  va_end(args);

  // just remove linefeeds
  XrdOucString sfullmessage = fullmessage;
  sfullmessage.replace("\n","");


  if (llevel <= GRST_LOG_WARNING) {
    TRACE(Authen," ("<< lfile << ":" << lline <<"): " << sfullmessage);    
  } else if (llevel <= GRST_LOG_INFO) {
    TRACE(Authen, " ("<< lfile << ":" << lline <<"): " << sfullmessage);    
  } else {
    DEBUG(" ("<< lfile << ":" << lline <<"): " << sfullmessage);
  }
}

/*----------------------------------------------------------------------------*/
void   
XrdSecProtocolssl::secServer(int theFD, XrdOucErrInfo      *error) {
  int err;

  char*    str;

  EPNAME("secServer");

  XrdSecsslSessionLock sessionlock; 

  // check if we should reload the store
  if ((time(NULL)-storeLoadTime) > 3600) {
    if (store) {
      TRACE(Authen,"Reloading X509 Store from " << sslcadir);
      X509_STORE_free(store);
      store = SSL_X509_STORE_create(NULL, sslcadir);
      X509_STORE_set_flags(XrdSecProtocolssl::store,0);
      storeLoadTime = time(NULL);
    } 
  }
  SSLMutex.Lock();  
  SSL_CTX_set_session_cache_mode(XrdSecProtocolssl::ctx, SSL_SESS_CACHE_BOTH | SSL_SESS_CACHE_NO_AUTO_CLEAR );

  ssl = SSL_new (ctx);                           
  SSL_set_purpose(ssl,X509_PURPOSE_ANY);

  SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_BOTH | SSL_SESS_CACHE_NO_AUTO_CLEAR );

  TRACE(Authen,"Info: ("<<__FUNCTION__<<") Session Cache has size: " <<SSL_CTX_sess_get_cache_size(ctx));

  if (!ssl) {
    fprintf(stderr,"Error: (%s) failed to create context\n",__FUNCTION__);
    TRACE(Authen,"Error: ("<<__FUNCTION__<<") failed to create context");
    exit(5);
  }

  SSL_set_fd (ssl, theFD);

  // the accept run's without mutex
  err = SSL_accept (ssl);                        

  if (err!=1) {
    long verifyresult = SSL_get_verify_result(ssl);
    if (verifyresult != X509_V_OK) {
      Fatal(error,X509_verify_cert_error_string(verifyresult),verifyresult);
      TRACE(Authen,"Error: ("<<__FUNCTION__<<") failed SSL_accept ");
    } else {
      Fatal(error,"do SSL_accept",-1);
      unsigned long lerr;
      while ((lerr=ERR_get_error())) {TRACE(Authen,"SSL Queue error: err=" << lerr << " msg=" <<
					  ERR_error_string(lerr, NULL));Fatal(error,ERR_error_string(lerr,NULL),-1);}
    }
    SSLMutex.UnLock();
    if (theFD>=0) {close(theFD); theFD=-1;}
    return;
  }
  
  SSL_SESSION* session = SSL_get_session(ssl);

  if (session) {
    char session_id[1024];
    for (int i=0; i< (int)session->session_id_length; i++) {
      sprintf(session_id+(i*2),"%02x",session->session_id[i]);
    }
    
    DEBUG("Info: ("<<__FUNCTION__<<") Session Id: "<< session_id << " Verify: " << session->verify_result << " (" << X509_verify_cert_error_string(session->verify_result) << ")");
    DEBUG("Info: ("<<__FUNCTION__<<") cache items             : " << SSL_CTX_sess_number(ctx));
    DEBUG("Info: ("<<__FUNCTION__<<") client connects         : " << SSL_CTX_sess_connect(ctx));
    DEBUG("Info: ("<<__FUNCTION__<<") client renegotiates     : " << SSL_CTX_sess_connect_renegotiate(ctx));
    DEBUG("Info: ("<<__FUNCTION__<<") client connect finished : " << SSL_CTX_sess_connect_good(ctx));
    DEBUG("Info: ("<<__FUNCTION__<<") server accepts          : " << SSL_CTX_sess_accept(ctx));
    DEBUG("Info: ("<<__FUNCTION__<<") server renegotiates     : " << SSL_CTX_sess_accept_renegotiate(ctx));
    DEBUG("Info: ("<<__FUNCTION__<<") server accepts finished : " << SSL_CTX_sess_accept_good(ctx));
    DEBUG("Info: ("<<__FUNCTION__<<") session cache hits      : " << SSL_CTX_sess_hits(ctx));
    DEBUG("Info: ("<<__FUNCTION__<<") session cache misses    : " << SSL_CTX_sess_misses(ctx));
    DEBUG("Info: ("<<__FUNCTION__<<") session cache timeouts  : " << SSL_CTX_sess_timeouts(ctx));
    DEBUG("Info: ("<<__FUNCTION__<<") callback cache hits     : " << SSL_CTX_sess_cb_hits(ctx));
    DEBUG("Info: ("<<__FUNCTION__<<") cache full overflows    : " << SSL_CTX_sess_cache_full(ctx) << " allowed: " << SSL_CTX_sess_get_cache_size(ctx));
    
  }
  /* get the grst_chain  */
  GRSTx509Chain *grst_chain = (GRSTx509Chain*) SSL_get_app_data(ssl);
  
  XrdOucString vomsroles="";
  XrdOucString clientdn="";
  
  if (grst_chain) {
    GRST_print_ssl_creds((void*) grst_chain);
    char* vr = GRST_get_voms_roles_and_free((void*) grst_chain);
    if (vr) {
      vomsroles = vr;
      free(vr);
    }
  }
  
  TRACE(Authen,"Authenticated with VOMS roles: "<<vomsroles);
  
  long verifyresult = SSL_get_verify_result(ssl);
  
  TRACE(Authen,"Verify result is = "<<verifyresult);
  
  /* Get the cipher - opt */
  
  DEBUG("SSL connection uses cipher " << SSL_get_cipher(ssl));
  
  /* Get client's certificate (note: beware of dynamic allocation) - opt */
  
  client_cert = SSL_get_peer_certificate (ssl);


  if (client_cert != NULL) {
    str = X509_NAME_oneline (X509_get_subject_name (client_cert), 0, 0);
    if (str) {
      TRACE(Authen,"client certificate subject: "<< str);
      clientdn = str;
      OPENSSL_free (str);
    } else {
      TRACE(Authen,"client certificate subject: none");
    }
    
    str = X509_NAME_oneline (X509_get_issuer_name  (client_cert), 0, 0);
    
    if (str) {
      TRACE(Authen,"client certificate issuer : "<<str);
      TRACE(Authen,"Setting dn="<<clientdn<<" roles="<<vomsroles);
      OPENSSL_free (str);
    } else {
      TRACE(Authen,"client certificate issuer : none");
      Fatal(error,"no client issuer",-1);
      if (theFD>=0) {close(theFD);theFD=-1;}
      SSLMutex.UnLock();
      return;
    }
  } else {
    TRACE(Authen,"Client does not have certificate.");
    Fatal(error,"no client certificate",-1);
    if (theFD>=0) {close(theFD);theFD=-1;}
    SSLMutex.UnLock();
    return;
  }

  // receive client proxy - if he send's one
  err = SSL_read(ssl,proxyBuff, sizeof(proxyBuff));
  if (err>0) {
    TRACE(Authen,"Recevied proxy buffer with " << err << " bytes");
    Entity.endorsements = proxyBuff;
    err = SSL_write(ssl,"OK\n",3);
    if (err!=3) {
      Fatal(error,"could not send end of handshake OK",-1);
      if (theFD>=0) {close(theFD);theFD=-1;}
      SSLMutex.UnLock();
      return;
    }
    // pseudo read to let the client close the connection
    char dummy[1];
    err = SSL_read(ssl,dummy, sizeof(dummy));
  } else {
    TRACE(Authen,"Received no proxy");
  }

  SSL_shutdown(ssl);

  strncpy(Entity.prot,"ssl", sizeof(Entity.prot));

  /*----------------------------------------------------------------------------*/
  /* mapping interface                                                          */
  /*----------------------------------------------------------------------------*/
     
  if (!mapuser && !mapcerncertificates) { 
    // no mapping put the DN
    Entity.name = strdup(clientdn.c_str());
  } else {
    bool mapped=false;
    // map user from grid map file
    if (mapcerncertificates) {
      // map from CERN DN
      if ( (mapcerncertificates) && (clientdn.beginswith("/DC=ch/DC=cern/OU=Organic Units/OU=Users/CN="))) {
	XrdOucString certsubject = clientdn;
	certsubject.erasefromstart(strlen("/DC=ch/DC=cern/OU=Organic Units/OU=Users/CN="));
	int pos=certsubject.find('/');                               
	if (pos != STR_NPOS)                                         
	  certsubject.erase(pos);			  	        
	Entity.name = strdup(certsubject.c_str());
	mapped=true;
	TRACE(Authen,"Found CERN certificate - mapping to AFS account " << certsubject);
      }
    }
    if (!mapped) {
      if (mapuser) {
	// treatment of old proxy
	XrdOucString certsubject = clientdn;
	certsubject.replace("/CN=proxy","");                           
	// treatment of new proxy - leave only the first CN=, cut the rest
	int pos = certsubject.find("CN=");
	int pos2 = certsubject.find("/",pos);
	if (pos2>0) certsubject.erase(pos2);
	XrdOucString* gridmaprole;                                     
	ReloadGridMapFile();
	GridMapMutex.Lock();                             
	
	if ((gridmaprole = gridmapstore.Find(certsubject.c_str()))) { 
	  Entity.name = strdup(gridmaprole->c_str());      
	  Entity.role = 0;
	}  else {
	  Entity.name = (char*)"nobody";
	  Entity.role = 0;
	  Fatal(error,"user cannot be mapped",-1);      
	}
	GridMapMutex.UnLock();
      } else {
	Entity.name = (char*)"nobody";
	Entity.role = 0;
	Fatal(error,"user cannot be mapped",-1);      
      }
    }
  }
  
  
  if (!mapgroup) {
    if (vomsroles.length()) {
      // no mapping put the VOMS groups and role
      Entity.grps = strdup(vomsroles.c_str());
      
      XrdOucString vomsrole = vomsroles.c_str();
      
      if (vomsroles.length()) {
	int dp = vomsrole.find(":");
	if (dp != STR_NPOS) {
	  vomsrole.assign(vomsroles,0,dp-1);
	}
	Entity.role = strdup(vomsrole.c_str());
      } else {
	Entity.role = strdup("");
      }
    } else {
      // map the group from the passwd/group file
      struct passwd* pwd;
      struct group*  grp;
      StoreMutex.Lock();
      if ( (pwd = getpwnam(Entity.name)) && (grp = getgrgid(pwd->pw_gid))) {
	Entity.grps   = strdup(grp->gr_name);
	Entity.role   = strdup(grp->gr_name);
      }
      StoreMutex.UnLock();
    }
  } else {
    // map groups & role from VOMS mapfile
    XrdOucString defaultgroup="";                                     
    XrdOucString allgroups="";  
    if (VomsMapGroups(vomsroles.c_str(), allgroups,defaultgroup)) {
      if (!strcmp(allgroups.c_str(),":")) {
	// map the group from the passwd/group file
	struct passwd* pwd;
	struct group*  grp;
	StoreMutex.Lock();
	if ( (pwd = getpwnam(Entity.name)) && (grp = getgrgid(pwd->pw_gid))) {
	  allgroups    = grp->gr_name;
	  defaultgroup = grp->gr_name;
	}
	StoreMutex.UnLock();
      }
      Entity.grps   = strdup(allgroups.c_str());
      Entity.role   = strdup(defaultgroup.c_str());
    } else {
      Fatal(error,"incomplete VOMS mapping",-1);
    }
  }

  // ev. export proxy
  if (sslproxyexportdir && Entity.endorsements) {
    StoreMutex.Lock();
    // get the UID of the entity name
    struct passwd* pwd;
    XrdOucString outputproxy = sslproxyexportdir; outputproxy+="/x509up_u"; 
    if ( (pwd = getpwnam(Entity.name)) ) {
      outputproxy += (int)pwd->pw_uid;
    } else {
      outputproxy += Entity.name;
    }
    int fd = open (outputproxy.c_str(),O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);
    if (fd>0) {
      if ( ((int)write(fd,Entity.endorsements,strlen(Entity.endorsements))) != (int)strlen(Entity.endorsements)) {
	Fatal(error,"cannot export(write) user proxy",-1);
      } else {
	TRACE(Identity,"Exported proxy buffer of " << Entity.name << " to file " << outputproxy.c_str());
      }
      close(fd);
    } else {
      Fatal(error,"cannot export(open) user proxy",-1);
    }
    StoreMutex.UnLock();
  }


  TRACE(Identity,"[usermapping] name=|" << Entity.name << "| role=|" << Entity.role << "| grps=|"<< Entity.grps << "| DN=|" << clientdn.c_str() << "| VOMS=|" << vomsroles.c_str() << "|");

  if (ssl) {
    SSL_free(ssl);ssl = 0;
  }
  if (theFD>=0) {close(theFD); theFD=-1;}       
  SSLMutex.UnLock();

  return;
}

int 
XrdSecProtocolssl::GenerateSession(const SSL* ssl, unsigned char *id, unsigned int *id_len) {
  EPNAME("GenerateSession");
  unsigned int count = 0;
  do      {
    RAND_pseudo_bytes(id, *id_len);
    /* Prefix the session_id with the required prefix. NB: If our
     * prefix is too long, clip it - but there will be worse effects
     * anyway, eg. the server could only possibly create 1 session
     * ID (ie. the prefix!) so all future session negotiations will
     * fail due to conflicts. */
    memcpy(id, "xrootdssl",
	   (strlen("xrootdssl") < *id_len) ?
	   strlen("xrootdssl") : *id_len);
   TRACE(Authen,"Generated SSID **********************");
  }
  while(SSL_has_matching_session_id(ssl, id, *id_len) &&
	(++count < MAX_SESSION_ID_ATTEMPTS));
  if(count >= MAX_SESSION_ID_ATTEMPTS)
    return 0;
  return 1;
}

int 
XrdSecProtocolssl::NewSession(SSL* ssl, SSL_SESSION *session) {
  EPNAME("NewSession");
  TRACE(Authen,"Creating new Session");
  char session_id[1024];
  for (int i=0; i< (int)session->session_id_length; i++) {
    sprintf(session_id+(i*2),"%02x",session->session_id[i]);
  }
  DEBUG("Info: ("<<__FUNCTION__<<") Session Id: "<< session_id << " Verify: " << session->verify_result << " (" << X509_verify_cert_error_string(session->verify_result) << ")");
  
  SSL_set_timeout(session, sslsessionlifetime);
  return 0;
}

/*----------------------------------------------------------------------------*/
void 
XrdSecProtocolssl::ReloadGridMapFile()
{
  EPNAME("ReloadGridMapFile");

  static time_t         GridMapMtime=0;
  static time_t         GridMapCheckTime=0;
  int now = time(NULL);

  if ((!GridMapCheckTime) || ((now >GridMapCheckTime + 60)) ) {
    // load it for the first time or again
    struct stat buf;
    if (!::stat(gridmapfile,&buf)) {
      if (buf.st_mtime != GridMapMtime) {
	GridMapMutex.Lock();
	// store the last modification time
	GridMapMtime = buf.st_mtime;
	// store the current time of the check
	GridMapCheckTime = now;
	// dump the current table
	gridmapstore.Purge();
	// open the gridmap file
	FILE* mapin = fopen(gridmapfile,"r");
	if (!mapin) {
	  // error no grid map possible
	  TRACE(Authen,"Unable to open gridmapfile " << XrdOucString(gridmapfile) << " - no mapping!");
	} else {
	  char userdnin[4096];
	  char usernameout[4096];
	  int nitems;
	  // parse it
	  while ( (nitems = fscanf(mapin,"\"%[^\"]\" %s\n", userdnin,usernameout)) == 2) {
	    XrdOucString dn = userdnin;
	    dn.replace("\"","");
	    // leave only the first CN=, cut the rest
	    int pos = dn.find("CN=");
	    int pos2 = dn.find("/",pos);
	    if (pos2>0) dn.erase(pos2);

	    if (!gridmapstore.Find(dn.c_str())) {
	      gridmapstore.Add(dn.c_str(), new XrdOucString(usernameout));
	      TRACE(Authen, "gridmapfile Mapping Added: " << dn.c_str() << " |=> " << usernameout);
	    }
	  }
	  fclose(mapin);
	}
	GridMapMutex.UnLock();
      } else {
	// the file didn't change, we don't do anything
      }
    } else {
      //      printf("Gridmapfile %s\n",gridmapfile);
      TRACE(Authen,"Unable to stat gridmapfile " << XrdOucString(gridmapfile) << " - no mapping!");
    }
  }
}

/*----------------------------------------------------------------------------*/

void 
XrdSecProtocolssl::ReloadVomsMapFile()
{
  EPNAME("ReloadVomsMapFile");

  static time_t         VomsMapMtime=0;
  static time_t         VomsMapCheckTime=0;
  int now = time(NULL);

  if ((!VomsMapCheckTime) || ((now >VomsMapCheckTime + 60 )) ) {
    // load it for the first time or again
    struct stat buf;
    if (!::stat(vomsmapfile,&buf)) {
      if (buf.st_mtime != VomsMapMtime) {
	VomsMapMutex.Lock();
	// store the last modification time
	VomsMapMtime = buf.st_mtime;
	// store the current time of the check
	VomsMapCheckTime = now;
	// dump the current table
	vomsmapstore.Purge();
	// open the vomsmap file
	FILE* mapin = fopen(vomsmapfile,"r");
	if (!mapin) {
	  // error no voms map possible
	  TRACE(Authen,"Unable to open vomsmapfile " << XrdOucString(vomsmapfile) << " - no mapping!");
	} else {
	  char userdnin[4096];
	  char usernameout[4096];
	  int nitems;
	  // parse it
	  while ( (nitems = fscanf(mapin,"\"%[^\"]\" %s\n", userdnin,usernameout)) == 2) {
	    XrdOucString dn = userdnin;
	    dn.replace("\"","");
	    if (!vomsmapstore.Find(dn.c_str())) {
	      vomsmapstore.Add(dn.c_str(), new XrdOucString(usernameout));
	      TRACE(Authen,"vomsmapfile Mapping Added: " << dn.c_str() << " |=> " << usernameout);
	    }
	  }
	  fclose(mapin);
	}
	VomsMapMutex.UnLock();
      } else {
	// the file didn't change, we don't do anything
      }
    } else {
      //      printf("Vomsmapfile %s\n",vomsmapfile);
      TRACE(Authen,"Unable to stat vomsmapfile " << XrdOucString(vomsmapfile) << " - no mapping!");
    }
  }
}

/*----------------------------------------------------------------------------*/

bool
XrdSecProtocolssl::VomsMapGroups(const char* groups, XrdOucString& allgroups, XrdOucString& defaultgroup) 
{
  EPNAME("VomsMapGroups");
  // loop over all VOMS groups and replace them according to the mapping
  XrdOucString vomsline = groups;
  allgroups = ":";
  defaultgroup = "";
  vomsline.replace(":","\n");
  XrdOucTokenizer vomsgroups((char*)vomsline.c_str());
  const char* stoken;
  int ntoken=0;
  XrdOucString* vomsmaprole;                                     
  while( (stoken = vomsgroups.GetLine())) {
    if ((vomsmaprole = XrdSecProtocolssl::vomsmapstore.Find(stoken))) { 
      allgroups += vomsmaprole->c_str();
      allgroups += ":";
      if (ntoken == 0) {
	defaultgroup = vomsmaprole->c_str();
      }
      ntoken++;
    } else {
      TRACE(Authen,"No VOMS mapping found for " << XrdOucString(stoken));
      return false;
    }
  }
  return true;
}

/******************************************************************************/
/*                X r d S e c p r o t o c o l u n i x I n i t                 */
/******************************************************************************/
  
extern "C"
{
char  *XrdSecProtocolsslInit(const char     mode,
                              const char    *parms,
                              XrdOucErrInfo *erp)
{
  EPNAME("ProtocolsslInit");
  // Initiate error logging and tracing
  XrdSecProtocolssl::eDest.logger(&XrdSecProtocolssl::Logger);
  
  GRSTerrorLogFunc = &MyGRSTerrorLogFunc;

  // create the tracer
  SSLxTrace = new XrdOucTrace(&XrdSecProtocolssl::eDest);
  // read the configuration options
  if (mode == 's') {
    XrdSecProtocolssl::isServer = 1;
    if (parms){
      // Duplicate the parms
      char parmbuff[1024];
      strlcpy(parmbuff, parms, sizeof(parmbuff));
      //
      // The tokenizer
      XrdOucTokenizer inParms(parmbuff);
      char *op;

      while (inParms.GetLine()) { 
	while ((op = inParms.GetToken())) {
	  if (!strncmp(op, "-d:",3)) {
	    XrdSecProtocolssl::debug = atoi(op+3);
	  } else if (!strncmp(op, "-cadir:",7)) {
	    XrdSecProtocolssl::sslcadir = strdup(op+7);
	  } else if (!strncmp(op, "-vomsdir:",6)) {
	    XrdSecProtocolssl::sslvomsdir = strdup(op+6);
	  } else if (!strncmp(op, "-cert:",6)) {
	    XrdSecProtocolssl::sslcertfile = strdup(op+6);
	  } else if (!strncmp(op, "-key:",5)) {
	    XrdSecProtocolssl::sslkeyfile = strdup(op+5);
	  } else if (!strncmp(op, "-ca:",4)) {
	    XrdSecProtocolssl::verifydepth = atoi(op+4);
	  } else if (!strncmp(op, "-t:",3)) {
	    XrdSecProtocolssl::sslsessionlifetime = atoi(op+3);
	  } else if (!strncmp(op, "-export:",8)) {
	    XrdSecProtocolssl::sslproxyexportdir = strdup(op+8);
	  } else if (!strncmp(op, "-gridmapfile:",13)) {
	    XrdSecProtocolssl::gridmapfile = strdup(op+13);
	  } else if (!strncmp(op, "-vomsmapfile:",13)) {
	    XrdSecProtocolssl::vomsmapfile = strdup(op+13);
	  } else if (!strncmp(op, "-mapuser:",9)) {
	    XrdSecProtocolssl::mapuser = (bool) atoi(op+9);
	  } else if (!strncmp(op, "-mapgroup:",10)) {
	    XrdSecProtocolssl::mapgroup = (bool) atoi(op+10);
	  } else if (!strncmp(op, "-mapcernuser:",13)) {
	    XrdSecProtocolssl::mapcerncertificates = (bool) atoi(op+13);
	  }
	}
      }
    }
  }

  if (mode == 'c') {
    // default the cert/key file to the standard proxy locations
    char proxyfile[1024];
    sprintf(proxyfile,"/tmp/x509up_u%d",(int)geteuid());

    XrdSecProtocolssl::sslcertfile = strdup(proxyfile);
    XrdSecProtocolssl::sslkeyfile  = strdup(proxyfile);

    char *nossl = getenv("XrdSecNoSSL");
    if (nossl) {
      erp->setErrInfo(ENOENT,"");
      return 0;
    }

    char *cenv = getenv("XrdSecDEBUG");
    // debug
    if (cenv)
      if (cenv[0] >= 49 && cenv[0] <= 58) XrdSecProtocolssl::debug = atoi(cenv);
    
    // directory with CA certificates
    cenv = getenv("XrdSecSSLCADIR");
    if (cenv)
      XrdSecProtocolssl::sslcadir = strdup(cenv);
    else {
      // accept X509_CERT_DIR 
      cenv = getenv("X509_CERT_DIR");
      if (cenv) {
	XrdSecProtocolssl::sslcadir = strdup(cenv);
      }
    }
    // directory with VOMS certificates
    cenv = getenv("XrdSecSSLVOMSDIR");
    if (cenv)
      XrdSecProtocolssl::sslvomsdir = strdup(cenv);
    
    // file with user cert
    cenv = getenv("XrdSecSSLUSERCERT");
    if (cenv) {
      XrdSecProtocolssl::sslcertfile = strdup(cenv);  
    } else {
      // accept X509_USER_CERT
      cenv = getenv("X509_USER_CERT");
      if (cenv) {
	XrdSecProtocolssl::sslkeyfile = strdup(cenv);
      } else {
	// accept X509_USER_PROXY
	cenv = getenv("X509_USER_PROXY");
	if (cenv) {
	  XrdSecProtocolssl::sslkeyfile = strdup(cenv);
	}
      }
    }
    
    // file with user key
    cenv = getenv("XrdSecSSLUSERKEY");
    if (cenv) {
      XrdSecProtocolssl::sslkeyfile = strdup(cenv);
    } else {
      // accept X509_USER_KEY
      cenv = getenv("X509_USER_KEY");
      if (cenv) {
	XrdSecProtocolssl::sslkeyfile = strdup(cenv);
      } else {
	// accept X509_USER_PROXY
	cenv = getenv("X509_USER_PROXY");
	if (cenv) {
	  XrdSecProtocolssl::sslkeyfile = strdup(cenv);
	}
      }
    }
    // verify depth
    cenv = getenv("XrdSecSSLVERIFYDEPTH");
    if (cenv)
      XrdSecProtocolssl::verifydepth = atoi(cenv);
    
    // proxy forwarding
    cenv = getenv("XrdSecSSLPROXYFORWARD");
    if (cenv)
      XrdSecProtocolssl::forwardProxy = atoi(cenv);

    // ssl session reuse
    cenv = getenv("XrdSecSSLSESSION");
    if (cenv)
      XrdSecProtocolssl::allowSessions = atoi(cenv);
  }

  if (XrdSecProtocolssl::debug >= 4) {
    SSLxTrace->What = TRACE_ALL;
  } else if (XrdSecProtocolssl::debug == 3 ) {
    SSLxTrace->What |= TRACE_Authen;
    SSLxTrace->What |= TRACE_Debug;
    SSLxTrace->What |= TRACE_Identity;
  } else if (XrdSecProtocolssl::debug == 2) {
    SSLxTrace->What = TRACE_Debug;
  } else if (XrdSecProtocolssl::debug == 1) {
    SSLxTrace->What = TRACE_Identity;
  }

  if (XrdSecProtocolssl::isServer) {
    TRACE(Authen,"====> debug         = " << XrdSecProtocolssl::debug);
    TRACE(Authen,"====> cadir         = " << XrdSecProtocolssl::sslcadir);
    TRACE(Authen,"====> keyfile       = " << XrdSecProtocolssl::sslkeyfile);
    TRACE(Authen,"====> certfile      = " << XrdSecProtocolssl::sslcertfile);
    TRACE(Authen,"====> verify depth  = " << XrdSecProtocolssl::verifydepth);
    TRACE(Authen,"====> sess.lifetime = " << XrdSecProtocolssl::sslsessionlifetime);
    TRACE(Authen,"====> gridmapfile   = " << XrdSecProtocolssl::gridmapfile);
    TRACE(Authen,"====> vomsmapfile   = " << XrdSecProtocolssl::vomsmapfile);
    TRACE(Authen,"====> mapuser       = " << XrdSecProtocolssl::mapuser);
    TRACE(Authen,"====> mapgroup      = " << XrdSecProtocolssl::mapgroup);
    TRACE(Authen,"====> mapcernuser   = " << XrdSecProtocolssl::mapcerncertificates);
  } else {
    if (XrdSecProtocolssl::debug) {
      TRACE(Authen,"====> debug         = " << XrdSecProtocolssl::debug);
      TRACE(Authen,"====> cadir         = " << XrdSecProtocolssl::sslcadir);
      TRACE(Authen,"====> keyfile       = " << XrdSecProtocolssl::sslkeyfile);
      TRACE(Authen,"====> certfile      = " << XrdSecProtocolssl::sslcertfile);
      TRACE(Authen,"====> verify depth  = " << XrdSecProtocolssl::verifydepth);
    }
  }

  if (XrdSecProtocolssl::isServer) {
    // check if we can map with a grid map file
    if (XrdSecProtocolssl::mapuser && access(XrdSecProtocolssl::gridmapfile,R_OK)) {
      fprintf(stderr,"Error: (%s) cannot access gridmapfile %s\n",__FUNCTION__,XrdSecProtocolssl::gridmapfile);
      TRACE(Authen,"Error: cannot access gridmapfile "<< XrdOucString(XrdSecProtocolssl::gridmapfile));
      return 0;
    }
    // check if we can map with a voms map file
    if (XrdSecProtocolssl::mapgroup && access(XrdSecProtocolssl::vomsmapfile,R_OK)) {
      fprintf(stderr,"Error: (%s) cannot access vomsmapfile %s\n",__FUNCTION__,XrdSecProtocolssl::vomsmapfile);
      TRACE(Authen,"Error: cannot access vomsmapfile "<< XrdOucString(XrdSecProtocolssl::vomsmapfile));
      return 0;
    }
    // check if we can export proxies
    if (XrdSecProtocolssl::sslproxyexportdir && access(XrdSecProtocolssl::sslproxyexportdir,R_OK | W_OK)) {
      fprintf(stderr,"Error: (%s) cannot read/write proxy export directory %s\n",__FUNCTION__,XrdSecProtocolssl::sslproxyexportdir);
      TRACE(Authen,"Error: cannot access proxyexportdir "<< XrdOucString(XrdSecProtocolssl::sslproxyexportdir));
      return 0;
    }
  }

  if (XrdSecProtocolssl::isServer) {
    SSL_METHOD *meth;
    // initialize openssl until the context is created 
    SSL_load_error_strings();
    SSLeay_add_ssl_algorithms();
    
    meth = (SSL_METHOD*)SSLv23_server_method();

    XrdSecProtocolssl::ctx = SSL_CTX_new (meth);
    if (!XrdSecProtocolssl::ctx) {
      ERR_print_errors_fp(stderr);
      return 0;
    }
      
    if (SSL_CTX_use_certificate_file(XrdSecProtocolssl::ctx, XrdSecProtocolssl::sslcertfile, SSL_FILETYPE_PEM) <= 0) {
      ERR_print_errors_fp(stderr);
      return 0;
    }
    
    if (SSL_CTX_use_PrivateKey_file(XrdSecProtocolssl::ctx,XrdSecProtocolssl:: sslkeyfile, SSL_FILETYPE_PEM) <= 0) {
      ERR_print_errors_fp(stderr);
      return 0;
    }
      
    if (!SSL_CTX_check_private_key(XrdSecProtocolssl::ctx)) {
      fprintf(stderr,"Private key does not match the certificate public key\n");
      return 0;
    }
    
    SSL_CTX_load_verify_locations(XrdSecProtocolssl::ctx, NULL,XrdSecProtocolssl::sslcadir);  
    
    // create the store
    if (!XrdSecProtocolssl::store) {
      DEBUG("Created SSL CRL store: " << XrdSecProtocolssl::store);
      XrdSecProtocolssl::store = SSL_X509_STORE_create(NULL,XrdSecProtocolssl::sslcadir);
      X509_STORE_set_flags(XrdSecProtocolssl::store,0);
      XrdSecProtocolssl::storeLoadTime = time(NULL);
    }
    
    XrdSecProtocolssl::ctx->verify_mode = SSL_VERIFY_PEER | SSL_VERIFY_CLIENT_ONCE|SSL_VERIFY_FAIL_IF_NO_PEER_CERT;

    grst_cadir   = XrdSecProtocolssl::sslcadir;
    grst_vomsdir = XrdSecProtocolssl::sslvomsdir;
    grst_depth   = XrdSecProtocolssl::verifydepth;
    
    SSL_CTX_set_cert_verify_callback(XrdSecProtocolssl::ctx,
				     GRST_verify_cert_wrapper,
				     (void *) NULL);
    
    SSL_CTX_set_verify(XrdSecProtocolssl::ctx, XrdSecProtocolssl::ctx->verify_mode,GRST_callback_SSLVerify_wrapper);
    SSL_CTX_set_verify_depth(XrdSecProtocolssl::ctx, XrdSecProtocolssl::verifydepth + 1);

    if(!SSL_CTX_set_generate_session_id(XrdSecProtocolssl::ctx, XrdSecProtocolssl::GenerateSession)) {
      TRACE(Authen,"Cannot set session generator");
      return 0;
    }

    //    SSL_CTX_set_quiet_shutdown(XrdSecProtocolssl::ctx,1);
    SSL_CTX_set_options(XrdSecProtocolssl::ctx,  SSL_OP_ALL | SSL_OP_NO_SSLv2);
    SSL_CTX_set_session_cache_mode(XrdSecProtocolssl::ctx, SSL_SESS_CACHE_BOTH | SSL_SESS_CACHE_NO_AUTO_CLEAR );
    SSL_CTX_set_session_id_context(XrdSecProtocolssl::ctx,(const unsigned char*) XrdSecProtocolssl::SessionIdContext,  strlen(XrdSecProtocolssl::SessionIdContext));
    SSL_CTX_sess_set_new_cb(XrdSecProtocolssl::ctx, XrdSecProtocolssl::NewSession);
  }
  return (char *)"";
}
}

/******************************************************************************/
/*              X r d S e c P r o t o c o l u n i x O b j e c t               */
/******************************************************************************/
  
extern "C"
{
XrdSecProtocol *XrdSecProtocolsslObject(const char              mode,
                                         const char             *hostname,
                                         const struct sockaddr  &netaddr,
                                         const char             *parms,
                                               XrdOucErrInfo    *erp)
{
   XrdSecProtocolssl *prot;

// Return a new protocol object
//
   if (!(prot = new XrdSecProtocolssl(hostname, &netaddr)))
      {const char *msg = "Secssl: Insufficient memory for protocol.";
       if (erp) erp->setErrInfo(ENOMEM, msg);
          else cerr <<msg <<endl;
       return (XrdSecProtocol *)0;
      }

// All done
//
   return prot;
}
}
